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
class ExpressionExecutor;
class MarkJoinRowComparison;

struct NestedLoopJoinInner {
	static idx_t Perform(idx_t &ltuple, idx_t &rtuple, DataChunk &left_conditions, DataChunk &right_conditions,
	                     SelectionVector &lvector, SelectionVector &rvector, const vector<JoinCondition> &conditions);
};

struct NestedLoopJoinMark {
	//! Nested keys are compared with comparison_executor over comparison_input ([left row, right rows]), or with
	//! tuple_comparison for TUPLE (in)equality
	static void Perform(DataChunk &left, ColumnDataCollection &right, bool found_match[],
	                    const vector<JoinCondition> &conditions, ExpressionExecutor &comparison_executor,
	                    DataChunk &comparison_input, optional_ptr<MarkJoinRowComparison> tuple_comparison,
	                    optional_ptr<bool> found_unknown = nullptr);
	//! Marks the rows of `left` that a row of `right` matches
	static void Perform(const Vector &left, const Vector &right, idx_t left_count, idx_t right_count,
	                    bool found_match[], ExpressionType comparison, ExpressionExecutor &comparison_executor,
	                    DataChunk &comparison_input, optional_ptr<MarkJoinRowComparison> tuple_comparison);
};

} // namespace duckdb
