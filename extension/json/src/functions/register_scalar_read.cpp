// Read-only scalar functions.
//
// json_type, json_valid, json_exists, json_extract (+ json_extract_path,
// json_extract_string, json_extract_path_text, ->>), json_value, json_array_length, json_keys, json_contains and
// json_pretty.

#include "json_functions.hpp"

namespace duckdb {
namespace json {

//! functions/<name>.cpp
void RegisterJSONKeys(Extension &extension, Context &context);
void RegisterJSONContains(Extension &extension, Context &context);
void RegisterJSONPretty(Extension &extension, Context &context);

void RegisterScalarRead(Extension &extension, Context &context) {
	RegisterJSONTypeFunction(extension, context);
	RegisterJSONValid(extension, context);
	RegisterJSONExists(extension, context);
	RegisterJSONExtract(extension, context);
	RegisterJSONValue(extension, context);
	RegisterJSONArrayLength(extension, context);
	RegisterJSONKeys(extension, context);
	RegisterJSONContains(extension, context);
	RegisterJSONPretty(extension, context);
}

} // namespace json
} // namespace duckdb
