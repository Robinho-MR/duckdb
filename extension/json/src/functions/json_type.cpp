// Ports legacy/json_functions/json_type.cpp

#include "json_executors.hpp"

namespace duckdb {
namespace json {

static inline std::optional<string_view> GetType(yyjson_val *val, yyjson_alc *) {
	return JSONCommon::ValTypeToStringView(val);
}

static void UnaryTypeFunction(ScalarFunction::ExecInput &input) {
	JSONExecutors::UnaryExecute<string_view>(input, GetType);
}

static void BinaryTypeFunction(ScalarFunction::ExecInput &input) {
	JSONExecutors::BinaryExecute<string_view>(input, GetType);
}

static void ManyTypeFunction(ScalarFunction::ExecInput &input) {
	JSONExecutors::ExecuteMany<string_view>(input, GetType);
}

void RegisterJSONTypeFunction(Extension &extension, Context &context) {
	auto varchar = context.GetFactory().CreateType(LogicalTypeId::VARCHAR);
	RegisterJSONReadFunctions(extension, context, {"json_type"}, varchar, UnaryTypeFunction, BinaryTypeFunction,
	                          ManyTypeFunction, true, true);
}

} // namespace json
} // namespace duckdb
