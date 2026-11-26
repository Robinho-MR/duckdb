#include "duckdb/function/lazy_execute_input.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

LazyExecuteInput::LazyExecuteInput(ExpressionExecutor &executor_p, const BoundFunctionExpression &expr_p,
                                   ExpressionState &state_p, optional_ptr<const SelectionVector> sel_p, idx_t count_p)
    : executor(executor_p), expr(expr_p), state(state_p), sel(sel_p), count(count_p) {
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

ScopedValueGuard::ScopedValueGuard(LazyExecuteInput &input, const Vector &value) : root(input.state.root) {
	optional_ptr<const SelectionVector> map;
	if (input.sel) {
		// the value is compact: map the physical rows of the selection to their position in the value
		row_map.Initialize(STANDARD_VECTOR_SIZE);
		for (idx_t i = 0; i < input.count; i++) {
			row_map.set_index(input.sel->get_index(i), i);
		}
		map = &row_map;
	}
	root.scoped_values.push_back(ScopedValue {value, map});
}

ScopedValueGuard::~ScopedValueGuard() {
	root.scoped_values.pop_back();
}

void LazyExecuteInput::ReferenceScopedValue(Vector &result) {
	auto &scopes = state.root.scoped_values;
	if (scopes.empty()) {
		throw InternalException("No value is in scope for function \"%s\"", expr.Function().GetName());
	}
	auto &scope = scopes.back();
	if (!sel && !scope.row_map) {
		result.Reference(scope.value);
		return;
	}
	if (!scope.row_map) {
		result.Slice(scope.value, *sel, count);
		return;
	}
	SelectionVector value_sel(count);
	for (idx_t i = 0; i < count; i++) {
		auto row = sel ? sel->get_index(i) : i;
		value_sel.set_index(i, scope.row_map->get_index(row));
	}
	result.Slice(scope.value, value_sel, count);
}

} // namespace duckdb
