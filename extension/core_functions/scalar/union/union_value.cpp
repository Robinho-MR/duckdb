#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "core_functions/scalar/union_functions.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"

namespace duckdb {

namespace {

void UnionValueFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	// a single-member UNION stores the argument as a VARIANT
	VectorOperations::DefaultCast(args.data[0], result, args.size());
}

unique_ptr<FunctionData> UnionValueBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	if (arguments.size() != 1) {
		throw BinderException("union_value takes exactly one argument");
	}
	if (TypeVisitor::Contains(arguments[0]->GetReturnType(), LogicalTypeId::TYPE)) {
		throw BinderException("union_value cannot take a value of type %s", arguments[0]->GetReturnType());
	}
	child_list_t<LogicalType> union_members;
	union_members.emplace_back(make_pair(input.GetArgumentName(0), arguments[0]->GetReturnType()));

	bound_function.SetReturnType(LogicalType::UNION(std::move(union_members)));
	return make_uniq<VariableReturnBindData>(bound_function.GetReturnType());
}

unique_ptr<ParsedExpression> UnionValueUnbind(FunctionUnbindInput &input) {
	auto &expression = input.expression;
	D_ASSERT(input.children.size() == 1);
	D_ASSERT(UnionType::GetMemberCount(expression.GetReturnType()) == 1);
	vector<FunctionArgument> arguments;
	arguments.emplace_back(UnionType::GetMemberName(expression.GetReturnType(), 0), std::move(input.children[0]));
	return make_uniq<FunctionExpression>(expression.Function().GetDefinition()->GetQualifiedName(),
	                                     std::move(arguments));
}

} // namespace

ScalarFunction UnionValueFun::GetFunction() {
	ScalarFunction fun("union_value", {}, LogicalType::VARIANT(), UnionValueFunction, UnionValueBind, nullptr, nullptr);
	fun.GetSignature().AddKwargs("kwargs", LogicalType::ANY);
	fun.GetProperties().SetRequiresExpressionNames(true);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetSerializeCallback(VariableReturnBindData::Serialize);
	fun.SetDeserializeCallback(VariableReturnBindData::Deserialize);
	fun.SetUnbindCallback(UnionValueUnbind);
	return fun;
}

} // namespace duckdb
