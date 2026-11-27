// json_merge_patch

#include "json_merge_helpers.hpp"

namespace duckdb {
namespace json {

//! RFC 7396 merge patch; a missing or non-object orig is treated as an empty object
static inline yyjson_mut_val *MergePatch(yyjson_mut_doc *doc, yyjson_mut_val *orig, yyjson_mut_val *patch) {
	if ((yyjson_mut_get_tag(orig) != (YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE)) ||
	    (yyjson_mut_get_tag(patch) != (YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE))) {
		// If either is not an object, we just return the second argument
		return patch;
	}

	// Both are object, do the merge
	auto root_builder = yyjson_mut_obj(doc);

	struct stack_item {
		yyjson_mut_val *key;
		yyjson_mut_val *orig;
		yyjson_mut_val *patch;
		yyjson_mut_val *builder;
	};
	vector<stack_item> stack;
	stack.push_back(stack_item {nullptr, orig, patch, root_builder});

	while (!stack.empty()) {
		auto nodes = stack.back();
		stack.pop_back();

		if (!yyjson_mut_is_obj(nodes.orig)) {
			// yyjson_mut_obj_getn on a non-object returns nullptr, so lookups below need no special case
			nodes.orig = nullptr;
		}

		// Copy orig keys that the patch does not touch
		if (nodes.orig) {
			size_t idx, max;
			yyjson_mut_val *key, *orig_val;
			yyjson_mut_obj_foreach(nodes.orig, idx, max, key, orig_val) {
				auto patch_val =
				    yyjson_mut_obj_getn(nodes.patch, unsafe_yyjson_get_str(key), unsafe_yyjson_get_len(key));
				if (!patch_val) {
					yyjson_mut_obj_add(nodes.builder, JSONCommon::MutValMutCopy(doc, key),
					                   JSONCommon::MutValMutCopy(doc, orig_val));
				}
			}
		}

		// Merge patch keys; null removes the key
		size_t idx, max;
		yyjson_mut_val *key, *patch_val;
		yyjson_mut_obj_foreach(nodes.patch, idx, max, key, patch_val) {
			if (unsafe_yyjson_is_null(patch_val)) {
				continue;
			}
			auto mut_key = JSONCommon::MutValMutCopy(doc, key);
			if (!yyjson_mut_is_obj(patch_val)) {
				yyjson_mut_obj_add(nodes.builder, mut_key, JSONCommon::MutValMutCopy(doc, patch_val));
				continue;
			}
			auto orig_val = yyjson_mut_obj_getn(nodes.orig, unsafe_yyjson_get_str(key), unsafe_yyjson_get_len(key));
			auto child_builder = yyjson_mut_obj(doc);
			yyjson_mut_obj_add(nodes.builder, mut_key, child_builder);
			stack.push_back(stack_item {mut_key, orig_val, patch_val, child_builder});
		}
	}

	return root_builder;
}

//! Follows MySQL behaviour
static void MergePatchFunction(ScalarFunction::ExecInput &input) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(input);
	auto alc = lstate.json_allocator.GetYYAlc();

	auto doc = JSONCommon::CreateDocument(alc);
	const auto count = input.GetRowCount();

	// Read the first json arg
	auto origs = JSONCommon::AllocateArray<yyjson_mut_val *>(alc, count);
	ReadMutableObjects(doc, input.GetArg(0), count, origs);

	// Read the next json args one by one and merge them into the first json arg
	auto patches = JSONCommon::AllocateArray<yyjson_mut_val *>(alc, count);
	for (idx_t arg_idx = 1; arg_idx < input.GetArgCount(); arg_idx++) {
		ReadMutableObjects(doc, input.GetArg(arg_idx), count, patches);
		for (idx_t i = 0; i < count; i++) {
			if (patches[i] == nullptr) {
				// Next json arg is NULL, obj becomes NULL
				origs[i] = nullptr;
			} else if (origs[i] == nullptr) {
				// Current obj is NULL, obj becomes next json arg
				origs[i] = patches[i];
			} else {
				// Neither is NULL, merge them
				origs[i] = MergePatch(doc, origs[i], patches[i]);
			}
		}
	}

	auto result = input.GetResult();
	WriteMutableObjects(result, count, origs, alc);
}

void RegisterJSONMergePatch(Extension &extension, Context &context) {
	RegisterJSONFold(extension, context, "json_merge_patch", MergePatchFunction, FunctionFallibility::INFALLIBLE);
}

} // namespace json
} // namespace duckdb
