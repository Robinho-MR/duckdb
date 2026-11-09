//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_schema_inference.hpp
//
// The schema inference of read_json auto-detection, independent of the reader. Usage:
//
//     JSONInferenceOptions options;                       // thresholds, depth, GeoJSON, ...
//     JSONStructureNode node(options.case_insensitive_keys);
//     for (each sample document `val`)
//         JSONStructure::ExtractStructure(val, node, options);
//     JSONStringTypeTester tester(factory, date_formats_or_null, options);   // one per thread
//     node.InitializeCandidateTypes(options);
//     for (each batch of sample documents `vals`)
//         node.RefineCandidateTypes(vals, count, tester);
//     // nodes of parallel tasks are combined with JSONStructure::MergeNodes(merged, task_node)
//     LogicalType type = JSONStructure::StructureToType(factory, node, options);
//
//===----------------------------------------------------------------------===//

#pragma once

#include "json_functions.hpp"

namespace duckdb {
namespace json {

//! Sticky ISO-8601 offset observation across refinement vectors and parallel merges (issue #14919)
enum class TimestampOffsetState : uint8_t { UNKNOWN, WITH_OFFSET, WITHOUT_OFFSET, MIXED };

//! Whether a JSON object is a GeoJSON geometry. Provided by the caller when GeoJSON detection is wanted.
using JSONGeometryDetector = bool (*)(yyjson_val *obj);

//! What the schema inference needs to know of the read_json options
struct JSONInferenceOptions {
	//! Max depth we go to detect nested JSON schema (defaults to unlimited)
	idx_t max_depth = static_cast<idx_t>(-1);
	//! We divide the number of appearances of each JSON field by the auto-detection sample size
	//! If the average over the fields of an object is less than this threshold,
	//! we default to the MAP type with value type of merged field types
	double field_appearance_threshold = 0.1;
	//! If a struct contains more fields than this threshold with at least 80% similar types, we infer a MAP type.
	//! The maximum value (idx_t(-1)) disables MAP inference.
	idx_t map_inference_threshold = 200;
	//! How many documents the reader samples (informational: the reader does the sampling)
	idx_t sample_size = STANDARD_VECTOR_SIZE * 10;
	//! Ignore (rather than throw on) duplicate object keys
	bool ignore_errors = false;
	//! Whether object keys that differ only in ASCII case are the same key
	bool case_insensitive_keys = false;
	//! Whether we auto-detect and convert JSON strings to integers
	bool convert_strings_to_integers = false;
	//! Whether the user passed a date/timestamp format (TIMESTAMPTZ is then never inferred)
	bool user_specified_timestamp_format = false;
	//! Whether the members of the top-level object that differ only in case are renamed (the columns of a record)
	//! instead of dropped (ignore_errors) or an error
	bool rename_duplicate_root_members = false;
	//! Detects GeoJSON geometries, which become GEOMETRY leaves. Null: no GeoJSON detection.
	JSONGeometryDetector geometry_detector = nullptr;
};

//! The user-provided or auto-detected date/timestamp formats, implemented by the reader (strptime formats are not
//! part of the stable API)
class JSONInferenceDateFormats {
public:
	virtual ~JSONInferenceDateFormats() = default;
	//! Whether there are formats for DATE / TIMESTAMP
	virtual bool HasFormats(LogicalTypeId type) const = 0;
	//! Whether any format of the type parses all non-NULL strings of the vector. Called for rows [0, count).
	virtual bool MatchesAnyFormat(LogicalTypeId type, const Vector &strings, idx_t count) = 0;
};

//! Tests whether strings convert to a candidate type. Holds scratch vectors, so use one per thread.
class JSONStringTypeTester {
public:
	JSONStringTypeTester(Factory &factory, JSONInferenceDateFormats *date_formats, const JSONInferenceOptions &options);

	//! Loads the strings of `vals` (null and non-string values become NULL)
	void LoadStrings(yyjson_val *vals[], idx_t count);
	//! Whether every loaded string converts to the type (NULLs always do)
	bool TryCast(LogicalTypeId type);
	//! Updates the offset observation of the loaded strings
	void UpdateTimestampOffsetState(TimestampOffsetState &state);

	JSONInferenceDateFormats *GetDateFormats() const {
		return date_formats;
	}
	bool UserSpecifiedTimestampFormat() const {
		return user_specified_timestamp_format;
	}
	//! The loaded strings
	Vector LoadedStrings();
	//! The scratch chunk for converting to the type (cleared)
	Vector ResultVector(LogicalTypeId type);

private:
	Factory &factory;
	JSONInferenceDateFormats *date_formats;
	bool user_specified_timestamp_format;
	idx_t count = 0;
	std::optional<cxx::DataChunk> string_chunk;
	std::unordered_map<uint32_t, cxx::DataChunk> result_chunks;
};

struct JSONStructureDescription;

struct JSONStructureNode {
public:
	explicit JSONStructureNode(bool case_insensitive_keys);
	JSONStructureNode(const char *key_ptr, const size_t key_len, bool case_insensitive_keys);
	//! An empty node that matches object keys the way parent does
	static JSONStructureNode EmptyLike(const JSONStructureNode &parent);

	//! Disable copy constructors
	JSONStructureNode(const JSONStructureNode &other) = delete;
	JSONStructureNode &operator=(const JSONStructureNode &) = delete;
	//! Enable move constructors
	JSONStructureNode(JSONStructureNode &&other) noexcept;
	JSONStructureNode &operator=(JSONStructureNode &&) noexcept;
	~JSONStructureNode();

	JSONStructureDescription &GetOrCreateDescription(LogicalTypeId type);

	bool ContainsVarchar() const;
	void InitializeCandidateTypes(const JSONInferenceOptions &options, idx_t depth = 0);
	void RefineCandidateTypes(yyjson_val *vals[], idx_t val_count, JSONStringTypeTester &tester);

private:
	void RefineCandidateTypesArray(yyjson_val *vals[], idx_t val_count, JSONStringTypeTester &tester);
	void RefineCandidateTypesObject(yyjson_val *vals[], idx_t val_count, JSONStringTypeTester &tester);
	void RefineCandidateTypesString(yyjson_val *vals[], idx_t val_count, JSONStringTypeTester &tester);
	void EliminateCandidateTypes(idx_t vec_count, JSONStringTypeTester &tester);
	bool EliminateCandidateFormats(idx_t vec_count, LogicalTypeId type, JSONStringTypeTester &tester);

public:
	unique_ptr<string> key;
	bool initialized = false;
	vector<JSONStructureDescription> descriptions;
	idx_t count;
	idx_t null_count;
	//! Object keys that differ only in ASCII case are the same key (read_json case_insensitive_keys). Set when the
	//! node is created: the root from the reader options, every child from its parent
	bool case_insensitive_keys = false;
};

struct JSONStructureDescription {
public:
	JSONStructureDescription(LogicalTypeId type_p, bool case_insensitive_keys);
	//! Disable copy constructors
	JSONStructureDescription(const JSONStructureDescription &other) = delete;
	JSONStructureDescription &operator=(const JSONStructureDescription &) = delete;
	//! Enable move constructors
	JSONStructureDescription(JSONStructureDescription &&other) noexcept;
	JSONStructureDescription &operator=(JSONStructureDescription &&) noexcept;
	~JSONStructureDescription();

	JSONStructureNode &GetOrCreateChild();
	JSONStructureNode &GetOrCreateChild(const char *key_ptr, size_t key_size);
	JSONStructureNode &GetOrCreateChild(yyjson_val *key, yyjson_val *val, const JSONInferenceOptions &options,
	                                    const idx_t depth);

public:
	//! Type of this description
	LogicalTypeId type = LogicalTypeId::INVALID;

	//! Map to children and children
	json_key_optional_case_map_t<idx_t> key_map;
	vector<JSONStructureNode> children;

	//! Candidate types (if auto-detecting and type == LogicalTypeId::VARCHAR)
	vector<LogicalTypeId> candidate_types;

	//! Whether any UBIGINT value exceeds the BIGINT range (i.e., >= 2^63)
	bool has_large_ubigint = false;

	//! MIXED clears the timestamp candidates (VARCHAR): neither TIMESTAMPTZ nor TIMESTAMP is safe
	TimestampOffsetState timestamp_offset_state = TimestampOffsetState::UNKNOWN;
};

struct JSONSchemaInference {
public:
	//! The numeric types a JSON value can have, plus HUGEINT (BIGINT merged with a large UBIGINT)
	static bool IsNumericType(LogicalTypeId type);
	//! The type that holds both of two different numeric types
	static LogicalTypeId MaxNumericType(LogicalTypeId a, LogicalTypeId b);
};

struct JSONStructure {
public:
	static void ExtractStructure(yyjson_val *val, JSONStructureNode &node, const JSONInferenceOptions &options,
	                             const idx_t depth = 0);
	static void MergeNodes(JSONStructureNode &merged, const JSONStructureNode &node);
	//! The type the structure was inferred to be. Falls back to JSON where no single type fits.
	static LogicalType StructureToType(Factory &factory, const JSONStructureNode &node,
	                                   const JSONInferenceOptions &options, idx_t depth = 0);
};

} // namespace json
} // namespace duckdb
