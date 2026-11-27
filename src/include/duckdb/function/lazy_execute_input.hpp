//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/lazy_execute_input.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/execution/expression_executor_state.hpp"

namespace duckdb {
class BoundFunctionExpression;
class Expression;
class ExpressionExecutor;
class LazyExecuteInput;

//! The input of a lazy scalar function (see ScalarFunction::SetLazyExecuteCallback).
//!
//! The arguments of a regular scalar function have all been executed on all rows by the time the function is called.
//! A lazy function receives its children unevaluated instead, together with the selection of rows it has to produce a
//! result for. It decides which children are executed, on which rows, and in which order - for example to only execute
//! a child on the rows for which an earlier child was true (CASE, COALESCE).
//!
//! Rows are described by a selection vector `sel` and a `count` - the same convention as the expression executor:
//! the i'th row is the physical row `sel[i]` (or just `i` if there is no selection). The result of executing an
//! expression on (sel, count) is compact: the i'th entry of the result belongs to the i'th row of the selection.
class LazyExecuteInput {
public:
	LazyExecuteInput(ExpressionExecutor &executor, const BoundFunctionExpression &expr, ExpressionState &state,
	                 optional_ptr<const SelectionVector> sel, idx_t count);

public:
	//! Whether the expression is a call of a lazy scalar function
	static bool IsLazyFunction(const duckdb::Expression &expr);

public:
	//! The function expression that is being executed
	const BoundFunctionExpression &Expression() const {
		return expr;
	}
	ExpressionState &State() {
		return state;
	}
	optional_ptr<const FunctionData> BindData() const;
	//! The thread-local state created by the init_local_state callback of the function
	optional_ptr<FunctionLocalState> LocalState() const;
	//! The rows the function has to produce a result for. The selection is null if these are the rows [0, Count())
	optional_ptr<const SelectionVector> Sel() const {
		return sel;
	}
	idx_t Count() const {
		return count;
	}
	idx_t ChildCount() const;

public:
	//! Execute child `child` on the given rows (which must be a subset of the rows of this input). `result` must
	//! have the type of the child. The result is compact, i.e. its i'th entry belongs to the i'th given row
	void Execute(idx_t child, optional_ptr<const SelectionVector> child_sel, idx_t child_count, Vector &result);
	//! Execute boolean child `child` on the given rows and split them into the rows where it is true and the rows where
	//! it is false (or NULL). The selections hold physical row indexes. Returns the number of true rows
	idx_t Select(idx_t child, optional_ptr<const SelectionVector> child_sel, idx_t child_count,
	             optional_ptr<SelectionVector> true_sel, optional_ptr<SelectionVector> false_sel);
	//! A vector with the type of child `child` that can be used as the target of Execute. It is reset
	//! every time the function is executed
	Vector &ChildBuffer(idx_t child);

public:
	//! Copy the compact `source` into `result` at the physical rows described by (fill_sel, fill_count)
	void Fill(const Vector &source, Vector &result, const SelectionVector &fill_sel, idx_t fill_count);
	//! Turns a result that was written at physical row positions (using Fill) into the compact result that has to be
	//! returned. Not required when the result was written by executing a child directly into it
	void Compact(Vector &result);

private:
	ExpressionExecutor &executor;
	const BoundFunctionExpression &expr;
	ExpressionState &state;
	optional_ptr<const SelectionVector> sel;
	idx_t count;
};

} // namespace duckdb
