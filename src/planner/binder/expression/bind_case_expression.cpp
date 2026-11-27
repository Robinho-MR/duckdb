#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/function/scalar/case_with_operand.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

//! The comparisons of a check that was expanded from a simple CASE: a comparison, or an OR of comparisons
static bool GetSimpleCaseComparisons(CaseCheck &check, vector<reference<ComparisonExpression>> &comparisons) {
	auto &when_expr = *check.when_expr;
	if (when_expr.GetExpressionClass() == ExpressionClass::COMPARISON) {
		comparisons.push_back(when_expr.Cast<ComparisonExpression>());
		return true;
	}
	if (when_expr.GetExpressionClass() != ExpressionClass::CONJUNCTION ||
	    when_expr.GetExpressionType() != ExpressionType::CONJUNCTION_OR) {
		return false;
	}
	for (auto &child : when_expr.Cast<ConjunctionExpression>().GetChildrenMutable()) {
		if (child->GetExpressionClass() != ExpressionClass::COMPARISON) {
			return false;
		}
		comparisons.push_back(child->Cast<ComparisonExpression>());
	}
	return !comparisons.empty();
}

//! A simple CASE is expanded into a CASE with a copy of the operand in every comparison. This is not equivalent if
//! the operand is volatile, as the operand is then evaluated once per comparison instead of once. Returns the bound
//! operand if the CASE has to evaluate the operand once, and the comparisons of every check.
unique_ptr<Expression>
ExpressionBinder::TryBindSharedCaseOperand(CaseExpression &expr, idx_t depth,
                                           vector<vector<reference<ComparisonExpression>>> &comparisons) {
	if (!expr.IsSimpleCase()) {
		return nullptr;
	}
	for (auto &check : expr.CaseChecksMutable()) {
		comparisons.emplace_back();
		if (!GetSimpleCaseComparisons(check, comparisons.back())) {
			return nullptr;
		}
	}
	if (comparisons.empty()) {
		return nullptr;
	}
	// all comparisons must still have the same operand
	auto &operand = comparisons[0][0].get().Left();
	for (auto &check_comparisons : comparisons) {
		for (auto &comparison : check_comparisons) {
			if (!comparison.get().Left().Equals(operand)) {
				return nullptr;
			}
		}
	}
	// bind a copy of the operand: a failure is reported when binding the checks
	auto operand_copy = operand.Copy();
	ErrorData operand_error;
	auto bound_operand = BindChild(operand_copy, depth, operand_error);
	if (operand_error.HasError() || !bound_operand || !bound_operand->IsVolatile()) {
		return nullptr;
	}
	return bound_operand;
}

BindResult ExpressionBinder::BindExpression(CaseExpression &expr, idx_t depth) {
	// first try to bind the children of the case expression
	ErrorData error;
	vector<vector<reference<ComparisonExpression>>> comparisons;
	auto shared_operand = TryBindSharedCaseOperand(expr, depth, comparisons);
	vector<pair<unique_ptr<Expression>, unique_ptr<Expression>>> checks;
	for (auto &check : expr.CaseChecksMutable()) {
		unique_ptr<Expression> when_expr;
		if (shared_operand) {
			// compare the right-hand sides to the (single) value of the operand
			vector<unique_ptr<Expression>> conditions;
			for (auto &comparison : comparisons[checks.size()]) {
				auto rhs = BindChild(comparison.get().RightMutable(), depth, error);
				if (!rhs) {
					break;
				}
				auto lhs = CaseWithOperand::CreateOperandReference(shared_operand->GetReturnType());
				auto condition =
				    CreateBoundComparison(comparison.get().GetExpressionType(), std::move(lhs), std::move(rhs), error);
				if (!condition) {
					error.AddQueryLocation(expr);
					break;
				}
				conditions.push_back(std::move(condition));
			}
			if (error.HasError()) {
				return BindResult(std::move(error));
			}
			if (conditions.size() == 1) {
				when_expr = std::move(conditions[0]);
			} else {
				when_expr = BoundConjunctionExpression::Create(ExpressionType::CONJUNCTION_OR, std::move(conditions));
			}
		} else {
			when_expr = BindChild(check.when_expr, depth, error);
		}
		auto then_expr = BindChild(check.then_expr, depth, error);
		checks.emplace_back(std::move(when_expr), std::move(then_expr));
	}
	auto else_expr = BindChild(expr.ElseMutable(), depth, error);
	if (error.HasError()) {
		return BindResult(std::move(error));
	}
	// the children have been successfully resolved
	// figure out the result type of the CASE expression
	auto return_type = ExpressionBinder::GetExpressionReturnType(*else_expr);
	const auto decimal_width_limit = binder.GetDecimalWidthLimit();
	for (auto &check : checks) {
		auto then_type = ExpressionBinder::GetExpressionReturnType(*check.second);
		if (!LogicalType::TryGetMaxLogicalType(context, return_type, then_type, return_type, decimal_width_limit)) {
			throw BinderException(
			    expr, "Cannot mix values of type %s and %s in CASE expression - an explicit cast is required",
			    return_type.ToString(), then_type.ToString());
		}
	}

	if (shared_operand) {
		vector<pair<unique_ptr<Expression>, unique_ptr<Expression>>> casted_checks;
		for (auto &check : checks) {
			auto when_expr = BoundCastExpression::AddCastToType(context, std::move(check.first), LogicalType::BOOLEAN);
			auto then_expr = BoundCastExpression::AddCastToType(context, std::move(check.second), return_type);
			casted_checks.emplace_back(std::move(when_expr), std::move(then_expr));
		}
		auto else_result = BoundCastExpression::AddCastToType(context, std::move(else_expr), return_type);
		return BindResult(
		    CaseWithOperand::Create(std::move(shared_operand), std::move(casted_checks), std::move(else_result)));
	}

	// bind all the individual components of the CASE statement
	auto result = make_uniq<BoundCaseExpression>(return_type);
	for (auto &check : checks) {
		BoundCaseCheck result_check;
		result_check.when_expr =
		    BoundCastExpression::AddCastToType(context, std::move(check.first), LogicalType::BOOLEAN);
		result_check.then_expr = BoundCastExpression::AddCastToType(context, std::move(check.second), return_type);
		result->CaseChecksMutable().push_back(std::move(result_check));
	}
	result->ElseMutable() = BoundCastExpression::AddCastToType(context, std::move(else_expr), return_type);
	return BindResult(std::move(result));
}
} // namespace duckdb
