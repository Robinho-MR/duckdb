//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/comparison_executor.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

//! Compares two vectors by running the expression "left <op> right" through the expression executor
class ComparisonExecutor {
public:
	ComparisonExecutor(Context &context, ExpressionType comparison, const LogicalType &left_type,
	                   const LogicalType &right_type);
	ComparisonExecutor(ClientContext &context, ExpressionType comparison, const LogicalType &left_type,
	                   const LogicalType &right_type);
	ComparisonExecutor(Context &context, ExpressionType comparison, const LogicalType &type)
	    : ComparisonExecutor(context, comparison, type, type) {
	}
	ComparisonExecutor(ClientContext &context, ExpressionType comparison, const LogicalType &type)
	    : ComparisonExecutor(context, comparison, type, type) {
	}

public:
	//! result = left <op> right (result must be a BOOLEAN vector)
	void Execute(const Vector &left, const Vector &right, Vector &result);
	//! Select the rows (the first 'count' entries of 'sel', or all rows if there is no 'sel') for which
	//! left <op> right is true. NULL results are added to false_sel.
	idx_t Select(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel, idx_t count,
	             optional_ptr<SelectionVector> true_sel, optional_ptr<SelectionVector> false_sel);

private:
	void SetInput(const Vector &left, const Vector &right, idx_t min_count);

private:
	unique_ptr<Expression> expression;
	DataChunk input;
	ExpressionExecutor executor;
};

} // namespace duckdb
