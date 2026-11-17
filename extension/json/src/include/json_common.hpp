//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_common.hpp
//
// What every part of the json extension shares: the vocabulary types, the errors, and the yyjson helpers (JSONCommon).
// The extension depends on nothing but the stable C++ API (duckdb_cpp.hpp), yyjson and the standard library.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb_cpp.hpp"
#include "yyjson.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace duckdb_yyjson; // NOLINT

#ifndef D_ASSERT
#define D_ASSERT(condition) assert(condition)
#endif

namespace duckdb {
namespace json {

//===--------------------------------------------------------------------===//
// Vocabulary
//===--------------------------------------------------------------------===//
using cxx::idx_t;
using std::string;
using std::string_view;
using std::unique_ptr;
using std::unordered_map;
using std::unordered_set;
using std::vector;

using cxx::blob_t;
using cxx::bignum_t;
using cxx::Context;
using cxx::Extension;
using cxx::Factory;
using cxx::list_entry_t;
using cxx::LogicalType;
using cxx::LogicalTypeId;
using cxx::Value;
using cxx::varchar_t;
using cxx::Vector;

constexpr idx_t STANDARD_VECTOR_SIZE = 2048;

template <class T, class... ARGS>
unique_ptr<T> make_uniq(ARGS &&...args) { // NOLINT: match the engine's naming
	return unique_ptr<T>(new T(std::forward<ARGS>(args)...));
}

//===--------------------------------------------------------------------===//
// Errors
//===--------------------------------------------------------------------===//
//! printf-style formatting that also accepts std::string arguments
inline const char *FormatArgument(const std::string &value) {
	return value.c_str();
}
template <class T>
inline T FormatArgument(const T &value) {
	return value;
}

template <class... ARGS>
string Format(const char *format, const ARGS &...args) {
	if constexpr (sizeof...(ARGS) == 0) {
		return format;
	} else {
		const auto size = std::snprintf(nullptr, 0, format, FormatArgument(args)...);
		if (size <= 0) {
			return format;
		}
		string result(static_cast<size_t>(size) + 1, '\0');
		std::snprintf(&result[0], result.size(), format, FormatArgument(args)...);
		result.resize(static_cast<size_t>(size));
		return result;
	}
}

//! The error codes of the C API (duckdb_v2.h) that select which kind of error the engine reports
enum class ErrorCode : int {
	IO_GENERAL = 1004,
	INVALID_INPUT = 2001,
	OUT_OF_RANGE = 2003,
	CONVERSION = 4001,
	BINDER = 5003,
	NOT_IMPLEMENTED = 5009,
	INTERNAL = 8001,
};

//! An error of the given kind: "Binder Error: ...", "Conversion Error: ...", ...
class Error : public cxx::Exception {
public:
	Error(ErrorCode code, const string &message) : cxx::Exception(static_cast<int>(code), message, message) {
	}
};

#define JSON_DEFINE_EXCEPTION(NAME, CODE)                                                                              \
	class NAME : public Error {                                                                                        \
	public:                                                                                                            \
		explicit NAME(const string &message) : Error(ErrorCode::CODE, message) {                                       \
		}                                                                                                              \
		template <class... ARGS>                                                                                       \
		explicit NAME(const char *format, const ARGS &...args)                                                         \
		    : Error(ErrorCode::CODE, ::duckdb::json::Format(format, args...)) {                                        \
		}                                                                                                              \
	}

JSON_DEFINE_EXCEPTION(IOException, IO_GENERAL);
JSON_DEFINE_EXCEPTION(InvalidInputException, INVALID_INPUT);
JSON_DEFINE_EXCEPTION(BinderException, BINDER);
JSON_DEFINE_EXCEPTION(ConversionException, CONVERSION);
JSON_DEFINE_EXCEPTION(NotImplementedException, NOT_IMPLEMENTED);
JSON_DEFINE_EXCEPTION(InternalException, INTERNAL);

//===--------------------------------------------------------------------===//
// Strings
//===--------------------------------------------------------------------===//
struct StringUtil {
	//! Whether two strings are equal, ignoring the case of ASCII letters
	static bool CIEquals(const char *a, size_t a_len, const char *b, size_t b_len) {
		if (a_len != b_len) {
			return false;
		}
		for (size_t i = 0; i < a_len; i++) {
			auto ca = static_cast<unsigned char>(a[i]);
			auto cb = static_cast<unsigned char>(b[i]);
			if (ca >= 'A' && ca <= 'Z') {
				ca = static_cast<unsigned char>(ca + ('a' - 'A'));
			}
			if (cb >= 'A' && cb <= 'Z') {
				cb = static_cast<unsigned char>(cb + ('a' - 'A'));
			}
			if (ca != cb) {
				return false;
			}
		}
		return true;
	}
	//! The string with its ASCII letters in lower case
	static string Lower(string source) {
		for (auto &c : source) {
			if (c >= 'A' && c <= 'Z') {
				c = static_cast<char>(c + ('a' - 'A'));
			}
		}
		return source;
	}
	static string Replace(string source, const string &from, const string &to) {
		if (from.empty()) {
			return source;
		}
		size_t pos = 0;
		while ((pos = source.find(from, pos)) != string::npos) {
			source.replace(pos, from.size(), to);
			pos += to.size();
		}
		return source;
	}
};

//===--------------------------------------------------------------------===//
// Keys
//===--------------------------------------------------------------------===//
//! JSONKey / json_key_map_t speeds up mapping from JSON key to column ID
struct JSONKey {
	const char *ptr;
	size_t len;
};

struct JSONKeyHash {
	inline std::size_t operator()(const JSONKey &k) const {
		size_t result;
		if (k.len >= sizeof(size_t)) {
			memcpy(&result, k.ptr + k.len - sizeof(size_t), sizeof(size_t));
		} else {
			result = 0;
			memcpy(&result, k.ptr, k.len);
		}
		return result;
	}
};

struct JSONKeyEquality {
	inline bool operator()(const JSONKey &a, const JSONKey &b) const {
		if (a.len != b.len) {
			return false;
		}
		return memcmp(a.ptr, b.ptr, a.len) == 0;
	}
};

template <typename T>
using json_key_map_t = unordered_map<JSONKey, T, JSONKeyHash, JSONKeyEquality>;
using json_key_set_t = unordered_set<JSONKey, JSONKeyHash, JSONKeyEquality>;

//! Case-insensitive counterparts of JSONKeyHash / JSONKeyEquality for json_key_ci_map_t: keys that differ only in
//! the case of ASCII letters (A-Z / a-z) are the same key
struct JSONKeyCaseInsensitiveHash {
	inline std::size_t operator()(const JSONKey &k) const {
		// JSONKeyHash is the key's (last 8) raw bytes: setting bit 5 of every byte maps 'A'-'Z' to 'a'-'z'. Other
		// bytes may collide (e.g. '@' and '`'), which equality sorts out
		return JSONKeyHash()(k) | static_cast<size_t>(0x2020202020202020ULL);
	}
};

struct JSONKeyCaseInsensitiveEquality {
	inline bool operator()(const JSONKey &a, const JSONKey &b) const {
		return StringUtil::CIEquals(a.ptr, a.len, b.ptr, b.len);
	}
};

template <typename T>
using json_key_ci_map_t = unordered_map<JSONKey, T, JSONKeyCaseInsensitiveHash, JSONKeyCaseInsensitiveEquality>;

//! JSONKeyHash / JSONKeyEquality, or their case-insensitive counterparts if case_insensitive is set: for maps that
//! are only used while detecting the schema, where a runtime choice is cheap enough
struct JSONKeyOptionalCaseHash {
	bool case_insensitive = false;

	inline std::size_t operator()(const JSONKey &k) const {
		return case_insensitive ? JSONKeyCaseInsensitiveHash()(k) : JSONKeyHash()(k);
	}
};

struct JSONKeyOptionalCaseEquality {
	bool case_insensitive = false;

	inline bool operator()(const JSONKey &a, const JSONKey &b) const {
		return case_insensitive ? JSONKeyCaseInsensitiveEquality()(a, b) : JSONKeyEquality()(a, b);
	}
};

template <typename T>
using json_key_optional_case_map_t = unordered_map<JSONKey, T, JSONKeyOptionalCaseHash, JSONKeyOptionalCaseEquality>;

//===--------------------------------------------------------------------===//
// Paths
//===--------------------------------------------------------------------===//
//! The type of a single element of a JSON path
enum class JSONPathElementType : uint8_t {
	//! Object key ($.key)
	KEY,
	//! Array index ($[0])
	INDEX,
	//! Array index counting from the back ($[#-1] or $[-1])
	REVERSE_INDEX,
	//! The position after the last array element ($[#])
	APPEND,
	//! Wildcard ($.* or $[*])
	WILDCARD,
	//! Recursive wildcard ($.**)
	RECURSIVE_WILDCARD,
};

//! A single element of a JSON path
struct JSONPathElement {
	JSONPathElementType type = JSONPathElementType::KEY;
	//! Key for KEY elements
	string key;
	//! Index for INDEX and REVERSE_INDEX elements
	idx_t index = 0;

	bool operator==(const JSONPathElement &other) const {
		return type == other.type && key == other.key && index == other.index;
	}
};

//! Iterates over the elements of a JSON path ($.field[index]... syntax)
class JSONPathIterator {
public:
	//! The path must start with '$' (validate before constructing)
	JSONPathIterator(const char *ptr, idx_t len, bool binder);
	//! Get the next path element, returns false when the path is exhausted. Throws on a malformed path.
	bool Next(JSONPathElement &element);

private:
	const char *ptr;
	const char *const end;
	const bool binder;
};

//===--------------------------------------------------------------------===//
// JSONCommon
//===--------------------------------------------------------------------===//
//! Common JSON functionality for most JSON functions
struct JSONCommon {
public:
	//! Deep copy of a mutable value - equivalent to yyjson_val_mut_copy, but iterative instead of recursive, so that
	//! deeply nested values do not overflow the stack
	static yyjson_mut_val *MutValMutCopy(yyjson_mut_doc *doc, yyjson_mut_val *val);

	//! Maximum recursion depth for recursive functions
	static constexpr idx_t MAX_RECURSION_DEPTH = 128;
	//! Read/Write flags
	static constexpr auto READ_FLAG =
	    YYJSON_READ_ALLOW_INF_AND_NAN | YYJSON_READ_ALLOW_TRAILING_COMMAS | YYJSON_READ_BIGNUM_AS_RAW;
	static constexpr auto READ_STOP_FLAG = READ_FLAG | YYJSON_READ_STOP_WHEN_DONE;
	static constexpr auto READ_INSITU_FLAG = READ_STOP_FLAG | YYJSON_READ_INSITU;
	static constexpr auto WRITE_FLAG = YYJSON_WRITE_ALLOW_INF_AND_NAN;
	static constexpr auto WRITE_PRETTY_FLAG = YYJSON_WRITE_ALLOW_INF_AND_NAN | YYJSON_WRITE_PRETTY;

	//! Whether documents read into these types keep numbers as text (YYJSON_READ_NUMBER_AS_RAW): a DECIMAL needs it
	static bool NumbersAsRaw(const LogicalType &type);
	static bool NumbersAsRaw(const vector<LogicalType> &types);
	//! The flags to read documents with, given NumbersAsRaw
	static yyjson_read_flag GetReadFlag(yyjson_read_flag flag, bool numbers_as_raw);
	//! Parses a number kept as text as the default flags would have; false if those keep it as text too (big numbers)
	static inline bool ReadRawNumber(const char *str, yyjson_val &number) {
		return yyjson_read_number(str, &number, READ_FLAG, nullptr, nullptr) && !unsafe_yyjson_is_raw(&number);
	}
	static inline bool ReadRawNumber(yyjson_val *val, yyjson_val &number) {
		D_ASSERT(unsafe_yyjson_is_raw(val));
		return ReadRawNumber(unsafe_yyjson_get_str(val), number);
	}

public:
	//! Constant JSON type strings
	static constexpr char const *TYPE_STRING_NULL = "NULL";
	static constexpr char const *TYPE_STRING_BOOLEAN = "BOOLEAN";
	static constexpr char const *TYPE_STRING_BIGINT = "BIGINT";
	static constexpr char const *TYPE_STRING_UBIGINT = "UBIGINT";
	static constexpr char const *TYPE_STRING_DOUBLE = "DOUBLE";
	static constexpr char const *TYPE_STRING_HUGEINT = "HUGEINT";
	static constexpr char const *TYPE_STRING_VARCHAR = "VARCHAR";
	static constexpr char const *TYPE_STRING_ARRAY = "ARRAY";
	static constexpr char const *TYPE_STRING_OBJECT = "OBJECT";

	static inline const char *ValTypeToString(yyjson_val *val) {
		switch (yyjson_get_tag(val)) {
		case YYJSON_TYPE_NULL | YYJSON_SUBTYPE_NONE:
			return TYPE_STRING_NULL;
		case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NOESC:
		case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NONE:
			return TYPE_STRING_VARCHAR;
		case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE:
			return TYPE_STRING_ARRAY;
		case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE:
			return TYPE_STRING_OBJECT;
		case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_TRUE:
		case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_FALSE:
			return TYPE_STRING_BOOLEAN;
		case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT:
			return TYPE_STRING_UBIGINT;
		case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_SINT:
			return TYPE_STRING_BIGINT;
		case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL:
		case YYJSON_TYPE_RAW | YYJSON_SUBTYPE_NONE:
			return TYPE_STRING_DOUBLE;
		default:
			throw InternalException("Unexpected yyjson tag in ValTypeToString");
		}
	}

	static inline string_view ValTypeToStringView(yyjson_val *val) {
		return string_view(ValTypeToString(val));
	}

	static inline LogicalTypeId ValTypeToLogicalTypeId(yyjson_val *val) {
		switch (yyjson_get_tag(val)) {
		case YYJSON_TYPE_NULL | YYJSON_SUBTYPE_NONE:
			return LogicalTypeId::SQLNULL;
		case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NOESC:
		case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NONE:
			return LogicalTypeId::VARCHAR;
		case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE:
			return LogicalTypeId::LIST;
		case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE:
			return LogicalTypeId::STRUCT;
		case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_TRUE:
		case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_FALSE:
			return LogicalTypeId::BOOLEAN;
		case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT:
			return LogicalTypeId::UBIGINT;
		case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_SINT:
			return LogicalTypeId::BIGINT;
		case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL:
		case YYJSON_TYPE_RAW | YYJSON_SUBTYPE_NONE:
			return LogicalTypeId::DOUBLE;
		default:
			throw InternalException("Unexpected yyjson tag in ValTypeToLogicalTypeId");
		}
	}

public:
	//===--------------------------------------------------------------------===//
	// Document creation / reading / writing
	//===--------------------------------------------------------------------===//
	template <class T>
	static T *AllocateArray(yyjson_alc *alc, idx_t count) {
		return reinterpret_cast<T *>(alc->malloc(alc->ctx, sizeof(T) * count));
	}

	template <class T>
	static T *AllocateArray(yyjson_mut_doc *doc, idx_t count) {
		return AllocateArray<T>(&doc->alc, count);
	}

	static inline yyjson_mut_doc *CreateDocument(yyjson_alc *alc) {
		D_ASSERT(alc);
		return yyjson_mut_doc_new(alc);
	}
	//! Parses without throwing; returns nullptr (and fills err) on malformed input. Does not modify the input unless
	//! the flags say YYJSON_READ_INSITU
	static inline yyjson_doc *ReadDocumentUnsafe(char *data, idx_t size, const yyjson_read_flag flg, yyjson_alc *alc,
	                                             yyjson_read_err *err = nullptr) {
		D_ASSERT(alc);
		return yyjson_read_opts(data, size, flg, alc, err);
	}
	static inline yyjson_doc *ReadDocumentUnsafe(varchar_t input, const yyjson_read_flag flg, yyjson_alc *alc,
	                                             yyjson_read_err *err = nullptr) {
		return ReadDocumentUnsafe(input.data_mut(), input.size(), flg, alc, err);
	}
	static inline yyjson_doc *ReadDocument(char *data, idx_t size, const yyjson_read_flag flg, yyjson_alc *alc) {
		yyjson_read_err error;
		auto result = ReadDocumentUnsafe(data, size, flg, alc, &error);
		if (error.code != YYJSON_READ_SUCCESS) {
			ThrowParseError(data, size, error);
		}
		return result;
	}
	static inline yyjson_doc *ReadDocument(varchar_t input, const yyjson_read_flag flg, yyjson_alc *alc) {
		return ReadDocument(input.data_mut(), input.size(), flg, alc);
	}

	static string FormatParseError(const char *data, idx_t length, yyjson_read_err &error, const string &extra = "") {
		D_ASSERT(error.code != YYJSON_READ_SUCCESS);
		// Truncate, so we don't print megabytes worth of JSON
		auto input = length > 50 ? string(data, 47) + "..." : string(data, length);
		// Have to replace \r, otherwise output is unreadable
		input = StringUtil::Replace(input, "\r", "\\r");
		return Format("Malformed JSON at byte %lld of input: %s. %s Input: \"%s\"", static_cast<long long>(error.pos),
		              error.msg, extra, input);
	}
	static void ThrowParseError(const char *data, idx_t length, yyjson_read_err &error, const string &extra = "") {
		throw InvalidInputException(FormatParseError(data, length, error, extra));
	}

	//! Writes a value into memory of the allocator
	template <class YYJSON_VAL_T>
	static inline char *WriteVal(YYJSON_VAL_T *val, yyjson_alc *alc, idx_t &len) {
		throw InternalException("Unknown yyjson val type");
	}
	template <class YYJSON_VAL_T>
	static inline string_view WriteVal(YYJSON_VAL_T *val, yyjson_alc *alc) {
		D_ASSERT(alc);
		idx_t len;
		auto data = WriteVal<YYJSON_VAL_T>(val, alc, len);
		return string_view(data, len);
	}

	//! Slow and easy ToString for errors
	static string ValToString(yyjson_val *val, idx_t max_len = static_cast<idx_t>(-1));
	//! Throw an error with the printed yyjson_val: the message takes a single %s
	static void ThrowValFormatError(const string &error_string, yyjson_val *val);

public:
	//===--------------------------------------------------------------------===//
	// JSON pointer / path
	//===--------------------------------------------------------------------===//
	enum class JSONPathType : uint8_t {
		//! Extract a single value
		REGULAR = 0,
		//! Extract multiple values (when we have a '*' wildcard in the JSON Path)
		WILDCARD = 1,
	};

	//! Get JSON value using JSON path query (safe, checks the path query)
	static yyjson_val *Get(yyjson_val *val, string_view path_str, bool integral_argument);

	//! Get JSON value using JSON path query (unsafe)
	static inline yyjson_val *GetUnsafe(yyjson_val *val, const char *ptr, const idx_t &len) {
		if (len == 0) {
			return val;
		}
		switch (*ptr) {
		case '/':
			return GetPointer(val, ptr, len);
		case '$':
			return GetPath(val, ptr, len);
		default:
			throw InternalException("JSON pointer/path does not start with '/' or '$'");
		}
	}

	//! Get JSON values using JSON path query with wildcards (unsafe)
	static void GetWildcardPath(yyjson_val *val, const char *ptr, const idx_t &len, vector<yyjson_val *> &vals);

	//! Validate JSON Path ($.field[index]... syntax), returns true if there are wildcards in the path
	static JSONPathType ValidatePath(const char *ptr, const idx_t &len, const bool binder);

	//! Parse a JSON path ($.field[index]... syntax, no wildcards) into path elements
	static vector<JSONPathElement> ParsePathElements(const char *ptr, idx_t len, bool binder);
	//! Get JSON value by walking pre-parsed path elements
	static yyjson_val *GetPathElements(yyjson_val *val, const vector<JSONPathElement> &elements);

public:
	//! Same as BigQuery json_value
	static inline std::optional<string_view> JSONValue(yyjson_val *val, yyjson_alc *alc) {
		switch (yyjson_get_tag(val)) {
		case YYJSON_TYPE_NULL | YYJSON_SUBTYPE_NONE:
		case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE:
		case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE:
			return std::nullopt;
		default:
			return JSONCommon::WriteVal<yyjson_val>(val, alc);
		}
	}

private:
	//! Get JSON pointer (/field/index/... syntax)
	static inline yyjson_val *GetPointer(yyjson_val *val, const char *ptr, const idx_t &len) {
		yyjson_ptr_err err;
		return unsafe_yyjson_ptr_getx(val, ptr, len, &err);
	}
	//! Get JSON path ($.field[index]... syntax)
	static yyjson_val *GetPath(yyjson_val *val, const char *ptr, const idx_t &len);
};

template <>
inline char *JSONCommon::WriteVal(yyjson_val *val, yyjson_alc *alc, idx_t &len) {
	size_t len_size_t;
	// yyjson_val_write_opts must not throw
	auto ret = yyjson_val_write_opts(val, JSONCommon::WRITE_FLAG, alc, &len_size_t, nullptr);
	len = len_size_t;
	return ret;
}
template <>
inline char *JSONCommon::WriteVal(yyjson_mut_val *val, yyjson_alc *alc, idx_t &len) {
	size_t len_size_t;
	// yyjson_mut_val_write_opts must not throw
	auto ret = yyjson_mut_val_write_opts(val, JSONCommon::WRITE_FLAG, alc, &len_size_t, nullptr);
	len = len_size_t;
	return ret;
}

struct yyjson_doc_deleter {
	void operator()(yyjson_doc *doc) {
		if (doc) {
			yyjson_doc_free(doc);
		}
	}
};

struct yyjson_mut_doc_deleter {
	void operator()(yyjson_mut_doc *doc) {
		if (doc) {
			yyjson_mut_doc_free(doc);
		}
	}
};

using yyjson_doc_ptr = unique_ptr<yyjson_doc, yyjson_doc_deleter>;
using yyjson_mut_doc_ptr = unique_ptr<yyjson_mut_doc, yyjson_mut_doc_deleter>;

//! Writes values whose numbers were kept as text (NumbersAsRaw) as if read with the default flags (ReadRawNumber)
class JSONRawNumberWriter {
public:
	explicit JSONRawNumberWriter(yyjson_alc *alc) : alc(alc), doc(nullptr) {
	}
	string_view Write(yyjson_val *val);

private:
	yyjson_alc *alc;
	//! Created by the first value that has a number to parse; holds the mutable copies
	yyjson_mut_doc *doc;
	vector<yyjson_mut_val *> containers;
};

} // namespace json
} // namespace duckdb
