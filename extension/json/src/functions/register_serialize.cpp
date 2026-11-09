// Serializing SQL and query plans as JSON.
//
// To port (or to move into the engine, see api_spec/proposals/json-extension-stable-port.md section 2.4):
// legacy/json_functions/json_serialize_sql.cpp (json_serialize_sql, json_deserialize_sql, json_execute_serialized_sql),
// legacy/json_functions/json_serialize_plan.cpp, legacy/json_serializer.cpp, legacy/json_deserializer.cpp,
// legacy/serialize_json.cpp, legacy/json_enums.cpp. These use engine internals that the C++ API does not expose.

#include "json_functions.hpp"

namespace duckdb {
namespace json {

void RegisterSerialize(Extension &extension, Context &context) {
}

} // namespace json
} // namespace duckdb
