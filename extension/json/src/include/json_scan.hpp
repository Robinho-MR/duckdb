//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_scan.hpp
//
// The bind data of the JSON file scans (read_json, read_ndjson, read_json_objects, ... and COPY ... FROM), the schema
// auto-detection that decides the columns of a file, and the scan of a file into the columns of a chunk. The scan
// functions are single-file table functions that the stable multi-file function wraps (globbing, lists of files, hive
// partitioning, filename, union_by_name).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "json_date_formats.hpp"
#include "json_reader.hpp"
#include "json_schema_inference.hpp"
#include "json_transform.hpp"

namespace duckdb {
namespace json {

using cxx::CopyFunction;
using cxx::TableFunction;

//! What distinguishes the single-file functions (the user data of the table function)
struct JSONScanInfo {
	JSONScanInfo(string name_p, JSONScanType type_p, JSONFormat format_p, JSONRecordType record_type_p,
	             bool auto_detect_p)
	    : name(std::move(name_p)), type(type_p), format(format_p), record_type(record_type_p),
	      auto_detect(auto_detect_p) {
	}

	//! The name of the single-file function
	string name;
	JSONScanType type;
	JSONFormat format;
	JSONRecordType record_type;
	bool auto_detect;
};

//! The bind data of a scan of one JSON file
struct JSONScanData {
public:
	JSONScanData() = default;
	JSONScanData(JSONScanData &&) = default;

	void InitializeFormats();
	void InitializeFormats(bool auto_detect);

public:
	//! JSON reader options
	JSONReaderOptions options;
	//! The file that is read: the path, and what it was given as (a path or a file struct with open options)
	string file_path;
	std::optional<Value> file;
	//! The format of the file, and whether the rows are the "features" array of a GeoJSON FeatureCollection
	JSONFormat format = JSONFormat::AUTO_DETECT;
	bool skip_feature_collection_prefix = false;

	//! The set of keys to extract (case sensitive)
	vector<string> key_names;
	//! For JSONRecordType::FEATURES: where each column reads its value from, in bind order
	vector<JSONFeatureColumn> feature_columns;

	//! The date format map
	std::shared_ptr<JSONDateFormatMap> date_formats;

	//! Options when transforming the JSON to columnar data
	bool strict_cast = false;
	bool error_duplicate_key = false;
	bool error_unknown_key = false;
	bool numbers_as_raw = false;
	JSONKeyMatch key_match = JSONKeyMatch::EXACT;

	//! The estimated number of rows of the file
	std::optional<idx_t> estimated_cardinality;

	//! The structure detected for the file, to merge with the structures of the other files of a scan
	std::shared_ptr<JSONStructureNode> structure;
	//! Whether the record type was auto-detected from that structure
	bool record_type_auto_detected = false;
};

struct JSONScan {
public:
	//! Parse a single read_json option - returns false if the option is not a JSON reader option
	static bool ParseOption(Context &context, const string &key, const Value &value, JSONReaderOptions &options);
	//! The named arguments of a bind
	static void ParseNamedArguments(TableFunction::BindInput &input, JSONReaderOptions &options);

	//! Determines the names/types that are read from the file - performing auto-detection if required.
	static void BindSchema(Context &context, JSONScanData &data, vector<LogicalType> &return_types,
	                       vector<string> &names);
	//! Set up the transform options of the scan
	static void FinalizeBind(JSONScanData &data);
	//! JSON may contain columns such as "id" and "Id", which are duplicates for us due to case-insensitivity -
	//! rename them so we can parse the file anyway
	static void DeduplicateColumnNames(vector<string> &names);

	//! Derive the columns that are read from a detected JSON structure
	static void StructureToColumns(Context &context, JSONScanData &data, const JSONStructureNode &node,
	                               vector<LogicalType> &return_types, vector<string> &names, vector<string> &keys);

	//! Installs the GeoJSON hooks of the reader (see json_geometry_*): without them GeoJSON is not detected
	static void SetGeoJSONGeometryDetector(JSONGeometryDetector detector);
};

//! The columns the scan reads and the state shared by the threads that scan a file
struct JSONScanGlobalState {
public:
	JSONScanGlobalState(const JSONScanData &data, unique_ptr<JSONReader> reader, vector<string> names,
	                    vector<JSONFeatureColumn> feature_columns);

public:
	const JSONScanData &data;
	unique_ptr<JSONReader> reader;
	//! Column names that we're actually reading (after projection pushdown)
	vector<string> names;
	vector<JSONFeatureColumn> feature_columns;
	bool error_unknown_key;
};

struct JSONScanLocalState {
	JSONReaderScanState scan_state;
};

//! Fills the output vectors with the documents of the last Scan. Returns the number of rows (0 if an error was
//! recorded that could not be thrown yet)
idx_t JSONScanToVectors(Context &context, JSONScanGlobalState &gstate, JSONScanLocalState &lstate,
                        vector<Vector> &output);

//! Registers read_json and friends, and the single-file functions they wrap (functions/read_json.cpp)
void RegisterReadJSON(Extension &extension, Context &context);

//! Gives a copy function (json, ndjson, jsonl, geojson, geojsonl) its COPY ... FROM callbacks
//! (functions/copy_json_from.cpp). GeoJSON is recognised from the file name, `geojson` is currently unused.
void SetJSONCopyFromCallbacks(CopyFunction &function, bool geojson);

} // namespace json
} // namespace duckdb
