//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/scalar/case_with_operand.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

//! A simple CASE expression (CASE <operand> WHEN <x> ..) whose operand has to be evaluated exactly once, for example
//! because it is volatile. The WHEN conditions refer to the value of the operand through operand references.
//!
//! It is a lazy scalar function (see LazyExecuteInput) with the children
//! [operand, when_1, then_1, .., when_n, then_n, else]. It executes the operand first and exposes the result as a
//! scoped value, and then executes the checks in the same way as a regular CASE.
struct CaseWithOperand {
	//! Create the expression. The conditions have to refer to the operand using CreateOperandReference
	static unique_ptr<Expression> Create(unique_ptr<Expression> operand,
	                                     vector<pair<unique_ptr<Expression>, unique_ptr<Expression>>> checks,
	                                     unique_ptr<Expression> else_expr);
	//! Create a reference to the value of the operand of the innermost enclosing CASE with an operand
	static unique_ptr<Expression> CreateOperandReference(const LogicalType &operand_type);
};

} // namespace duckdb
