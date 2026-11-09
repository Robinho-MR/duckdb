// Reading JSON files.
//
// To port: legacy/json_reader.cpp, legacy/json_scan.cpp, legacy/json_functions/read_json.cpp,
// legacy/json_functions/read_single_json_file.cpp (read_json, read_ndjson, read_json_objects, read_json_auto, ...), the
// replacement scan (ReadJSONReplacement in legacy/json_functions.cpp) and the extension's settings
// (json_geometry_format in legacy/json_extension.cpp).

#include "json_functions.hpp"

namespace duckdb {
namespace json {

void RegisterReader(Extension &extension, Context &context) {
}

} // namespace json
} // namespace duckdb
