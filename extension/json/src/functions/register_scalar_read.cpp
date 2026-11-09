// Read-only scalar functions (WP1 owns the rest of this file).
//
// Ports (legacy/json_functions/): json_type, json_valid, json_exists, json_extract (+ json_extract_path,
// json_extract_string, json_extract_path_text, ->>), json_value, json_array_length are done. Still to port:
// json_contains.cpp, json_keys.cpp, json_pretty.cpp (and anything else that only reads a document).

#include "json_functions.hpp"

namespace duckdb {
namespace json {

void RegisterScalarRead(Extension &extension, Context &context) {
	RegisterJSONTypeFunction(extension, context);
	RegisterJSONValid(extension, context);
	RegisterJSONExists(extension, context);
	RegisterJSONExtract(extension, context);
	RegisterJSONValue(extension, context);
	RegisterJSONArrayLength(extension, context);
}

} // namespace json
} // namespace duckdb
