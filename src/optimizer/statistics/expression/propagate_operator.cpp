#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

//! Casting a variable-size value to VARIANT throws if the encoded row exceeds the maximum size, even for TRY_CAST
static bool ContainsThrowingVariantCast(const Expression &expr) {
	if (BoundCastExpression::IsCast(expr)) {
		auto &cast = expr.Cast<BoundFunctionExpression>();
		auto source_type = BoundCastExpression::SourceType(cast);
		if (BoundCastExpression::TargetType(cast).id() == LogicalTypeId::VARIANT &&
		    source_type.id() != LogicalTypeId::VARIANT && !TypeIsConstantSize(source_type.InternalType())) {
			return true;
		}
	}
	bool result = false;
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { result = result || ContainsThrowingVariantCast(child); });
	return result;
}

//! Folding a NULL check removes its child from the plan
static bool CanFoldNullCheck(const BoundOperatorExpression &expr) {
	auto &child = *expr.GetChildren()[0];
	return !child.IsVolatile() && !ContainsThrowingVariantCast(child);
}

unique_ptr<BaseStatistics> StatisticsPropagator::PropagateExpression(BoundOperatorExpression &expr,
                                                                     unique_ptr<Expression> &expr_ptr) {
	bool all_have_stats = true;
	vector<unique_ptr<BaseStatistics>> child_stats;
	child_stats.reserve(expr.GetChildren().size());
	for (auto &child : expr.GetChildrenMutable()) {
		auto stats = PropagateExpression(child);
		if (!stats) {
			all_have_stats = false;
		}
		child_stats.push_back(std::move(stats));
	}
	if ((expr.GetExpressionType() == ExpressionType::COMPARE_IN ||
	     expr.GetExpressionType() == ExpressionType::COMPARE_NOT_IN) &&
	    !child_stats.empty() && HasNoValidValue(child_stats[0].get())) {
		bool removable = true;
		for (auto &child : expr.GetChildren()) {
			removable = removable && IsRemovable(*child);
		}
		if (removable) {
			// x [NOT] IN (...) is NULL for every row when x holds only NULLs
			expr_ptr = make_uniq<BoundConstantExpression>(Value(LogicalType::BOOLEAN));
			removed_expressions = true;
			return PropagateExpression(expr_ptr);
		}
	}
	if (!all_have_stats) {
		return nullptr;
	}
	switch (expr.GetExpressionType()) {
	case ExpressionType::OPERATOR_IS_NULL:
		if (!CanFoldNullCheck(expr)) {
			return nullptr;
		}
		if (!child_stats[0]->CanHaveNull()) {
			// child has no null values: x IS NULL will always be false
			expr_ptr = make_uniq<BoundConstantExpression>(Value::BOOLEAN(false));
			return PropagateExpression(expr_ptr);
		}
		if (!child_stats[0]->CanHaveNoNull()) {
			// child has no valid values: x IS NULL will always be true
			expr_ptr = make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
			return PropagateExpression(expr_ptr);
		}
		return nullptr;
	case ExpressionType::OPERATOR_IS_NOT_NULL:
		if (!CanFoldNullCheck(expr)) {
			return nullptr;
		}
		if (!child_stats[0]->CanHaveNull()) {
			// child has no null values: x IS NOT NULL will always be true
			expr_ptr = make_uniq<BoundConstantExpression>(Value::BOOLEAN(true));
			return PropagateExpression(expr_ptr);
		}
		if (!child_stats[0]->CanHaveNoNull()) {
			// child has no valid values: x IS NOT NULL will always be false
			expr_ptr = make_uniq<BoundConstantExpression>(Value::BOOLEAN(false));
			return PropagateExpression(expr_ptr);
		}
		return nullptr;
	default:
		return nullptr;
	}
}

} // namespace duckdb
