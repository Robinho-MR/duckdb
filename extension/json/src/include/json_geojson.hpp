//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_geojson.hpp
//
// GeoJSON <-> WKB. GEOMETRY values are stored as (little endian, ISO) WKB, which is what these functions read and
// write.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "json_create.hpp"

namespace duckdb {
namespace json {

//! The bounding box of a geometry. A geometry without a finite XY extent (empty) has none
struct JSONGeometryExtent {
	double x_min = 0, x_max = 0, y_min = 0, y_max = 0, z_min = 0, z_max = 0;
	bool has_xy = false;
	bool has_z = false;
};

//! Converts WKB to a GeoJSON geometry object in the document. Throws InvalidInputException on malformed WKB
yyjson_mut_val *WKBToGeoJSON(yyjson_mut_doc *doc, std::string_view wkb);

//! Converts a GeoJSON geometry object to WKB. Returns false and sets `error` if it is not a valid GeoJSON geometry
bool GeoJSONToWKB(yyjson_val *val, string &wkb, string &error);

//! Whether the value looks like a GeoJSON geometry object (a known "type" and its coordinates/geometries member)
bool IsGeoJSONGeometry(yyjson_val *val);

//! The extent of a geometry. Throws InvalidInputException on malformed WKB
JSONGeometryExtent GetWKBExtent(std::string_view wkb);

//! Registers st_asgeojson, st_geomfromgeojson and the json_geometry_format setting, and installs the GeoJSON hooks of
//! the casts from and to JSON
void RegisterGeoJSON(Extension &extension, Context &context);

//! The json_geometry_format value (anything but 'wkt' / 'geojson' throws an InvalidInputException)
JSONGeometryFormat ParseJSONGeometryFormat(const Value &value);

} // namespace json
} // namespace duckdb
