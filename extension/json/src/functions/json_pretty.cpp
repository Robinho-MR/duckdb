// json_pretty

#include "json_executors.hpp"

namespace duckdb {
namespace json {

//! Pretty print a given JSON document
static std::optional<string_view> PrettyPrint(yyjson_val *val, yyjson_alc *alc) {
	D_ASSERT(alc);
	size_t len;
	auto data = yyjson_val_write_opts(val, JSONCommon::WRITE_PRETTY_FLAG, alc, &len, nullptr);
	return string_view(data, len);
}

static void PrettyPrintFunction(ScalarFunction::ExecInput &input) {
	JSONExecutors::UnaryExecute<string_view>(input, PrettyPrint);
}

void RegisterJSONPretty(Extension &extension, Context &context) {
	auto &factory = context.GetFactory();
	RegisterJSONReadFunction(extension, context, {"json_pretty"}, JSONReadKind::UNARY, JSONType(factory),
	                         factory.CreateType(LogicalTypeId::VARCHAR), PrettyPrintFunction,
	                         FunctionFallibility::INFALLIBLE);
}

} // namespace json
} // namespace duckdb
