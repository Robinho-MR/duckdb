#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

template <class PREDICATE>
static idx_t DistinctComparatorSelect(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel,
                                      idx_t count, bool nulls_first, optional_ptr<SelectionVector> true_sel,
                                      optional_ptr<SelectionVector> false_sel, PREDICATE predicate) {
	Vector comparator_result(LogicalType::TINYINT, count);
	if (nulls_first) {
		VectorOperations::DistinctComparatorNullsFirstFill(left, right, comparator_result, count);
	} else {
		VectorOperations::DistinctComparatorFill(left, right, comparator_result, count);
	}
	auto cmp_data = comparator_result.Values<int8_t>();

	idx_t true_count = 0;
	idx_t false_count = 0;
	for (idx_t i = 0; i < count; i++) {
		auto result_idx = sel ? sel->get_index(i) : i;
		if (predicate(cmp_data[i].GetValueUnsafe())) {
			if (true_sel) {
				true_sel->set_index(true_count, result_idx);
			}
			true_count++;
		} else {
			if (false_sel) {
				false_sel->set_index(false_count, result_idx);
			}
			false_count++;
		}
	}
	return true_count;
}

idx_t VectorOperations::SelectDistinctComparator(const Vector &left, const Vector &right,
                                                 optional_ptr<const SelectionVector> sel, idx_t count,
                                                 ExpressionType comparison, bool nulls_first,
                                                 optional_ptr<SelectionVector> true_sel,
                                                 optional_ptr<SelectionVector> false_sel) {
	switch (comparison) {
	case ExpressionType::COMPARE_GREATERTHAN:
		return DistinctComparatorSelect(left, right, sel, count, nulls_first, true_sel, false_sel,
		                                [](int8_t v) { return v > 0; });
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return DistinctComparatorSelect(left, right, sel, count, nulls_first, true_sel, false_sel,
		                                [](int8_t v) { return v >= 0; });
	case ExpressionType::COMPARE_LESSTHAN:
		return DistinctComparatorSelect(left, right, sel, count, nulls_first, true_sel, false_sel,
		                                [](int8_t v) { return v < 0; });
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return DistinctComparatorSelect(left, right, sel, count, nulls_first, true_sel, false_sel,
		                                [](int8_t v) { return v <= 0; });
	default:
		throw InternalException("SelectDistinctComparator: unsupported comparison type %s",
		                        EnumUtil::ToString(comparison));
	}
}

} // namespace duckdb
