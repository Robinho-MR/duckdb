// Ports legacy/json_functions/json_value.cpp

#include "json_executors.hpp"

namespace duckdb {
namespace json {

//! Just like json_extract, but NULL if the JSON is not a scalar value
static void ValueFunction(ScalarFunction::ExecInput &input) {
	JSONExecutors::BinaryExecute<string_view>(input, JSONCommon::JSONValue);
}

static void ValueManyFunction(ScalarFunction::ExecInput &input) {
	JSONExecutors::ExecuteMany<string_view>(input, JSONCommon::JSONValue);
}

void RegisterJSONValue(Extension &extension, Context &context) {
	auto varchar = context.GetFactory().CreateType(LogicalTypeId::VARCHAR);
	RegisterJSONReadFunctions(extension, context, {"json_value"}, varchar, nullptr, ValueFunction, ValueManyFunction,
	                          true, true);
}

} // namespace json
} // namespace duckdb
