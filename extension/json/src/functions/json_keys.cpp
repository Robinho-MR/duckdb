// json_keys

#include "json_executors.hpp"

namespace duckdb {
namespace json {

using KeysResult = ListResult<string_view>;

static inline std::optional<KeysResult> GetJSONKeys(yyjson_val *val, yyjson_alc *) {
	KeysResult keys;
	keys.items.reserve(yyjson_obj_size(val));
	size_t idx, max;
	yyjson_val *key, *child_val;
	yyjson_obj_foreach(val, idx, max, key, child_val) {
		keys.items.emplace_back(string_view(unsafe_yyjson_get_str(key), unsafe_yyjson_get_len(key)));
	}
	return keys;
}

static void UnaryJSONKeysFunction(ScalarFunction::ExecInput &input) {
	JSONExecutors::UnaryExecute<KeysResult>(input, GetJSONKeys);
}

static void BinaryJSONKeysFunction(ScalarFunction::ExecInput &input) {
	JSONExecutors::BinaryExecute<KeysResult>(input, GetJSONKeys);
}

static void ManyJSONKeysFunction(ScalarFunction::ExecInput &input) {
	JSONExecutors::ExecuteMany<KeysResult>(input, GetJSONKeys);
}

void RegisterJSONKeys(Extension &extension, Context &context) {
	auto &factory = context.GetFactory();
	auto keys = ListOf(factory, factory.CreateType(LogicalTypeId::VARCHAR));
	RegisterJSONReadFunctions(extension, context, {"json_keys"}, keys, UnaryJSONKeysFunction, BinaryJSONKeysFunction,
	                          ManyJSONKeysFunction, false, true);
}

} // namespace json
} // namespace duckdb
