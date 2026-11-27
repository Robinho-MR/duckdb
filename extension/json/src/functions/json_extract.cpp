// json_extract, json_extract_path, json_extract_string,
// json_extract_path_text and the ->> operator

#include "json_executors.hpp"

namespace duckdb {
namespace json {

static inline std::optional<string_view> ExtractFromVal(yyjson_val *val, yyjson_alc *alc) {
	return JSONCommon::WriteVal<yyjson_val>(val, alc);
}

static inline std::optional<string_view> ExtractStringFromVal(yyjson_val *val, yyjson_alc *alc) {
	switch (yyjson_get_tag(val)) {
	case YYJSON_TYPE_NULL | YYJSON_SUBTYPE_NONE:
		return std::nullopt;
	case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NOESC:
	case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NONE:
		return string_view(unsafe_yyjson_get_str(val), unsafe_yyjson_get_len(val));
	default:
		return JSONCommon::WriteVal<yyjson_val>(val, alc);
	}
}

static void ExtractFunction(ScalarFunction::ExecInput &input) {
	JSONExecutors::BinaryExecute<string_view>(input, ExtractFromVal);
}

static void ExtractManyFunction(ScalarFunction::ExecInput &input) {
	JSONExecutors::ExecuteMany<string_view>(input, ExtractFromVal);
}

static void ExtractStringFunction(ScalarFunction::ExecInput &input) {
	JSONExecutors::BinaryExecute<string_view>(input, ExtractStringFromVal);
}

static void ExtractStringManyFunction(ScalarFunction::ExecInput &input) {
	JSONExecutors::ExecuteMany<string_view>(input, ExtractStringFromVal);
}

void RegisterJSONExtract(Extension &extension, Context &context) {
	auto &factory = context.GetFactory();
	RegisterJSONReadFunctions(extension, context, {"json_extract", "json_extract_path"}, JSONType(factory), nullptr,
	                          ExtractFunction, ExtractManyFunction, true, true);
	RegisterJSONReadFunctions(extension, context, {"json_extract_string", "json_extract_path_text", "->>"},
	                          factory.CreateType(LogicalTypeId::VARCHAR), nullptr, ExtractStringFunction,
	                          ExtractStringManyFunction, true, true);
}

} // namespace json
} // namespace duckdb
