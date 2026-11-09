// Scalar functions that build a new document from one or more documents.
//
// To port (legacy/json_functions/): json_merge_patch.cpp, json_merge_patch_diff.cpp, json_deep_merge.cpp,
// json_modify.cpp (json_insert, json_replace, json_set, json_remove), json_strip_nulls.cpp, json_normalize.cpp.

#include "json_functions.hpp"

namespace duckdb {
namespace json {

void RegisterScalarModify(Extension &extension, Context &context) {
}

} // namespace json
} // namespace duckdb
