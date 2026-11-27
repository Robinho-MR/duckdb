#include "duckdb/planner/expression/bound_coalesce_expression.hpp"

#include "duckdb/common/bit_utils.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/function/lazy_execute_input.hpp"
#include "duckdb/function/scalar/generic_functions.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

namespace {

struct CoalesceLocalState : public FunctionLocalState {
	void Prepare(idx_t count) {
		for (auto &selection : selections) {
			if (selection.Capacity() < count) {
				selection.Initialize(NextPowerOfTwo(count));
			}
		}
	}

	SelectionVector selections[4];
};

unique_ptr<FunctionLocalState> CoalesceInitLocalState(ExpressionState &state, const BoundFunctionExpression &expr,
                                                      optional_ptr<const FunctionData> bind_data) {
	return make_uniq<CoalesceLocalState>();
}

//! Executes every child only on the rows for which all earlier children were NULL
void CoalesceFunction(LazyExecuteInput &input, Vector &result) {
	auto &local_state = input.LocalState()->Cast<CoalesceLocalState>();
	const auto sel = input.Sel();
	const auto count = input.Count();
	local_state.Prepare(count);
	auto &sel_a = local_state.selections[0];
	auto &sel_b = local_state.selections[1];
	auto &slice_sel = local_state.selections[2];
	auto &result_sel = local_state.selections[3];
	SelectionVector *next_sel = &sel_a;
	optional_ptr<const SelectionVector> current_sel = sel;
	idx_t remaining_count = count;
	for (idx_t child = 0; child < input.ChildCount(); child++) {
		auto &vector_to_check = input.ChildBuffer(child);
		input.Execute(child, current_sel, remaining_count, vector_to_check);

		auto entries = vector_to_check.Validity();
		idx_t result_count = 0;
		idx_t next_count = 0;
		for (idx_t i = 0; i < remaining_count; i++) {
			auto base_idx = current_sel ? current_sel->get_index(i) : i;
			if (entries.IsValid(i)) {
				slice_sel.set_index(result_count, i);
				result_sel.set_index(result_count++, base_idx);
			} else {
				next_sel->set_index(next_count++, base_idx);
			}
		}
		if (result_count > 0) {
			vector_to_check.Slice(slice_sel, result_count);
			input.Fill(vector_to_check, result, result_sel, result_count);
		}
		current_sel = next_sel;
		next_sel = next_sel == &sel_a ? &sel_b : &sel_a;
		remaining_count = next_count;
		if (next_count == 0) {
			break;
		}
	}
	for (idx_t i = 0; i < remaining_count; i++) {
		FlatVector::SetNull(result, current_sel->get_index(i), true);
	}
	if (sel) {
		input.Compact(result);
	} else if (count == 1) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

string CoalesceToString(FunctionToStringInput &input) {
	string result = "COALESCE(";
	for (idx_t i = 0; i < input.children.size(); i++) {
		if (i > 0) {
			result += ", ";
		}
		result += input.children[i]->ToString();
	}
	return result + ")";
}

unique_ptr<FunctionData> CoalesceBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	if (arguments.size() < 2) {
		throw BinderException("%s requires at least two arguments", InternalCoalesceFun::Name);
	}
	auto &return_type = arguments[0]->GetReturnType();
	for (auto &argument : arguments) {
		if (argument->GetReturnType() != return_type) {
			throw BinderException("%s requires all arguments to have the same type", InternalCoalesceFun::Name);
		}
	}
	bound_function.SetReturnType(return_type);
	return make_uniq<VariableReturnBindData>(bound_function.GetReturnType());
}

ScalarFunction GetCoalesceFunction(const LogicalType &return_type) {
	ScalarFunction fun(InternalCoalesceFun::Name, {}, return_type, scalar_function_t());
	fun.GetSignature().AddArgs("args", LogicalType::ANY);
	fun.SetLazyExecuteCallback(CoalesceFunction);
	fun.SetBindCallback(CoalesceBind);
	fun.SetInitStateCallback(CoalesceInitLocalState);
	fun.SetToStringCallback(CoalesceToString);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

} // namespace

ScalarFunction InternalCoalesceFun::GetFunction() {
	return GetCoalesceFunction(LogicalType::ANY);
}

bool BoundCoalesceExpression::IsCoalesce(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	return expr.Cast<BoundFunctionExpression>().Function().GetName() == InternalCoalesceFun::Name;
}

unique_ptr<Expression> BoundCoalesceExpression::Create(const LogicalType &return_type,
                                                       vector<unique_ptr<Expression>> children) {
	D_ASSERT(children.size() >= 2);
	return make_uniq<BoundFunctionExpression>(BoundScalarFunction(GetCoalesceFunction(return_type)),
	                                          std::move(children), make_uniq<VariableReturnBindData>(return_type));
}

unique_ptr<Expression> BoundCoalesceExpression::Create(const LogicalType &return_type, unique_ptr<Expression> lhs,
                                                       unique_ptr<Expression> rhs) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(lhs));
	children.push_back(std::move(rhs));
	return Create(return_type, std::move(children));
}

} // namespace duckdb
