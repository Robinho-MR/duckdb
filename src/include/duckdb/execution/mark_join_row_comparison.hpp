//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/mark_join_row_comparison.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {

//! The equalities of the (depth-first) non-TUPLE leaves of a MARK join key
struct MarkJoinKeyComparison {
	MarkJoinKeyComparison(ClientContext &context, const LogicalType &type);

	//! The types of the leaves
	vector<LogicalType> leaf_types;
	//! "#0 = #1" for every leaf
	vector<unique_ptr<Expression>> leaf_equalities;
};

//! The comparisons of the keys of an uncorrelated MARK join
struct MarkJoinComparisons {
	MarkJoinComparisons(ClientContext &context, const vector<LogicalType> &key_types);

	//! "key = key" for every nested key (nullptr otherwise), to find the keys that compare to NULL
	vector<unique_ptr<Expression>> key_self_equalities;
	//! The leaf equalities of every key
	vector<MarkJoinKeyComparison> key_comparisons;
};

//! Compares a row of one vector with the rows of another vector using three-valued logic, leaf by leaf
class MarkJoinRowComparison {
public:
	MarkJoinRowComparison(ClientContext &context, const MarkJoinKeyComparison &comparison);

	//! Updates which rows are certainly different (row_is_false) or compare to NULL (row_is_unknown)
	void CompareEquality(const Vector &left, idx_t left_row, idx_t left_count, const Vector &right, idx_t right_count,
	                     bool row_is_false[], bool row_is_unknown[]);

private:
	void CompareInternal(const Vector &left, idx_t left_row, idx_t left_count, const Vector &right, idx_t right_count,
	                     const bool active[], bool row_is_false[], bool row_is_unknown[], idx_t &leaf_idx);

private:
	vector<unique_ptr<ExpressionExecutor>> leaf_executors;
	//! The input of every leaf equality: [left row, right rows]
	vector<unique_ptr<DataChunk>> leaf_inputs;
};

//! The executors of the comparisons of the keys of an uncorrelated MARK join
class MarkJoinComparisonState {
public:
	MarkJoinComparisonState(ClientContext &context, const MarkJoinComparisons &comparisons);

	//! Whether any key compares to NULL, marking the rows for which it does
	bool KeysHaveNull(DataChunk &keys, optional_ptr<bool> rows_with_null = nullptr);
	//! Whether a row of "left" compares to NULL with a row of "right" that it is not certainly different from
	bool RowHasUnknown(DataChunk &left, idx_t left_row, DataChunk &right);

private:
	vector<unique_ptr<ExpressionExecutor>> key_self_equalities;
	vector<unique_ptr<MarkJoinRowComparison>> row_comparisons;
};

} // namespace duckdb
