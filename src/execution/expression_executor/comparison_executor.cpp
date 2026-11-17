#include "duckdb/execution/comparison_executor.hpp"

#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

namespace duckdb {

static unique_ptr<Expression> CreateComparison(ExpressionType comparison, const LogicalType &left_type,
                                               const LogicalType &right_type) {
	return BoundComparisonExpression::Create(comparison, make_uniq<BoundReferenceExpression>(left_type, 0),
	                                         make_uniq<BoundReferenceExpression>(right_type, 1));
}

ComparisonExecutor::ComparisonExecutor(Context &context, ExpressionType comparison, const LogicalType &left_type,
                                       const LogicalType &right_type)
    : expression(CreateComparison(comparison, left_type, right_type)), executor(context, *expression) {
	input.InitializeEmpty({left_type, right_type});
}

ComparisonExecutor::ComparisonExecutor(ClientContext &context, ExpressionType comparison,
                                       const LogicalType &left_type, const LogicalType &right_type)
    : expression(CreateComparison(comparison, left_type, right_type)), executor(context, *expression) {
	input.InitializeEmpty({left_type, right_type});
}

void ComparisonExecutor::SetInput(const Vector &left, const Vector &right, idx_t min_count) {
	input.data[0].Reference(left);
	input.data[1].Reference(right);
	input.SetCardinalityUnsafe(MaxValue<idx_t>(MaxValue<idx_t>(left.size(), right.size()), min_count));
}

void ComparisonExecutor::Execute(const Vector &left, const Vector &right, Vector &result) {
	SetInput(left, right, 0);
	executor.ExecuteExpression(input, result);
}

idx_t ComparisonExecutor::Select(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel,
                                 idx_t count, optional_ptr<SelectionVector> true_sel,
                                 optional_ptr<SelectionVector> false_sel) {
	SetInput(left, right, count);
	if (!sel) {
		// without a selection the executor requires the vectors to hold exactly 'count' rows
		for (auto &column : input.data) {
			if (column.GetVectorType() != VectorType::CONSTANT_VECTOR && column.size() > count) {
				column.Slice(*FlatVector::IncrementalSelectionVector(), count);
			}
		}
		input.SetCardinalityUnsafe(count);
	}
	return executor.SelectExpression(input, true_sel, false_sel, sel, count);
}

} // namespace duckdb
