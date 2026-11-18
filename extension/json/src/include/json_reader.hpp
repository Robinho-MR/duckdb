//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_reader.hpp
//
// Reading one JSON file in buffers and parsing the documents in them with yyjson: the options of read_json, and
// JSONReader, which hands out the parts of a file ("batches") to scanning threads. NDJSON files that can be read at
// an offset are split in chunks of maximum_object_size bytes that threads parse independently (every chunk owns the
// lines that start in it); everything else is one batch that reads the file front to back.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "json_allocator.hpp"
#include "json_functions.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

namespace duckdb {
namespace json {

using cxx::FileHandle;
using cxx::MemoryReservation;

enum class JSONScanType : uint8_t {
	INVALID = 0,
	//! Read JSON straight to columnar data
	READ_JSON = 1,
	//! Read JSON values as strings
	READ_JSON_OBJECTS = 2,
	//! Sample run for schema detection
	SAMPLE = 3
};

enum class JSONRecordType : uint8_t {
	AUTO_DETECT = 0,
	//! Sequential objects that are unpacked
	RECORDS = 1,
	//! Any other JSON type, e.g., ARRAY
	VALUES = 2,
	//! GeoJSON Features, unnested into a geometry column plus one column per property
	FEATURES = 3
};

enum class JSONFormat : uint8_t {
	//! Auto-detect format (UNSTRUCTURED / NEWLINE_DELIMITED)
	AUTO_DETECT = 0,
	//! One unit after another, newlines can be anywhere
	UNSTRUCTURED = 1,
	//! Units are separated by newlines, newlines do not occur within Units (NDJSON)
	NEWLINE_DELIMITED = 2,
	//! File is one big array of units
	ARRAY = 3
};

enum class JSONCompression : uint8_t { AUTO_DETECT, UNCOMPRESSED, GZIP, ZSTD };

//! Where a column of a GeoJSON Feature scan reads its value from. A Feature has two independent namespaces - its own
//! members and its "properties" - which can both contain the same name, so columns are resolved by position.
struct JSONFeatureColumn {
	//! Whether to read from the Feature's "properties" object instead of from the Feature itself
	bool from_properties = false;
	//! The JSON key to read
	string key;
};

struct JSONReaderOptions {
	//! Scan type
	JSONScanType type = JSONScanType::READ_JSON;
	//! The format of the JSON
	JSONFormat format = JSONFormat::AUTO_DETECT;
	//! Whether record types in the JSON
	JSONRecordType record_type = JSONRecordType::AUTO_DETECT;
	//! Whether file is compressed or not, and if so which compression type
	JSONCompression compression = JSONCompression::AUTO_DETECT;
	//! Whether or not we should ignore malformed JSON (default to NULL)
	bool ignore_errors = false;
	//! Maximum JSON object size (defaults to 16MB minimum)
	idx_t maximum_object_size = 16777216;
	//! Whether we auto-detect a schema
	bool auto_detect = false;
	//! Whether "format" was explicitly specified
	bool format_specified = false;
	//! Whether "auto_detect" was explicitly specified
	bool auto_detect_specified = false;
	//! Sample size for detecting schema
	idx_t sample_size = STANDARD_VECTOR_SIZE * 10;
	//! Max depth we go to detect nested JSON schema (defaults to unlimited)
	idx_t max_depth = static_cast<idx_t>(-1);
	//! If the average over the fields of an object of the appearances of its fields is less than this threshold, we
	//! default to the MAP type with value type of merged field types
	double field_appearance_threshold = 0.1;
	//! The maximum number of files we sample to sample sample_size rows
	idx_t maximum_sample_files = 32;
	//! Whether we auto-detect and convert JSON strings to integers
	bool convert_strings_to_integers = false;
	//! Whether object keys that differ only in ASCII case are the same key
	bool case_insensitive_keys = false;
	//! Whether GeoJSON is read. Unset means "decide from the file name" (.geojson / .geojsonl)
	std::optional<bool> geojson;
	//! If a struct contains more fields than this threshold with at least 80% similar types, we infer it as MAP type
	idx_t map_inference_threshold = 200;
	//! User-provided list of names (in order)
	vector<string> name_list;
	//! User-provided list of types (in order)
	vector<LogicalType> sql_type_list;
	//! Forced date/timestamp formats
	string date_format;
	string timestamp_format;

	JSONReaderOptions() = default;
	JSONReaderOptions(JSONReaderOptions &&) = default;
	JSONReaderOptions &operator=(JSONReaderOptions &&) = default;
	JSONReaderOptions Copy() const;
};

//! A document in the buffer a scan parsed it from
struct JSONString {
	const char *pointer = nullptr;
	idx_t size = 0;
};

class JSONReader;

//! The state of one scanning thread: the buffer it parses from and the documents of the last parse
struct JSONReaderScanState {
public:
	JSONReaderScanState();
	~JSONReaderScanState();
	JSONReaderScanState(const JSONReaderScanState &) = delete;
	JSONReaderScanState &operator=(const JSONReaderScanState &) = delete;

	//! Makes room for `capacity` bytes (plus padding), keeping the first `keep` bytes
	void EnsureCapacity(idx_t capacity, idx_t keep);
	//! Counts the buffer, which can grow to `capacity` bytes, against the memory limit
	void ReserveBuffer(Context &context, idx_t capacity);
	//! Reset for parsing the next batch of JSON from the current buffer
	void ResetForNextParse();
	//! Reports the documents of the buffer that was scanned, and forgets it
	void FinishBuffer();

public:
	//! Thread-local allocator the documents are parsed into
	JSONAllocator allocator;
	//! The buffer
	char *buffer_ptr = nullptr;
	idx_t buffer_allocated = 0;
	//! The memory of the buffer, as the buffer manager accounts for it
	std::optional<MemoryReservation> buffer_reservation;
	idx_t buffer_size = 0;
	idx_t buffer_offset = 0;
	//! Whether the buffer holds the end of the file (an unterminated last document is then complete)
	bool is_last = false;
	//! Whether this is the first time scanning the current batch
	bool is_first_scan = true;
	bool entire_file = true;
	//! Whether the rest of the file is deliberately not read (everything after a FeatureCollection's "features")
	bool skip_remainder_of_file = false;
	//! Chunk of the file to read when it has not been read yet (partial scans)
	bool needs_to_read = false;
	idx_t chunk_index = 0;
	//! The part of the last buffer that has to move to the front of the next one (entire file scans)
	idx_t prev_buffer_remainder = 0;
	idx_t prev_buffer_offset = 0;
	//! The buffer index (within the file) of the current buffer, and whether its count is still to be reported
	idx_t buffer_index = 0;
	bool buffer_open = false;
	idx_t lines_or_objects_in_buffer = 0;
	//! Documents of the last parse
	idx_t scan_count = 0;
	JSONString units[STANDARD_VECTOR_SIZE];
	yyjson_val *values[STANDARD_VECTOR_SIZE];
	JSONReader *current_reader = nullptr;
};

//! Reads a single JSON file
class JSONReader {
public:
	//! The reader of `file_name`. `options` has to outlive the reader
	JSONReader(const JSONReaderOptions &options, string file_name, JSONFormat format, JSONRecordType record_type,
	           bool skip_feature_collection_prefix);

	//! Opens the file. `file` is the argument the file was given as (a path, or a file struct)
	void Open(Context &context, const Value &file, bool allow_partial);
	void Close();

	//! Determines the format and record type of the file from its first bytes (for the ones that are
	//! AUTO_DETECT in the options). Reads the file from the start, independently of any scan.
	static void DetectFormat(Context &context, const Value &file, const string &file_name,
	                         const JSONReaderOptions &options, JSONFormat &format, JSONRecordType &record_type,
	                         bool &skip_feature_collection_prefix);

	JSONFormat GetFormat() const {
		return format;
	}
	JSONRecordType GetRecordType() const {
		return record_type;
	}
	const string &GetFileName() const {
		return file_name;
	}
	const JSONReaderOptions &GetOptions() const {
		return options;
	}
	//! The number of batches the file can be scanned with in parallel (1 when it has to be read front to back)
	idx_t BatchCount() const;
	//! The size of the file in bytes, 0 if unknown
	idx_t FileSize() const {
		return file_size;
	}

	//! Claims the next batch of the file for the scan state. False when all batches are claimed
	bool ClaimBatch(JSONReaderScanState &state);
	//! Scans the next documents of the claimed batch (0 when the batch is exhausted)
	idx_t Scan(JSONReaderScanState &state);

	//! Records a transform error found in the object `object_index` of the last scan
	void AddTransformError(JSONReaderScanState &state, idx_t object_index, const string &error_message);

	//! The number of bytes read from the file so far
	idx_t BytesRead() const {
		return bytes_read.load();
	}
	//! The size in bytes the buffer of a scan state can grow to when scanning this file
	idx_t BufferCapacity() const {
		return buffer_capacity;
	}
	//! Scan progress in [0, 1]
	double GetProgress() const;
	//! The bytes of the file scanned since the last call, as the profiler counts them: the stored bytes of a
	//! compressed file
	idx_t TakeBytesScanned();

	//! Reports the number of documents of a buffer that is done (grabs the lock)
	void SetBufferLineOrObjectCount(idx_t buffer_index, idx_t count);

private:
	struct JSONError {
		idx_t buf_index;
		idx_t line_or_object_in_buf;
		string error_msg;
	};

	void PrepareForScan(JSONReaderScanState &state);
	bool ParseNextChunk(JSONReaderScanState &state);
	bool ParseJSON(JSONReaderScanState &state, char *const json_start, const idx_t json_size, const idx_t remaining);
	void AddParseError(JSONReaderScanState &state, idx_t line_or_object_in_buf, yyjson_read_err &err,
	                   const string &extra = "");
	void SkipOverArrayStart(JSONReaderScanState &state);
	void FinalizeBuffer(JSONReaderScanState &state);
	//! Reads the next buffer of the file front to back
	bool ReadNextBuffer(JSONReaderScanState &state);
	//! Reads the chunk that was claimed
	void ReadChunk(JSONReaderScanState &state);
	idx_t GetBufferIndex();
	void ThrowObjectSizeError(const idx_t object_size);
	idx_t ReadSequential(char *pointer, idx_t size);

	void AddError(idx_t buf_index, idx_t line_or_object_in_buf, const string &error_msg);
	void ThrowErrorsIfPossible();
	std::optional<idx_t> TryGetLineNumber(idx_t buf_index, idx_t line_or_object_in_buf);

private:
	const JSONReaderOptions &options;
	string file_name;
	JSONFormat format;
	JSONRecordType record_type;
	bool skip_feature_collection_prefix;
	//! The flags documents are parsed with: numbers are kept as text when a column is or contains a DECIMAL
	yyjson_read_flag read_flag;

	std::optional<FileHandle> handle;
	idx_t file_size = 0;
	//! Whether the file is split in chunks that are read at an offset
	bool partial = false;
	idx_t chunk_size = 0;
	idx_t chunk_count = 0;
	idx_t buffer_capacity = 0;

	std::atomic<idx_t> bytes_read {0};
	//! The bytes scanned that were not taken yet, and for a compressed file the stored bytes counted so far
	std::atomic<idx_t> bytes_scanned {0};
	std::atomic<idx_t> stored_bytes_counted {0};
	bool compressed = false;

	mutable std::mutex lock;
	//! Next chunk to hand out (partial) / whether the file was handed out (entire file)
	idx_t next_chunk = 0;
	bool entire_file_claimed = false;
	bool last_read_requested = false;
	//! Next buffer index within the file, and the number of documents per buffer (-1: not yet known)
	idx_t next_buffer_index = 0;
	vector<int64_t> buffer_line_or_object_counts;
	std::unique_ptr<JSONError> error;
};

} // namespace json
} // namespace duckdb
