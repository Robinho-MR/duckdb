// json_strip_nulls

#include "json_functions.hpp"

namespace duckdb {
namespace json {

//! Remove all object keys with null values
static void StripNulls(yyjson_mut_val *val) {
	vector<yyjson_mut_val *> stack;
	stack.push_back(val);

	while (!stack.empty()) {
		auto curr_val = stack.back();
		stack.pop_back();

		if (!curr_val) {
			return;
		}
		if (yyjson_mut_is_obj(curr_val)) {
			yyjson_mut_obj_iter iter;
			yyjson_mut_obj_iter_init(curr_val, &iter);
			yyjson_mut_val *key;
			while ((key = yyjson_mut_obj_iter_next(&iter)) != nullptr) {
				auto child = yyjson_mut_obj_iter_get_val(key);
				if (unsafe_yyjson_is_null(child)) {
					yyjson_mut_obj_iter_remove(&iter);
				} else {
					stack.push_back(child);
				}
			}
		} else if (yyjson_mut_is_arr(curr_val)) {
			size_t idx, max;
			yyjson_mut_val *elem;
			yyjson_mut_arr_foreach(curr_val, idx, max, elem) {
				stack.push_back(elem);
			}
		}
	}
}

//! Strip all null-valued keys from a JSON document recursively
static void StripNullsFunction(ScalarFunction::ExecInput &input) {
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
		StripNulls(root);
		writer.WriteValue(JSONCommon::WriteVal<yyjson_mut_val>(root, alc));
	}
}

void RegisterJSONStripNulls(Extension &extension, Context &context) {
	auto &factory = context.GetFactory();
	auto json = JSONType(factory);
	RegisterJSONReadFunction(extension, context, {"json_strip_nulls"}, JSONReadKind::UNARY, json, json,
	                         StripNullsFunction, FunctionFallibility::INFALLIBLE);
}

} // namespace json
} // namespace duckdb
