#include "duckdb/function/scalar/serialize_functions.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

using namespace duckdb_yyjson; // NOLINT

string JSONSerializeFunctions::WriteDocument(yyjson_mut_val *root, bool format) {
	size_t len;
	auto data = yyjson_mut_val_write_opts(root, format ? WRITE_PRETTY_FLAG : WRITE_FLAG, nullptr, &len, nullptr);
	if (!data) {
		throw SerializationException("Failed to serialize json, perhaps the query contains invalid utf8 characters?");
	}
	string result(data, len);
	free(data);
	return result;
}

string JSONSerializeFunctions::WriteError(yyjson_mut_doc *doc, yyjson_mut_val *result_obj, const ErrorData &error,
                                          bool format) {
	yyjson_mut_obj_add_true(doc, result_obj, "error");
	yyjson_mut_obj_add_strcpy(doc, result_obj, "error_type",
	                          StringUtil::Lower(Exception::ExceptionTypeToString(error.Type())).c_str());
	yyjson_mut_obj_add_strcpy(doc, result_obj, "error_message", error.RawMessage().c_str());
	for (auto &entry : error.ExtraInfo()) {
		yyjson_mut_obj_add_strcpy(doc, result_obj, entry.first.c_str(), entry.second.c_str());
	}
	return WriteDocument(result_obj, format);
}

void JSONSerializeFunctions::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(GetSerializePlanFunction());
	set.AddFunction(GetSerializeSqlFunction());
	set.AddFunction(GetDeserializeSqlFunction());
	set.AddFunction("json_execute_serialized_sql", GetExecuteJsonSerializedSqlPragmaFunction());
	set.AddFunction(GetExecuteJsonSerializedSqlFunction());
}

void BuiltinFunctions::RegisterSerializeFunctions() {
	Register<JSONSerializeFunctions>();
}

} // namespace duckdb
