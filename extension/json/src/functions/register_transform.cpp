// Transforming JSON into SQL values.
//
// To port: legacy/json_functions/json_transform.cpp (json_transform, from_json, json_transform_strict,
// from_json_strict, and the casts from JSON to nested types: RegisterJSONTransformCastFunctions).

#include "json_functions.hpp"

namespace duckdb {
namespace json {

void RegisterTransform(Extension &extension, Context &context) {
}

} // namespace json
} // namespace duckdb
