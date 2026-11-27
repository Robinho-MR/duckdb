// json_structure, json_group_structure. The schema inference of read_json (json_schema_inference.cpp) is a library
// used by the reader and registers nothing.

#include "json_functions.hpp"

namespace duckdb {
namespace json {

//! functions/json_structure.cpp
void RegisterJSONStructure(Extension &extension, Context &context);

void RegisterStructure(Extension &extension, Context &context) {
	RegisterJSONStructure(extension, context);
}

} // namespace json
} // namespace duckdb
