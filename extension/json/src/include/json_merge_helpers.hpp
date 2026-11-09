//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_merge_helpers.hpp
//
// What the functions that fold JSON arguments into one document share (json_merge_patch, json_deep_merge).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "json_functions.hpp"

namespace duckdb {
namespace json {

//! Parses an argument into mutable values of `doc`; NULL rows become nullptr
inline void ReadMutableObjects(yyjson_mut_doc *doc, const Vector &input, idx_t count, yyjson_mut_val *objs[]) {
	ValueView<varchar_t> entries(input);
	for (idx_t i = 0; i < count; i++) {
		if (!entries.IsValid(i)) {
			objs[i] = nullptr;
		} else {
			objs[i] = yyjson_val_mut_copy(
			    doc, JSONCommon::ReadDocument(entries.Get(i), JSONCommon::READ_FLAG, &doc->alc)->root);
		}
	}
}

//! Writes the documents to the result, nullptr is NULL
inline void WriteMutableObjects(Vector &result, idx_t count, yyjson_mut_val *objs[], yyjson_alc *alc) {
	StringWriter writer(result, count);
	for (idx_t i = 0; i < count; i++) {
		if (objs[i] == nullptr) {
			writer.WriteNull();
		} else {
			writer.WriteValue(JSONCommon::WriteVal<yyjson_mut_val>(objs[i], alc));
		}
	}
}

//! Registers f(json1 JSON, json2 JSON, *args JSON) -> JSON, which handles NULLs itself
inline void RegisterJSONFold(Extension &extension, Context &context, const string &name,
                             ScalarFunction::ExecCallback exec, FunctionFallibility fallibility) {
	auto &factory = context.GetFactory();
	auto json = JSONType(factory);
	auto function = ScalarFunction::Create(factory);
	function.SetName(name);
	function.WithSignature([&](cxx::FunctionSignature &sig) {
		sig.AddParameter("json1", json);
		sig.AddParameter("json2", json);
		sig.AddArgs("args", json);
		sig.SetReturnType(json);
	});
	function.SetExecCallback(exec);
	function.SetInitCallback(JSONFunctionLocalState::Init);
	function.SetNullHandling(cxx::FunctionNullHandling::SPECIAL);
	function.SetFallibility(fallibility);
	extension.Register(function);
}

} // namespace json
} // namespace duckdb
