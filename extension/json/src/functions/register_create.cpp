// Creating JSON from SQL values.
//
// To port: legacy/json_functions/json_create.cpp (json_array, json_object, to_json/json_quote, array_to_json,
// row_to_json, the casts to JSON from nested types: RegisterJSONCreateCastFunctions), the json_group_array and
// json_group_object macros of legacy/json_extension.cpp, and the JSON_MACROS that depend on to_json.

#include "json_functions.hpp"

namespace duckdb {
namespace json {

void RegisterCreate(Extension &extension, Context &context) {
}

} // namespace json
} // namespace duckdb
