//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression/bound_coalesce_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression.hpp"

namespace duckdb {

//! COALESCE is bound as a lazy scalar function (__internal_coalesce), which only executes a child on the rows for
//! which all earlier children were NULL. This class exists only as a set of helper methods.
struct BoundCoalesceExpression {
	//! Whether or not the given expression is a bound COALESCE
	static bool IsCoalesce(const Expression &expr);
	//! Create a COALESCE over the given children, which must all have the type `return_type`
	static unique_ptr<Expression> Create(const LogicalType &return_type, vector<unique_ptr<Expression>> children);
	static unique_ptr<Expression> Create(const LogicalType &return_type, unique_ptr<Expression> lhs,
	                                     unique_ptr<Expression> rhs);
};

} // namespace duckdb
