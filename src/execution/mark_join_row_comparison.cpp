#include "duckdb/execution/mark_join_row_comparison.hpp"

#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"

namespace duckdb {

static void AddLeafTypes(const LogicalType &type, vector<LogicalType> &result) {
	if (type.id() != LogicalTypeId::TUPLE) {
		result.push_back(type);
		return;
	}
	for (auto &child : StructType::GetChildTypes(type)) {
		AddLeafTypes(child.second, result);
	}
}

MarkJoinKeyComparison::MarkJoinKeyComparison(ClientContext &context, const LogicalType &type) {
	AddLeafTypes(type, leaf_types);
	for (auto &leaf_type : leaf_types) {
		leaf_equalities.push_back(BoundComparisonExpression::CreateOnColumns(context, ExpressionType::COMPARE_EQUAL,
		                                                                     leaf_type, 0, leaf_type, 1));
	}
}

MarkJoinComparisons::MarkJoinComparisons(ClientContext &context, const vector<LogicalType> &key_types) {
	for (idx_t key_idx = 0; key_idx < key_types.size(); key_idx++) {
		auto &type = key_types[key_idx];
		if (type.IsNested()) {
			key_self_equalities.push_back(BoundComparisonExpression::CreateOnColumns(
			    context, ExpressionType::COMPARE_EQUAL, type, key_idx, type, key_idx));
		} else {
			key_self_equalities.push_back(nullptr);
		}
		key_comparisons.emplace_back(context, type);
	}
}

MarkJoinRowComparison::MarkJoinRowComparison(ClientContext &context, const MarkJoinKeyComparison &comparison) {
	for (idx_t leaf_idx = 0; leaf_idx < comparison.leaf_types.size(); leaf_idx++) {
		auto &leaf_type = comparison.leaf_types[leaf_idx];
		leaf_executors.push_back(make_uniq<ExpressionExecutor>(context, *comparison.leaf_equalities[leaf_idx]));
		leaf_inputs.push_back(make_uniq<DataChunk>());
		leaf_inputs.back()->InitializeEmpty({leaf_type, leaf_type});
	}
}

void MarkJoinRowComparison::CompareInternal(const Vector &left, idx_t left_row, idx_t left_count, const Vector &right,
                                            idx_t right_count, const bool active[], bool row_is_false[],
                                            bool row_is_unknown[], idx_t &leaf_idx) {
	if (left.GetType().id() != LogicalTypeId::TUPLE) {
		auto &input = *leaf_inputs[leaf_idx];
		ConstantVector::Reference(input.data[0], count_t(right_count), left, left_row, left_count);
		input.data[1].Reference(right);
		input.CheckCardinality(right_count);
		Vector comparison(LogicalType::BOOLEAN, right_count);
		leaf_executors[leaf_idx]->ExecuteExpression(input, comparison);
		leaf_idx++;

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

MarkJoinComparisonState::MarkJoinComparisonState(ClientContext &context, const MarkJoinComparisons &comparisons) {
	for (auto &self_equality : comparisons.key_self_equalities) {
		key_self_equalities.push_back(self_equality ? make_uniq<ExpressionExecutor>(context, *self_equality) : nullptr);
	}
	for (auto &key_comparison : comparisons.key_comparisons) {
		row_comparisons.push_back(make_uniq<MarkJoinRowComparison>(context, key_comparison));
	}
}

static bool MarkJoinNullRows(const UnifiedVectorFormat &format, idx_t count, optional_ptr<bool> rows_with_null) {
	if (format.validity.CannotHaveNull()) {
		return false;
	}
	bool has_null = false;
	for (idx_t row_idx = 0; row_idx < count; row_idx++) {
		auto format_idx = format.sel->get_index(row_idx);
		if (!format.validity.RowIsValid(format_idx)) {
			has_null = true;
			if (rows_with_null) {
				rows_with_null.get()[row_idx] = true;
			}
		}
	}
	return has_null;
}

bool MarkJoinComparisonState::KeysHaveNull(DataChunk &keys, optional_ptr<bool> rows_with_null) {
	D_ASSERT(keys.ColumnCount() == key_self_equalities.size());
	bool has_null = false;
	for (idx_t col_idx = 0; col_idx < keys.ColumnCount(); col_idx++) {
		UnifiedVectorFormat format;
		Vector comparison(LogicalType::BOOLEAN, keys.size());
		if (key_self_equalities[col_idx]) {
			key_self_equalities[col_idx]->ExecuteExpression(keys, comparison);
			comparison.ToUnifiedFormat(format);
		} else {
			keys.data[col_idx].ToUnifiedFormat(format);
		}
		if (MarkJoinNullRows(format, keys.size(), rows_with_null)) {
			has_null = true;
		}
	}
	return has_null;
}

bool MarkJoinComparisonState::RowHasUnknown(DataChunk &left, idx_t left_row, DataChunk &right) {
	D_ASSERT(left.ColumnCount() == right.ColumnCount());
	D_ASSERT(left.ColumnCount() == row_comparisons.size());
	bool row_is_false[STANDARD_VECTOR_SIZE] = {false};
	bool row_is_unknown[STANDARD_VECTOR_SIZE] = {false};
	for (idx_t col_idx = 0; col_idx < left.ColumnCount(); col_idx++) {
		row_comparisons[col_idx]->CompareEquality(left.data[col_idx], left_row, left.size(), right.data[col_idx],
		                                          right.size(), row_is_false, row_is_unknown);
	}
	for (idx_t right_row = 0; right_row < right.size(); right_row++) {
		if (!row_is_false[right_row] && row_is_unknown[right_row]) {
			return true;
		}
	}
	return false;
}

} // namespace duckdb
