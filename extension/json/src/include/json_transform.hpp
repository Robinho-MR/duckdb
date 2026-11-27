//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_transform.hpp
//
// Turning parsed JSON values (yyjson) into the rows of a vector of any type. This is what json_transform, from_json
// and the casts from JSON use, and what the readers use to fill the columns of a chunk (JSONTransform::Transform).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "json_functions.hpp"

namespace duckdb {
namespace json {

//! How object keys are matched to the names of a STRUCT
enum class JSONKeyMatch : uint8_t { EXACT, CASE_INSENSITIVE };

//! The columns of a (nested) column that are read: the engine's ColumnIndex. An empty `children` reads everything.
struct JSONColumnProjection {
	//! Index of the (struct) child
	idx_t index = 0;
	//! The projected children of a STRUCT, in the order they are read
	vector<JSONColumnProjection> children;
};

//! The date / timestamp formats detected for a file. The stable API has no strptime, so whoever owns the formats
//! (the reader) implements this.
class JSONDateFormats {
public:
	virtual ~JSONDateFormats() = default;
	//! Whether there are formats for DATE / TIMESTAMP
	virtual bool HasFormats(LogicalTypeId type) const = 0;
	//! The number of formats of DATE / TIMESTAMP. They are tried from the last one to the first one
	virtual idx_t FormatCount(LogicalTypeId type) const = 0;
	//! Parses with format `format_idx`; the result is the number of days (DATE) or microseconds (TIMESTAMP)
	virtual bool TryParse(LogicalTypeId type, idx_t format_idx, string_view input, int64_t &result,
	                      string &error) const = 0;
};

//! Parses a GeoJSON geometry into WKB for the cast from JSON objects to GEOMETRY. Returns false and sets `error` if the
//! value is not a well-formed GeoJSON geometry. Set by whoever implements GeoJSON (see JSONTransform::SetGeoJSONParser)
using JSONGeoJSONParser = bool (*)(yyjson_val *val, string &wkb, string &error);

//! Options for error handling while transforming JSON
struct JSONTransformOptions {
public:
	explicit JSONTransformOptions(Factory &factory);
	JSONTransformOptions(Factory &factory, bool strict_cast, bool error_duplicate_key, bool error_missing_key,
	                     bool error_unknown_key);

public:
	//! Used for the casts from strings and numbers to the target types
	Factory &factory;
	//! Throws an error if the cast doesn't work (instead of NULL-ing it)
	bool strict_cast = false;
	//! Throws an error if there is a duplicate key (instead of ignoring it)
	bool error_duplicate_key = false;
	//! Throws an error if a key is missing (instead of NULL-ing it)
	bool error_missing_key = false;
	//! Throws an error if an object has a key we didn't know about
	bool error_unknown_key = false;
	//! How object keys are matched to names
	JSONKeyMatch key_match = JSONKeyMatch::EXACT;

	//! Whether to delay the error when transforming (e.g., when non-strict casting or reading from file)
	bool delay_error = false;
	//! Whether the documents keep numbers as text (JSONCommon::NumbersAsRaw): nested values become strings through
	//! JSONRawNumberWriter, so that they read the same as without the DECIMAL column
	bool numbers_as_raw = false;
	//! Date format used for parsing (can be NULL)
	const JSONDateFormats *date_formats = nullptr;
	//! String to store errors in
	string error_message;
	//! Index of the object where the error occurred
	idx_t object_index = static_cast<idx_t>(-1);
};

struct JSONTransform {
	//! Transforms `count` values into the first `count` rows of `result` (of any type). A nullptr value is NULL. In
	//! case of a (strict) failure this returns false and sets options.error_message / options.object_index; with
	//! options.delay_error unset the object transforms throw instead.
	static bool Transform(yyjson_val *vals[], yyjson_alc *alc, Vector &result, idx_t count,
	                      JSONTransformOptions &options, const JSONColumnProjection *column_index);
	//! Transforms the objects into the given columns, matching the keys to `names`
	static bool TransformObject(yyjson_val *objects[], yyjson_alc *alc, idx_t count, const vector<string> &names,
	                            vector<Vector> &result_vectors, JSONTransformOptions &options,
	                            const vector<JSONColumnProjection> *column_indices, bool error_unknown_key);
	//! Fills a VARCHAR vector with the strings of the values (NULL for the values that are not strings; in strict mode
	//! that is an error)
	static bool GetStringVector(yyjson_val *vals[], idx_t count, const LogicalType &target, Vector &string_vector,
	                            JSONTransformOptions &options);
	//! Installs the GeoJSON parser for the cast from JSON objects to GEOMETRY (without one, those are a cast error)
	static void SetGeoJSONParser(JSONGeoJSONParser parser);
};

} // namespace json
} // namespace duckdb
