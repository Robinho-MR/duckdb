// Ports legacy/json_functions/json_normalize.cpp

#include "json_functions.hpp"

namespace duckdb {
namespace json {

//! Sort all object keys alphabetically (byte-order) in a mutable JSON value
static void SortKeys(yyjson_mut_val *v) {
	vector<yyjson_mut_val *> stack;
	stack.push_back(v);

	while (!stack.empty()) {
		auto val = stack.back();
		stack.pop_back();

		if (!val) {
			continue;
		}
		if (yyjson_mut_is_obj(val)) {
			idx_t size = yyjson_mut_obj_size(val);
			if (size <= 1) {
				if (size == 1) {
					yyjson_mut_obj_iter iter;
					yyjson_mut_obj_iter_init(val, &iter);
					yyjson_mut_val *key = yyjson_mut_obj_iter_next(&iter);
					stack.push_back(yyjson_mut_obj_iter_get_val(key));
				}
				continue;
			}

			// Extract key-value pairs
			vector<std::pair<yyjson_mut_val *, yyjson_mut_val *>> pairs;
			pairs.reserve(size);
			size_t idx, max;
			yyjson_mut_val *key, *child_val;
			yyjson_mut_obj_foreach(val, idx, max, key, child_val) {
				pairs.emplace_back(key, child_val);
			}

			// Sort by key string (byte order)
			std::sort(pairs.begin(), pairs.end(),
			          [](const std::pair<yyjson_mut_val *, yyjson_mut_val *> &a,
			             const std::pair<yyjson_mut_val *, yyjson_mut_val *> &b) {
				          const string_view a_key(unsafe_yyjson_get_str(a.first), unsafe_yyjson_get_len(a.first));
				          const string_view b_key(unsafe_yyjson_get_str(b.first), unsafe_yyjson_get_len(b.first));
				          return a_key < b_key;
			          });

			// Recursively sort nested values
			for (auto &pair : pairs) {
				stack.push_back(pair.second);
			}

			// Clear and rebuild in sorted order
			yyjson_mut_obj_clear(val);
			for (auto &pair : pairs) {
				yyjson_mut_obj_add(val, pair.first, pair.second);
			}
		} else if (yyjson_mut_is_arr(val)) {
			size_t idx, max;
			yyjson_mut_val *elem;
			yyjson_mut_arr_foreach(val, idx, max, elem) {
				stack.push_back(elem);
			}
		}
	}
}

//! Normalize JSON: sort all object keys recursively, serialize compact
static void NormalizeFunction(ScalarFunction::ExecInput &input) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(input);
	auto alc = lstate.json_allocator.GetYYAlc();
	const auto count = input.GetRowCount();

	ValueView<varchar_t> inputs(input.GetArg(0));
	auto result = input.GetResult();
	StringWriter writer(result, count);
	for (idx_t i = 0; i < count; i++) {
		if (!inputs.IsValid(i)) {
			writer.WriteNull();
			continue;
		}

		auto doc = JSONCommon::ReadDocument(inputs.Get(i), JSONCommon::READ_FLAG, alc);
		auto mut_doc = yyjson_doc_mut_copy(doc, alc);
		auto root = yyjson_mut_doc_get_root(mut_doc);

		SortKeys(root);

		writer.WriteValue(JSONCommon::WriteVal<yyjson_mut_val>(root, alc));
	}
}

void RegisterJSONNormalize(Extension &extension, Context &context) {
	auto &factory = context.GetFactory();
	RegisterJSONReadFunction(extension, context, {"json_normalize"}, JSONReadKind::UNARY, JSONType(factory),
	                         factory.CreateType(LogicalTypeId::VARCHAR), NormalizeFunction,
	                         FunctionFallibility::INFALLIBLE);
}

} // namespace json
} // namespace duckdb
