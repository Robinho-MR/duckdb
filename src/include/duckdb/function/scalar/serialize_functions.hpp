//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/scalar/serialize_functions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/built_in_functions.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/pragma_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "yyjson.hpp"

namespace duckdb {

//! Functions that expose the engine's serialization format (statements and plans) as JSON
struct JSONSerializeFunctions {
	static void RegisterFunction(BuiltinFunctions &set);

	static ScalarFunctionSet GetSerializeSqlFunction();
	static ScalarFunctionSet GetDeserializeSqlFunction();
	static ScalarFunctionSet GetSerializePlanFunction();
	static PragmaFunctionSet GetExecuteJsonSerializedSqlPragmaFunction();
	static TableFunctionSet GetExecuteJsonSerializedSqlFunction();

	static constexpr auto READ_FLAG = duckdb_yyjson::YYJSON_READ_ALLOW_INF_AND_NAN |
	                                  duckdb_yyjson::YYJSON_READ_ALLOW_TRAILING_COMMAS |
	                                  duckdb_yyjson::YYJSON_READ_BIGNUM_AS_RAW;
	static constexpr auto WRITE_FLAG = duckdb_yyjson::YYJSON_WRITE_ALLOW_INF_AND_NAN;
	static constexpr auto WRITE_PRETTY_FLAG =
	    duckdb_yyjson::YYJSON_WRITE_ALLOW_INF_AND_NAN | duckdb_yyjson::YYJSON_WRITE_PRETTY;

	//! Writes the document root to a string. Throws if the document cannot be written (e.g. invalid UTF-8)
	static string WriteDocument(duckdb_yyjson::yyjson_mut_val *root, bool format);
	//! Writes the document of an error result (error, error_type, error_message and extra info)
	static string WriteError(duckdb_yyjson::yyjson_mut_doc *doc, duckdb_yyjson::yyjson_mut_val *result_obj,
	                         const ErrorData &error, bool format);
};

struct yyjson_doc_deleter {
	void operator()(duckdb_yyjson::yyjson_doc *doc) {
		if (doc) {
			duckdb_yyjson::yyjson_doc_free(doc);
		}
	}
};

struct yyjson_mut_doc_deleter {
	void operator()(duckdb_yyjson::yyjson_mut_doc *doc) {
		if (doc) {
			duckdb_yyjson::yyjson_mut_doc_free(doc);
		}
	}
};

using yyjson_doc_ptr = unique_ptr<duckdb_yyjson::yyjson_doc, yyjson_doc_deleter>;
using yyjson_mut_doc_ptr = unique_ptr<duckdb_yyjson::yyjson_mut_doc, yyjson_mut_doc_deleter>;

} // namespace duckdb
