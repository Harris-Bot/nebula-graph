/* Copyright (c) 2020 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#include <thrift/lib/cpp/util/EnumUtils.h>
#include "validator/TraversalValidator.h"
#include "common/expression/VariableExpression.h"
#include "util/SchemaUtil.h"

namespace nebula {
namespace graph {

Status TraversalValidator::validateStarts(const VerticesClause* clause, Starts& starts) {
    if (clause == nullptr) {
        return Status::SemanticError("From clause nullptr.");
    }
    if (clause->isRef()) {
        auto* src = clause->ref();
        if (src->kind() != Expression::Kind::kInputProperty
                && src->kind() != Expression::Kind::kVarProperty) {
            return Status::SemanticError(
                    "`%s', Only input and variable expression is acceptable"
                    " when starts are evaluated at runtime.", src->toString().c_str());
        }
        starts.fromType = src->kind() == Expression::Kind::kInputProperty ? kPipe : kVariable;
        auto type = deduceExprType(src);
        if (!type.ok()) {
            return type.status();
        }
        auto vidType = space_.spaceDesc.vid_type_ref().value().get_type();
        if (type.value() != SchemaUtil::propTypeToValueType(vidType)) {
            std::stringstream ss;
            ss << "`" << src->toString() << "', the srcs should be type of "
                << apache::thrift::util::enumNameSafe(vidType) << ", but was`"
                << type.value() << "'";
            return Status::SemanticError(ss.str());
        }
        starts.originalSrc = src;
        auto* propExpr = static_cast<PropertyExpression*>(src);
        if (starts.fromType == kVariable) {
            starts.userDefinedVarName = propExpr->sym();
            userDefinedVarNameList_.emplace(starts.userDefinedVarName);
        }
        starts.firstBeginningSrcVidColName = propExpr->prop();
    } else {
        auto vidList = clause->vidList();
        QueryExpressionContext ctx;
        for (auto* expr : vidList) {
            if (!evaluableExpr(expr)) {
                return Status::SemanticError("`%s' is not an evaluable expression.",
                        expr->toString().c_str());
            }
            auto vid = expr->eval(ctx(nullptr));
            auto vidType = space_.spaceDesc.vid_type_ref().value().get_type();
            if (!SchemaUtil::isValidVid(vid, vidType)) {
                std::stringstream ss;
                ss << "Vid should be a " << apache::thrift::util::enumNameSafe(vidType);
                return Status::SemanticError(ss.str());
            }
            starts.vids.emplace_back(std::move(vid));
        }
    }
    return Status::OK();
}

Status TraversalValidator::validateOver(const OverClause* clause, Over& over) {
    if (clause == nullptr) {
        return Status::SemanticError("Over clause nullptr.");
    }

    over.direction = clause->direction();
    auto* schemaMng = qctx_->schemaMng();
    if (clause->isOverAll()) {
        auto allEdgeStatus = schemaMng->getAllEdge(space_.id);
        NG_RETURN_IF_ERROR(allEdgeStatus);
        auto edges = std::move(allEdgeStatus).value();
        if (edges.empty()) {
            return Status::SemanticError("No edge type found in space `%s'",
                    space_.name.c_str());
        }
        for (auto edge : edges) {
            auto edgeType = schemaMng->toEdgeType(space_.id, edge);
            if (!edgeType.ok()) {
                return Status::SemanticError("`%s' not found in space [`%s'].",
                        edge.c_str(), space_.name.c_str());
            }
            over.edgeTypes.emplace_back(edgeType.value());
        }
        over.allEdges = std::move(edges);
        over.isOverAll = true;
    } else {
        auto edges = clause->edges();
        for (auto* edge : edges) {
            auto edgeName = *edge->edge();
            auto edgeType = schemaMng->toEdgeType(space_.id, edgeName);
            if (!edgeType.ok()) {
                return Status::SemanticError("%s not found in space [%s].",
                        edgeName.c_str(), space_.name.c_str());
            }
            over.edgeTypes.emplace_back(edgeType.value());
        }
    }
    return Status::OK();
}

Status TraversalValidator::validateStep(const StepClause* clause, StepClause& step) {
    if (clause == nullptr) {
        return Status::SemanticError("Step clause nullptr.");
    }
    step = *clause;
    if (clause->isMToN()) {
        if (step.mSteps() == 0) {
            step.setMSteps(1);
        }
        if (step.nSteps() < step.mSteps()) {
            return Status::SemanticError(
                "`%s', upper bound steps should be greater than lower bound.",
                step.toString().c_str());
        }
    }
    return Status::OK();
}

PlanNode* TraversalValidator::projectDstVidsFromGN(PlanNode* gn, const std::string& outputVar) {
    Project* project = nullptr;
    auto* columns = qctx_->objPool()->add(new YieldColumns());
    auto* column = new YieldColumn(new EdgePropertyExpression("*", kDst), kVid);
    columns->addColumn(column);

    project = Project::make(qctx_, gn, columns);
    VLOG(1) << project->outputVar();

    auto* dedupDstVids = Dedup::make(qctx_, project);
    dedupDstVids->setOutputVar(outputVar);
    dedupDstVids->setColNames(project->colNames());
    return dedupDstVids;
}

void TraversalValidator::buildConstantInput(Starts& starts, std::string& startVidsVar) {
    startVidsVar = vctx_->anonVarGen()->getVar();
    DataSet ds;
    ds.colNames.emplace_back(kVid);
    for (auto& vid : starts.vids) {
        Row row;
        row.values.emplace_back(vid);
        ds.rows.emplace_back(std::move(row));
    }
    qctx_->ectx()->setResult(startVidsVar, ResultBuilder().value(Value(std::move(ds))).finish());

    starts.src = new VariablePropertyExpression(startVidsVar, kVid);
    qctx_->objPool()->add(starts.src);
}

PlanNode* TraversalValidator::buildRuntimeInput(Starts& starts, PlanNode*& projectStartVid) {
    auto pool = qctx_->objPool();
    auto* columns = pool->add(new YieldColumns());
    auto* column = new YieldColumn(starts.originalSrc->clone().release(), kVid);
    columns->addColumn(column);

    auto* project = Project::make(qctx_, nullptr, columns);
    if (starts.fromType == kVariable) {
        project->setInputVar(starts.userDefinedVarName);
    }
    VLOG(1) << project->outputVar() << " input: " << project->inputVar();
    starts.src = pool->add(new InputPropertyExpression(kVid));

    auto* dedupVids = Dedup::make(qctx_, project);

    projectStartVid = project;
    return dedupVids;
}

Expression* TraversalValidator::buildNStepLoopCondition(uint32_t steps) const {
    VLOG(1) << "steps: " << steps;
    // ++loopSteps{0} <= steps
    qctx_->ectx()->setValue(loopSteps_, 0);
    return new RelationalExpression(
        Expression::Kind::kRelLE,
        new UnaryExpression(Expression::Kind::kUnaryIncr, new VariableExpression(loopSteps_)),
        new ConstantExpression(static_cast<int32_t>(steps)));
}

// $var == empty || size($var) != 0
Expression* TraversalValidator::buildExpandEndCondition(const std::string &lastStepResult) const {
    auto* eqEmpty = ExpressionUtils::Eq(new VariableExpression(lastStepResult),
                                        new ConstantExpression(Value()));

    auto* args = new ArgumentList();
    args->addArgument(std::make_unique<VariableExpression>(lastStepResult));
    auto* neZero = new RelationalExpression(Expression::Kind::kRelNE,
                                            new FunctionCallExpression("size", args),
                                            new ConstantExpression(0));
    return ExpressionUtils::Or(eqEmpty, neZero);
}

}  // namespace graph
}  // namespace nebula
