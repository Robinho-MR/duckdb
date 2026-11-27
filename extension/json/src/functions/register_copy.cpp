// COPY ... TO ... (FORMAT json | ndjson | jsonl | geojson | geojsonl), GeoJSON and the json_geometry_format setting.
//
// COPY ... FROM is not here: reading goes through the reader (register_reader.cpp), which has to add its copy-from
// callbacks to the copy functions registered below.

#include "json_geojson.hpp"

namespace duckdb {
namespace json {

//! functions/json_copy.cpp
void RegisterJSONCopy(Extension &extension, Context &context);

void RegisterCopy(Extension &extension, Context &context) {
	RegisterGeoJSON(extension, context);
	RegisterJSONCopy(extension, context);
}

} // namespace json
} // namespace duckdb
