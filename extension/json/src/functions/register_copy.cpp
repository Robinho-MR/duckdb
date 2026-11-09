// COPY ... (FORMAT json) and GeoJSON.
//
// To port: legacy/json_functions/copy_json.cpp (json, ndjson, jsonl copy functions, the json_copy_strftime_* macros of
// legacy/json_extension.cpp), legacy/json_functions/json_geojson.cpp (geojson, geojsonl copy functions, ST_AsGeoJSON,
// ST_GeomFromGeoJSON).

#include "json_functions.hpp"

namespace duckdb {
namespace json {

void RegisterCopy(Extension &extension, Context &context) {
}

} // namespace json
} // namespace duckdb
