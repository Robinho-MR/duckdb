#include "duckdb/execution/mark_join_row_comparison.hpp"

#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"

namespace duckdb {

static void AddLeafEqualities(const LogicalType &type, vector<unique_ptr<Expression>> &result) {
	if (type.id() == LogicalTypeId::TUPLE) {
		for (auto &child : StructType::GetChildTypes(type)) {
			AddLeafEqualities(child.second, result);
		}
		return;
	}
	result.push_back(BoundComparisonExpression::CreateOnColumns(ExpressionType::COMPARE_EQUAL, type, type));
}

vector<unique_ptr<Expression>> MarkJoinComparisons::CreateLeafEqualities(const LogicalType &type) {
	vector<unique_ptr<Expression>> result;
	AddLeafEqualities(type, result);
	return result;
}

MarkJoinComparisons::MarkJoinComparisons(const vector<JoinCondition> &conditions) {
	for (auto &condition : conditions) {
		if (!condition.IsComparison()) {
			continue;
		}
		auto &type = condition.GetRHS().GetReturnType();
		key_equals.push_back(BoundComparisonExpression::CreateOnColumns(ExpressionType::COMPARE_EQUAL, type, type));
		key_leaf_equalities.push_back(CreateLeafEqualities(type));
	}
}

MarkJoinRowComparison::MarkJoinRowComparison(Context &context, const vector<unique_ptr<Expression>> &leaf_equalities) {
	for (auto &equality : leaf_equalities) {
		leaf_executors.push_back(make_uniq<ExpressionExecutor>(context, *equality));
	}
}

void MarkJoinRowComparison::CompareInternal(const Vector &left, idx_t left_row, idx_t left_count, const Vector &right,
                                            idx_t right_count, const bool active[], bool row_is_false[],
                                            bool row_is_unknown[], idx_t &leaf_idx) {
	if (left.GetType().id() != LogicalTypeId::TUPLE) {
		Vector left_reference(left.GetType());
		ConstantVector::Reference(left_reference, count_t(right_count), left, left_row, left_count);
		Vector comparison(LogicalType::BOOLEAN, right_count);
		leaf_executors[leaf_idx++]->ExecuteBinaryExpression(left_reference, right, comparison);

		UnifiedVectorFormat comparison_format;
		comparison.ToUnifiedFormat(comparison_format);
		auto comparison_data = comparison_format.GetData<bool>();
		for (idx_t right_row = 0; right_row < right_count; right_row++) {
			if (!active[right_row] || row_is_false[right_row]) {
				continue;
			}
			auto comparison_idx = comparison_format.sel->get_index(right_row);
			if (!comparison_format.validity.RowIsValid(comparison_idx)) {
				row_is_unknown[right_row] = true;
			} else if (!comparison_data[comparison_idx]) {
				row_is_false[right_row] = true;
				row_is_unknown[right_row] = false;
			}
		}
		return;
	}

	D_ASSERT(right.GetType().id() == LogicalTypeId::TUPLE);
	UnifiedVectorFormat left_format;
	UnifiedVectorFormat right_format;
	left.ToUnifiedFormat(left_format);
	right.ToUnifiedFormat(right_format);
	auto left_idx = left_format.sel->get_index(left_row);

	bool child_active[STANDARD_VECTOR_SIZE] = {false};
	for (idx_t right_row = 0; right_row < right_count; right_row++) {
		if (!active[right_row] || row_is_false[right_row]) {
			continue;
		}
		auto right_idx = right_format.sel->get_index(right_row);
		if (!left_format.validity.RowIsValid(left_idx) || !right_format.validity.RowIsValid(right_idx)) {
			row_is_unknown[right_row] = true;
		} else {
			child_active[right_row] = true;
		}
	}

	auto &left_children = StructVector::GetEntries(left);
	auto &right_children = StructVector::GetEntries(right);
	D_ASSERT(left_children.size() == right_children.size());
	for (idx_t child_idx = 0; child_idx < left_children.size(); child_idx++) {
		CompareInternal(left_children[child_idx], left_row, left_count, right_children[child_idx], right_count,
		                child_active, row_is_false, row_is_unknown, leaf_idx);
	}
}

void MarkJoinRowComparison::CompareEquality(const Vector &left, idx_t left_row, idx_t left_count, const Vector &right,
                                            idx_t right_count, bool row_is_false[], bool row_is_unknown[]) {
	D_ASSERT(left.GetType() == right.GetType());
	D_ASSERT(right_count <= STANDARD_VECTOR_SIZE);
	bool active[STANDARD_VECTOR_SIZE];
	for (idx_t right_row = 0; right_row < right_count; right_row++) {
		active[right_row] = !row_is_false[right_row];
	}
	idx_t leaf_idx = 0;
	CompareInternal(left, left_row, left_count, right, right_count, active, row_is_false, row_is_unknown, leaf_idx);
	D_ASSERT(leaf_idx == leaf_executors.size());
	for (idx_t right_row = 0; right_row < right_count; right_row++) {
		D_ASSERT(!row_is_false[right_row] || !row_is_unknown[right_row]);
	}
}

} // namespace duckdb
