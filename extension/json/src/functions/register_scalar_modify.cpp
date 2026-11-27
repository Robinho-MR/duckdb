// Scalar functions that build a new document from one or more documents.
//
// json_merge_patch, json_merge_patch_diff, json_deep_merge, json_insert,
// json_replace, json_set, json_remove, json_strip_nulls and json_normalize.

#include "json_functions.hpp"

namespace duckdb {
namespace json {

//! functions/<name>.cpp
void RegisterJSONMergePatch(Extension &extension, Context &context);
void RegisterJSONMergePatchDiff(Extension &extension, Context &context);
void RegisterJSONDeepMerge(Extension &extension, Context &context);
void RegisterJSONModify(Extension &extension, Context &context);
void RegisterJSONStripNulls(Extension &extension, Context &context);
void RegisterJSONNormalize(Extension &extension, Context &context);

void RegisterScalarModify(Extension &extension, Context &context) {
	RegisterJSONMergePatch(extension, context);
	RegisterJSONMergePatchDiff(extension, context);
	RegisterJSONDeepMerge(extension, context);
	RegisterJSONModify(extension, context);
	RegisterJSONStripNulls(extension, context);
	RegisterJSONNormalize(extension, context);
}

} // namespace json
} // namespace duckdb
