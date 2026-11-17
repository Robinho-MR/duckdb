//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/nested_loop_join.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"

namespace duckdb {
class ColumnDataCollection;

struct NestedLoopJoinInner {
	static idx_t Perform(idx_t &ltuple, idx_t &rtuple, DataChunk &left_conditions, DataChunk &right_conditions,
	                     SelectionVector &lvector, SelectionVector &rvector, const vector<JoinCondition> &conditions);
};

//! The comparisons of the condition of a MARK nested loop join, created during planning
struct MarkJoinPlan {
	MarkJoinPlan(const Expression &comparison, const vector<unique_ptr<Expression>> &leaf_equalities)
	    : comparison(comparison), leaf_equalities(leaf_equalities) {
	}

	//! The comparison of the condition
	const Expression &comparison;
	//! The equalities of the leaves of the condition (for comparing tuples)
	const vector<unique_ptr<Expression>> &leaf_equalities;
};

struct NestedLoopJoinMark {
	static void Perform(Context &context, const MarkJoinPlan &plan, DataChunk &left, ColumnDataCollection &right,
	                    bool found_match[], const vector<JoinCondition> &conditions,
	                    optional_ptr<bool> found_unknown = nullptr);
};

} // namespace duckdb
