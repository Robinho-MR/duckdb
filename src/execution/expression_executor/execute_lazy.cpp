#include "duckdb/function/lazy_execute_input.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

LazyExecuteInput::LazyExecuteInput(ExpressionExecutor &executor_p, const BoundFunctionExpression &expr_p,
                                   ExpressionState &state_p, optional_ptr<const SelectionVector> sel_p, idx_t count_p)
    : executor(executor_p), expr(expr_p), state(state_p), sel(sel_p), count(count_p) {
}

bool LazyExecuteInput::IsLazyFunction(const duckdb::Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION &&
	       expr.Cast<BoundFunctionExpression>().Function().HasLazyExecuteCallback();
}

optional_ptr<const FunctionData> LazyExecuteInput::BindData() const {
	return expr.BindInfo().get();
}

optional_ptr<FunctionLocalState> LazyExecuteInput::LocalState() const {
	return ExecuteFunctionState::GetFunctionState(state);
}

idx_t LazyExecuteInput::ChildCount() const {
	return expr.GetChildren().size();
}

void LazyExecuteInput::Execute(idx_t child, optional_ptr<const SelectionVector> child_sel, idx_t child_count,
                               Vector &result) {
	D_ASSERT(child < ChildCount());
	executor.Execute(*expr.GetChildren()[child], state.child_states[child].get(), child_sel.get(), child_count, result);
}

idx_t LazyExecuteInput::Select(idx_t child, optional_ptr<const SelectionVector> child_sel, idx_t child_count,
                               optional_ptr<SelectionVector> true_sel, optional_ptr<SelectionVector> false_sel) {
	D_ASSERT(child < ChildCount());
	return executor.Select(*expr.GetChildren()[child], state.child_states[child].get(), child_sel.get(), child_count,
	                       true_sel.get(), false_sel.get());
}

Vector &LazyExecuteInput::ChildBuffer(idx_t child) {
	D_ASSERT(child < ChildCount());
	return state.intermediate_chunk.data[child];
}

void LazyExecuteInput::Fill(const Vector &source, Vector &result, const SelectionVector &fill_sel, idx_t fill_count) {
	executor.FillSwitch(source, result, fill_sel, NumericCast<sel_t>(fill_count));
}

void LazyExecuteInput::Compact(Vector &result) {
	if (sel) {
		result.Slice(*sel, count);
	}
}

} // namespace duckdb
