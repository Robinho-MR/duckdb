#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/uhugeint.hpp"
#include "duckdb/execution/mark_join_row_comparison.hpp"
#include "duckdb/execution/nested_loop_join.hpp"

namespace duckdb {

template <class T, class OP>
static void TemplatedMarkJoin(const Vector &left, const Vector &right, idx_t lcount, idx_t rcount, bool found_match[]) {
	using MATCH_OP = ComparisonOperationWrapper<OP>;

	auto left_entries = left.Values<T>();
	auto right_entries = right.Values<T>();
	for (idx_t i = 0; i < lcount; i++) {
		if (found_match[i]) {
			continue;
		}
		auto left_entry = left_entries[i];
		const auto left_null = !left_entry.IsValid();
		if (!MATCH_OP::COMPARE_NULL && left_null) {
			continue;
		}
		for (idx_t j = 0; j < rcount; j++) {
			auto right_entry = right_entries[j];
			const auto right_null = !right_entry.IsValid();
			if (!MATCH_OP::COMPARE_NULL && right_null) {
				continue;
			}
			if (MATCH_OP::template Operation<T>(left_entries.GetValueUnsafe(i), right_entries.GetValueUnsafe(j),
			                                    left_null, right_null)) {
				found_match[i] = true;
				break;
			}
		}
	}
}

static void MarkJoinNested(const Vector &left, const Vector &right, idx_t lcount, idx_t rcount, bool found_match[],
                           ExpressionType comparison_type, ExpressionExecutor &comparison_executor,
                           DataChunk &comparison_input, optional_ptr<MarkJoinRowComparison> tuple_comparison,
                           optional_ptr<bool> found_unknown) {
	if (left.GetType().id() == LogicalTypeId::TUPLE &&
	    (comparison_type == ExpressionType::COMPARE_EQUAL || comparison_type == ExpressionType::COMPARE_NOTEQUAL)) {
		D_ASSERT(tuple_comparison);
		for (idx_t left_row = 0; left_row < lcount; left_row++) {
			if (found_match[left_row]) {
				continue;
			}
			bool equality_is_false[STANDARD_VECTOR_SIZE] = {false};
			bool equality_is_unknown[STANDARD_VECTOR_SIZE] = {false};
			tuple_comparison->CompareEquality(left, left_row, lcount, right, rcount, equality_is_false,
			                                  equality_is_unknown);
			for (idx_t right_row = 0; right_row < rcount; right_row++) {
				const bool is_match = comparison_type == ExpressionType::COMPARE_EQUAL
				                          ? !equality_is_false[right_row] && !equality_is_unknown[right_row]
				                          : equality_is_false[right_row];
				if (is_match) {
					found_match[left_row] = true;
					break;
				}
				if (found_unknown && equality_is_unknown[right_row]) {
					found_unknown.get()[left_row] = true;
				}
			}
		}
		return;
	}

	for (idx_t i = 0; i < lcount; i++) {
		if (found_match[i]) {
			continue;
		}
		ConstantVector::Reference(comparison_input.data[0], count_t(rcount), left, i, lcount);
		comparison_input.data[1].Reference(right);
		comparison_input.CheckCardinality(rcount);
		Vector comparison(LogicalType::BOOLEAN, rcount);
		comparison_executor.ExecuteExpression(comparison_input, comparison);
		bool has_unknown = false;
		for (auto entry : comparison.Values<bool>()) {
			if (!entry.IsValid()) {
				has_unknown = true;
			} else if (entry.GetValue()) {
				found_match[i] = true;
				break;
			}
		}
		if (!found_match[i] && found_unknown && has_unknown) {
			found_unknown.get()[i] = true;
		}
	}
}

template <class OP>
static void MarkJoinSwitch(const Vector &left, const Vector &right, idx_t lcount, idx_t rcount, bool found_match[]) {
	switch (left.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return TemplatedMarkJoin<int8_t, OP>(left, right, lcount, rcount, found_match);
	case PhysicalType::INT16:
		return TemplatedMarkJoin<int16_t, OP>(left, right, lcount, rcount, found_match);
	case PhysicalType::INT32:
		return TemplatedMarkJoin<int32_t, OP>(left, right, lcount, rcount, found_match);
	case PhysicalType::INT64:
		return TemplatedMarkJoin<int64_t, OP>(left, right, lcount, rcount, found_match);
	case PhysicalType::INT128:
		return TemplatedMarkJoin<hugeint_t, OP>(left, right, lcount, rcount, found_match);
	case PhysicalType::UINT8:
		return TemplatedMarkJoin<uint8_t, OP>(left, right, lcount, rcount, found_match);
	case PhysicalType::UINT16:
		return TemplatedMarkJoin<uint16_t, OP>(left, right, lcount, rcount, found_match);
	case PhysicalType::UINT32:
		return TemplatedMarkJoin<uint32_t, OP>(left, right, lcount, rcount, found_match);
	case PhysicalType::UINT64:
		return TemplatedMarkJoin<uint64_t, OP>(left, right, lcount, rcount, found_match);
	case PhysicalType::UINT128:
		return TemplatedMarkJoin<uhugeint_t, OP>(left, right, lcount, rcount, found_match);
	case PhysicalType::FLOAT:
		return TemplatedMarkJoin<float, OP>(left, right, lcount, rcount, found_match);
	case PhysicalType::DOUBLE:
		return TemplatedMarkJoin<double, OP>(left, right, lcount, rcount, found_match);
	case PhysicalType::VARCHAR:
		return TemplatedMarkJoin<string_t, OP>(left, right, lcount, rcount, found_match);
	default:
		throw NotImplementedException("Unimplemented type for mark join!");
	}
}

static void MarkJoinComparisonSwitch(const Vector &left, const Vector &right, idx_t lcount, idx_t rcount,
                                     bool found_match[], ExpressionType comparison_type,
                                     ExpressionExecutor &comparison_executor, DataChunk &comparison_input,
                                     optional_ptr<MarkJoinRowComparison> tuple_comparison,
                                     optional_ptr<bool> found_unknown) {
	switch (left.GetType().InternalType()) {
	case PhysicalType::STRUCT:
	case PhysicalType::LIST:
	case PhysicalType::ARRAY:
		return MarkJoinNested(left, right, lcount, rcount, found_match, comparison_type, comparison_executor,
		                      comparison_input, tuple_comparison, found_unknown);
	default:
		break;
	}
	D_ASSERT(left.GetType() == right.GetType());
	switch (comparison_type) {
	case ExpressionType::COMPARE_EQUAL:
		return MarkJoinSwitch<Equals>(left, right, lcount, rcount, found_match);
	case ExpressionType::COMPARE_NOTEQUAL:
		return MarkJoinSwitch<NotEquals>(left, right, lcount, rcount, found_match);
	case ExpressionType::COMPARE_LESSTHAN:
		return MarkJoinSwitch<LessThan>(left, right, lcount, rcount, found_match);
	case ExpressionType::COMPARE_GREATERTHAN:
		return MarkJoinSwitch<GreaterThan>(left, right, lcount, rcount, found_match);
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return MarkJoinSwitch<LessThanEquals>(left, right, lcount, rcount, found_match);
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return MarkJoinSwitch<GreaterThanEquals>(left, right, lcount, rcount, found_match);
	case ExpressionType::COMPARE_DISTINCT_FROM:
		return MarkJoinSwitch<DistinctFrom>(left, right, lcount, rcount, found_match);
	default:
		throw NotImplementedException("Unimplemented comparison type for join!");
	}
}

void NestedLoopJoinMark::Perform(DataChunk &left, ColumnDataCollection &right, bool found_match[],
                                 const vector<JoinCondition> &conditions, ExpressionExecutor &comparison_executor,
                                 DataChunk &comparison_input, optional_ptr<MarkJoinRowComparison> tuple_comparison,
                                 optional_ptr<bool> found_unknown) {
	D_ASSERT(conditions.size() == 1);
	// initialize a new temporary selection vector for the left chunk
	// loop over all chunks in the RHS
	ColumnDataScanState scan_state;
	right.InitializeScan(scan_state);

	DataChunk scan_chunk;
	right.InitializeScanChunk(scan_chunk);

	while (right.Scan(scan_state, scan_chunk)) {
		MarkJoinComparisonSwitch(left.data[0], scan_chunk.data[0], left.size(), scan_chunk.size(), found_match,
		                         conditions[0].GetComparisonType(), comparison_executor, comparison_input,
		                         tuple_comparison, found_unknown);
	}
}

} // namespace duckdb
