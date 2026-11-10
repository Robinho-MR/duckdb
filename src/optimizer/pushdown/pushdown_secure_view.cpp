#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression/expression_barrier.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_empty_result.hpp"
#include "duckdb/planner/operator/logical_secure_view.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"

namespace duckdb {

static bool RetainSecureViewFilter(unique_ptr<Expression> &expression, const LogicalSecureView &view) {
	if (expression->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &column = expression->Cast<BoundColumnRefExpression>();
		if (column.Depth() != 0 || view.output_bindings.size() != view.output_expressions.size()) {
			return false;
		}
		for (idx_t i = 0; i < view.output_bindings.size(); i++) {
			if (column.Binding() == view.output_bindings[i]) {
				expression = view.output_expressions[i]->Copy();
				return true;
			}
		}
		return false;
	}
	bool valid = true;
	ExpressionIterator::EnumerateChildren(
	    *expression, [&](unique_ptr<Expression> &child) { valid = RetainSecureViewFilter(child, view) && valid; });
	return valid;
}

static bool ComputedUnsafely(LogicalOperator &op, const ColumnBinding &binding);

static bool ComputedUnsafely(LogicalOperator &op, const Expression &expr) {
	if (expr.CanThrow() || expr.IsVolatile()) {
		return true;
	}
	bool unsafe = false;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expr, [&](const BoundColumnRefExpression &colref) {
		unsafe = unsafe || ComputedUnsafely(op, colref.Binding());
	});
	return unsafe;
}

//! Whether filter pushdown can substitute an expression that may throw or is volatile for this column - a filter on
//! it becomes observable once it is pushed past that point
static bool ComputedUnsafely(LogicalOperator &op, const ColumnBinding &binding) {
	auto table_indexes = op.GetTableIndex();
	if (std::find(table_indexes.begin(), table_indexes.end(), binding.table_index) == table_indexes.end()) {
		for (auto &child : op.children) {
			for (auto &child_binding : child->GetColumnBindings()) {
				if (child_binding.table_index == binding.table_index) {
					return ComputedUnsafely(*child, binding);
				}
			}
		}
		return false;
	}
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_PROJECTION:
		return ComputedUnsafely(*op.children[0], *op.Cast<LogicalProjection>().expressions[binding.column_index]);
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
		auto &aggr = op.Cast<LogicalAggregate>();
		if (binding.table_index != aggr.group_index || aggr.grouping_sets.size() > 1) {
			return false;
		}
		return ComputedUnsafely(*op.children[0], *aggr.groups[binding.column_index]);
	}
	case LogicalOperatorType::LOGICAL_UNION:
	case LogicalOperatorType::LOGICAL_EXCEPT:
	case LogicalOperatorType::LOGICAL_INTERSECT:
		for (auto &child : op.children) {
			if (ComputedUnsafely(*child, child->GetColumnBindings()[binding.column_index])) {
				return true;
			}
		}
		return false;
	default:
		return false;
	}
}

static bool RequiresBarrier(const Expression &expr, LogicalOperator &child) {
	if (ExpressionBarrier::Required(expr)) {
		return true;
	}
	bool unsafe = false;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expr, [&](const BoundColumnRefExpression &colref) {
		unsafe = unsafe || ComputedUnsafely(child, colref.Binding());
	});
	return unsafe;
}

unique_ptr<LogicalOperator> FilterPushdown::PushdownSecureView(unique_ptr<LogicalOperator> op) {
	D_ASSERT(op->type == LogicalOperatorType::LOGICAL_SECURE_VIEW);
	// filters can be pushed into a secure view, but only if evaluating them on rows that the view does not emit
	// cannot be observed from the outside. Expressions that can throw an error or have side effects are wrapped in
	// a barrier: those are never pushed past an operator inside the view that removes rows, and are always
	// evaluated after the filters they end up next to.
	auto &secure_view = op->Cast<LogicalSecureView>();
	// freeze the cardinality estimate of the view before anything is pushed into it - otherwise the estimate of the
	// boundary node reports what the statistics of the view contents say about the caller's predicate. Views whose
	// statistics are exposed anyway do not need this - the caller can derive the same estimate above the boundary
	if (!filters.empty() && !secure_view.propagate_statistics && !op->has_estimated_cardinality) {
		op->SetEstimatedCardinality(op->children[0]->EstimateCardinality(optimizer.GetContext()));
	}

	FilterPushdown child_pushdown(optimizer, convert_mark_joins, projection_mode);
	// a filter barred here comes from the level just outside the view, one barred already from further out: the
	// former go first, so that barred filters stay ordered deepest level first
	vector<unique_ptr<Expression>> barred_before;
	vector<unique_ptr<Expression>> plain_or_new;
	for (auto &f : filters) {
		auto expr = std::move(f->filter);
		if (secure_view.has_source) {
			auto source_filter = expr->Copy();
			if (!RetainSecureViewFilter(source_filter, secure_view)) {
				source_filter.reset();
			}
			secure_view.source_filters.push_back(std::move(source_filter));
		}
		// the operators inside the view are never shown - report the filter as part of the boundary node instead
		secure_view.pushed_filters.push_back(expr->ToString());
		if (ExpressionBarrier::Contains(*expr)) {
			barred_before.push_back(std::move(expr));
			continue;
		}
		if (RequiresBarrier(*expr, *op->children[0])) {
			expr = ExpressionBarrier::Wrap(std::move(expr));
		}
		plain_or_new.push_back(std::move(expr));
	}
	for (auto &expr : barred_before) {
		plain_or_new.push_back(std::move(expr));
	}
	for (auto &expr : plain_or_new) {
		if (child_pushdown.AddFilter(std::move(expr)) == FilterResult::UNSATISFIABLE) {
			// filter statically evaluates to false, strip tree
			return make_uniq<LogicalEmptyResult>(std::move(op));
		}
	}
	filters.clear();
	child_pushdown.GenerateFilters();

	// note that the boundary node is always kept in place, even if the child collapses into an empty result - the
	// plan outside the view must not depend on anything the optimizer learns about the contents of the view
	op->children[0] = child_pushdown.Rewrite(std::move(op->children[0]));
	return op;
}

} // namespace duckdb
