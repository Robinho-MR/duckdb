//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_create.hpp
//
// Creating JSON from the rows of a vector of any type: the machinery behind to_json, json_object, json_array and the
// casts to JSON. COPY ... TO ... (FORMAT JSON) uses CreateJSONStrings with the date/timestamp formats of the copy.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "json_functions.hpp"
#include "json_strftime.hpp"

#include <functional>

namespace duckdb {
namespace json {

//! How a GEOMETRY value is represented in JSON. WKT is the default; COPY ... (FORMAT GEOJSON) selects GEOJSON
enum class JSONGeometryFormat : uint8_t { WKT, GEOJSON };

struct JSONCreateOptions {
	JSONGeometryFormat geometry_format = JSONGeometryFormat::WKT;
	//! strftime formats of DATE and of the TIMESTAMP types (nullptr: the default text representation)
	const JSONTimeFormat *date_format = nullptr;
	const JSONTimeFormat *timestamp_format = nullptr;
	//! Converts a GEOMETRY value (its WKB blob) to GeoJSON; needed when geometry_format is GEOJSON
	std::function<yyjson_mut_val *(yyjson_mut_doc *, blob_t)> geometry_to_geojson;
};

//! Registers json_object, json_array, to_json, json_quote, array_to_json, row_to_json and the casts to JSON
void RegisterCreateFunctions(Extension &extension, Context &context);

//! The options of a client: the geometry format comes from the json_geometry_format setting
JSONCreateOptions ClientJSONCreateOptions(Context &context);

//! Installs the conversion from a GEOMETRY value (its WKB) to GeoJSON that ClientJSONCreateOptions hands out
void SetGeometryToGeoJSON(yyjson_mut_val *(*converter)(yyjson_mut_doc *, blob_t));

//! Creates one JSON value per row for rows [0, count) of `input` (of any type) in the document, into `vals`. The rows
//! that are NULL (of nested values too) become JSON null. `input` may be flattened.
void CreateJSONValues(Context &context, yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &input, idx_t count,
                      const JSONCreateOptions &options);

//! Creates the JSON strings of rows [0, count) of `input` (of any type) into `result` (a JSON or VARCHAR vector, which
//! gets the size `count`). NULL rows stay NULL. `alc` is the arena that the JSON documents are built in: the caller
//! resets it between calls.
void CreateJSONStrings(Context &context, Vector &input, idx_t count, Vector &result, yyjson_alc *alc,
                       const JSONCreateOptions &options);

} // namespace json
} // namespace duckdb
