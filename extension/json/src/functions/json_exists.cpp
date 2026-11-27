// json_exists

#include "json_executors.hpp"

namespace duckdb {
namespace json {

static inline std::optional<bool> JSONExists(yyjson_val *val, yyjson_alc *) {
	return val != nullptr;
}

static void BinaryExistsFunction(ScalarFunction::ExecInput &input) {
	JSONExecutors::BinaryExecute<bool, false>(input, JSONExists);
}

static void ManyExistsFunction(ScalarFunction::ExecInput &input) {
	JSONExecutors::ExecuteMany<bool, false>(input, JSONExists);
}

void RegisterJSONExists(Extension &extension, Context &context) {
	auto boolean = context.GetFactory().CreateType(LogicalTypeId::BOOLEAN);
	RegisterJSONReadFunctions(extension, context, {"json_exists"}, boolean, nullptr, BinaryExistsFunction,
	                          ManyExistsFunction, false, false);
}

} // namespace json
} // namespace duckdb
