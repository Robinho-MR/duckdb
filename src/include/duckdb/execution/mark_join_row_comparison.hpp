//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/mark_join_row_comparison.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/joinside.hpp"

namespace duckdb {

//! The equality comparisons of the keys of a MARK join, created during planning
struct MarkJoinComparisons {
	MarkJoinComparisons() {
	}
	explicit MarkJoinComparisons(const vector<JoinCondition> &conditions);

	//! The equality of every key with itself, to find the NULLs inside nested keys
	vector<unique_ptr<Expression>> key_equals;
	//! The equalities of the (depth-first) leaves of every key
	vector<vector<unique_ptr<Expression>>> key_leaf_equalities;

	//! The equalities of the (depth-first) leaves of a type
	static vector<unique_ptr<Expression>> CreateLeafEqualities(const LogicalType &type);
};

//! Compares a row of one vector with the rows of another vector using three-valued logic, leaf by leaf
class MarkJoinRowComparison {
public:
	MarkJoinRowComparison(Context &context, const vector<unique_ptr<Expression>> &leaf_equalities);

	//! Updates which rows are certainly different (row_is_false) or compare to NULL (row_is_unknown)
	void CompareEquality(const Vector &left, idx_t left_row, idx_t left_count, const Vector &right, idx_t right_count,
	                     bool row_is_false[], bool row_is_unknown[]);

private:
	void CompareInternal(const Vector &left, idx_t left_row, idx_t left_count, const Vector &right, idx_t right_count,
	                     const bool active[], bool row_is_false[], bool row_is_unknown[], idx_t &leaf_idx);

private:
	vector<unique_ptr<ExpressionExecutor>> leaf_executors;
};

} // namespace duckdb
