// json_array_length

#include "json_executors.hpp"

namespace duckdb {
namespace json {

static inline std::optional<uint64_t> GetArrayLength(yyjson_val *val, yyjson_alc *) {
	return yyjson_arr_size(val);
}

static void UnaryArrayLengthFunction(ScalarFunction::ExecInput &input) {
	JSONExecutors::UnaryExecute<uint64_t>(input, GetArrayLength);
}

static void BinaryArrayLengthFunction(ScalarFunction::ExecInput &input) {
	JSONExecutors::BinaryExecute<uint64_t>(input, GetArrayLength);
}

static void ManyArrayLengthFunction(ScalarFunction::ExecInput &input) {
	JSONExecutors::ExecuteMany<uint64_t>(input, GetArrayLength);
}

void RegisterJSONArrayLength(Extension &extension, Context &context) {
	auto ubigint = context.GetFactory().CreateType(LogicalTypeId::UBIGINT);
	RegisterJSONReadFunctions(extension, context, {"json_array_length"}, ubigint, UnaryArrayLengthFunction,
	                          BinaryArrayLengthFunction, ManyArrayLengthFunction, false, true);
}

} // namespace json
} // namespace duckdb
