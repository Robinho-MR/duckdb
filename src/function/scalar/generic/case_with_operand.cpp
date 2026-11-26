#include "duckdb/function/scalar/case_with_operand.hpp"

#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/function/lazy_execute_input.hpp"
#include "duckdb/function/scalar/generic_functions.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

namespace {

struct CaseWithOperandState : public FunctionLocalState {
	CaseWithOperandState() : true_sel(STANDARD_VECTOR_SIZE), false_sel(STANDARD_VECTOR_SIZE) {
	}

	SelectionVector true_sel;
	SelectionVector false_sel;
};

unique_ptr<FunctionLocalState> CaseWithOperandInitState(ExpressionState &state, const BoundFunctionExpression &expr,
                                                        optional_ptr<const FunctionData> bind_data) {
	return make_uniq<CaseWithOperandState>();
}

//! Executes the same way as a regular CASE (see ExpressionExecutor::Execute(BoundCaseExpression)), after evaluating
//! the operand once. The children are [operand, when_1, then_1, .., when_n, then_n, else]
void CaseWithOperandFunction(LazyExecuteInput &input, Vector &result) {
	auto &local_state = input.LocalState()->Cast<CaseWithOperandState>();
	auto sel = input.Sel();
	auto count = input.Count();

	// the operand is evaluated once, the conditions read the result through the operand references
	auto &operand = input.ChildBuffer(0);
	input.Execute(0, sel, count, operand);
	ScopedValueGuard operand_scope(input, operand);

	const idx_t check_count = (input.ChildCount() - 2) / 2;
	const idx_t else_child = input.ChildCount() - 1;
	auto current_true_sel = &local_state.true_sel;
	auto current_false_sel = &local_state.false_sel;
	auto current_sel = sel;
	idx_t current_count = count;
	for (idx_t i = 0; i < check_count; i++) {
		const idx_t when_child = 1 + i * 2;
		const idx_t then_child = when_child + 1;
		idx_t tcount = input.Select(when_child, current_sel, current_count, current_true_sel, current_false_sel);
		if (tcount == 0) {
			// everything is false: do nothing
			continue;
		}
		idx_t fcount = current_count - tcount;
		if (fcount == 0 && current_count == count) {
			// everything is true in the first check: only execute the true side
			input.Execute(then_child, sel, count, result);
			return;
		}
		// execute the true side and fill in the desired rows in the result
		auto &then_result = input.ChildBuffer(then_child);
		input.Execute(then_child, current_true_sel, tcount, then_result);
		input.Fill(then_result, result, *current_true_sel, tcount);
		// continue with the false rows
		current_sel = current_false_sel;
		current_count = fcount;
		if (fcount == 0) {
			break;
		}
	}
	if (current_count > 0) {
		if (current_count == count) {
			// everything was false: execute the else expression directly
			input.Execute(else_child, sel, count, result);
			return;
		}
		auto &else_result = input.ChildBuffer(else_child);
		input.Execute(else_child, current_sel, current_count, else_result);
		input.Fill(else_result, result, *current_sel, current_count);
	}
	input.Compact(result);
}

void CaseOperandReferenceFunction(LazyExecuteInput &input, Vector &result) {
	input.ReferenceScopedValue(result);
}

string CaseWithOperandToString(FunctionToStringInput &input) {
	auto &children = input.children;
	string result = "CASE " + children[0]->ToString();
	for (idx_t i = 1; i + 1 < children.size(); i += 2) {
		result += " WHEN (" + children[i]->ToString() + ") THEN (" + children[i + 1]->ToString() + ")";
	}
	return result + " ELSE " + children.back()->ToString() + " END";
}

string CaseOperandReferenceToString(FunctionToStringInput &input) {
	return "case_operand";
}

unique_ptr<FunctionData> CaseWithOperandBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	if (arguments.size() < 4 || arguments.size() % 2 != 0) {
		throw InternalException("CASE with operand requires an operand, one or more checks and an ELSE expression");
	}
	bound_function.SetReturnType(arguments.back()->GetReturnType());
	return make_uniq<VariableReturnBindData>(bound_function.GetReturnType());
}

ScalarFunction GetCaseWithOperandFunction(const LogicalType &return_type) {
	ScalarFunction fun(InternalCaseWithOperandFun::Name, {}, return_type, scalar_function_t());
	fun.GetSignature().AddParameter("operand", LogicalType::ANY).AddArgs("args", LogicalType::ANY);
	fun.SetLazyExecuteCallback(CaseWithOperandFunction);
	fun.SetBindCallback(CaseWithOperandBind);
	fun.SetInitStateCallback(CaseWithOperandInitState);
	fun.SetToStringCallback(CaseWithOperandToString);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	// the children can throw
	fun.SetFallible();
	return fun;
}

ScalarFunction GetCaseOperandReferenceFunction(const LogicalType &return_type) {
	ScalarFunction fun(InternalCaseOperandFun::Name, {}, return_type, scalar_function_t());
	fun.SetLazyExecuteCallback(CaseOperandReferenceFunction);
	fun.SetToStringCallback(CaseOperandReferenceToString);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	// the value depends on the CASE the reference is part of: it can not be folded or shared with other expressions
	fun.SetVolatile();
	return fun;
}

} // namespace

ScalarFunction InternalCaseWithOperandFun::GetFunction() {
	return GetCaseWithOperandFunction(LogicalType::ANY);
}

ScalarFunction InternalCaseOperandFun::GetFunction() {
	return GetCaseOperandReferenceFunction(LogicalType::ANY);
}

unique_ptr<Expression> CaseWithOperand::Create(unique_ptr<Expression> operand,
                                               vector<pair<unique_ptr<Expression>, unique_ptr<Expression>>> checks,
                                               unique_ptr<Expression> else_expr) {
	auto return_type = else_expr->GetReturnType();
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(operand));
	for (auto &check : checks) {
		children.push_back(std::move(check.first));
		children.push_back(std::move(check.second));
	}
	children.push_back(std::move(else_expr));
	return make_uniq<BoundFunctionExpression>(BoundScalarFunction(GetCaseWithOperandFunction(return_type)),
	                                          std::move(children), make_uniq<VariableReturnBindData>(return_type));
}

unique_ptr<Expression> CaseWithOperand::CreateOperandReference(const LogicalType &operand_type) {
	return make_uniq<BoundFunctionExpression>(BoundScalarFunction(GetCaseOperandReferenceFunction(operand_type)),
	                                          vector<unique_ptr<Expression>>(), nullptr);
}

} // namespace duckdb
