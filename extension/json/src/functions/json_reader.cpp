#include "json_reader.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace duckdb {
namespace json {

//===--------------------------------------------------------------------===//
// Options
//===--------------------------------------------------------------------===//
JSONReaderOptions JSONReaderOptions::Copy() const {
	JSONReaderOptions result;
	result.type = type;
	result.format = format;
	result.record_type = record_type;
	result.compression = compression;
	result.ignore_errors = ignore_errors;
	result.maximum_object_size = maximum_object_size;
	result.auto_detect = auto_detect;
	result.format_specified = format_specified;
	result.auto_detect_specified = auto_detect_specified;
	result.sample_size = sample_size;
	result.max_depth = max_depth;
	result.field_appearance_threshold = field_appearance_threshold;
	result.maximum_sample_files = maximum_sample_files;
	result.convert_strings_to_integers = convert_strings_to_integers;
	result.case_insensitive_keys = case_insensitive_keys;
	result.geojson = geojson;
	result.map_inference_threshold = map_inference_threshold;
	result.name_list = name_list;
	for (auto &type : sql_type_list) {
		result.sql_type_list.push_back(type.Copy());
	}
	result.date_format = date_format;
	result.timestamp_format = timestamp_format;
	return result;
}

//===--------------------------------------------------------------------===//
// Scan state
//===--------------------------------------------------------------------===//
JSONReaderScanState::JSONReaderScanState() {
}

JSONReaderScanState::~JSONReaderScanState() {
	std::free(buffer_ptr);
}

void JSONReaderScanState::EnsureCapacity(idx_t capacity, idx_t keep) {
	(void)keep;
	const idx_t required = capacity + YYJSON_PADDING_SIZE;
	if (buffer_allocated >= required) {
		return;
	}
	auto new_ptr = static_cast<char *>(std::realloc(buffer_ptr, required));
	if (!new_ptr) {
		throw std::bad_alloc();
	}
	buffer_ptr = new_ptr;
	buffer_allocated = required;
}

void JSONReaderScanState::ReserveBuffer(Context &context, idx_t capacity) {
	const idx_t required = capacity + YYJSON_PADDING_SIZE;
	if (!buffer_reservation) {
		buffer_reservation.emplace(context.ReserveMemory(required));
	} else if (buffer_reservation->GetSize() < required) {
		buffer_reservation->Resize(required);
	}
}

void JSONReaderScanState::ResetForNextParse() {
	allocator.Reset();
	scan_count = 0;
}

void JSONReaderScanState::FinishBuffer() {
	if (!buffer_open) {
		return;
	}
	buffer_open = false;
	current_reader->SetBufferLineOrObjectCount(buffer_index, lines_or_objects_in_buffer);
}

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//
static inline bool IsSpace(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static inline bool IsDigit(char c) {
	return c >= '0' && c <= '9';
}

static inline void TrimWhitespace(JSONString &line) {
	while (line.size != 0 && IsSpace(line.pointer[0])) {
		line.pointer++;
		line.size--;
	}
	while (line.size != 0 && IsSpace(line.pointer[line.size - 1])) {
		line.size--;
	}
}

static inline void SkipWhitespace(const char *buffer_ptr, idx_t &buffer_offset, const idx_t &buffer_size) {
	for (; buffer_offset != buffer_size; buffer_offset++) {
		if (!IsSpace(buffer_ptr[buffer_offset])) {
			break;
		}
	}
}

static inline const char *NextNewline(const char *ptr, const idx_t size) {
	return static_cast<const char *>(memchr(ptr, '\n', size));
}

static inline const char *NextJSONDefault(const char *ptr, const char *const end) {
	idx_t parents = 0;
	while (ptr != end) {
		switch (*ptr++) {
		case '{':
		case '[':
			parents++;
			continue;
		case '}':
		case ']':
			parents--;
			break;
		case '"':
			while (ptr != end) {
				auto string_char = *ptr++;
				if (string_char == '"') {
					break;
				} else if (string_char == '\\') {
					if (ptr != end) {
						ptr++; // Skip the escaped char
					}
				}
			}
			break;
		default:
			continue;
		}

		if (parents == 0) {
			break;
		}
	}

	return ptr;
}

static inline const char *NextJSON(const char *ptr, const idx_t size) {
	D_ASSERT(!IsSpace(*ptr)); // Should be handled before

	const char *const end = ptr + size;
	switch (*ptr) {
	case '{':
	case '[':
	case '"':
		ptr = NextJSONDefault(ptr, end);
		break;
	default:
		// Special case: JSON array containing JSON without clear "parents", i.e., not obj/arr/str
		while (ptr != end) {
			switch (*ptr++) {
			case ',':
			case ']':
				ptr--;
				break;
			default:
				continue;
			}
			break;
		}
	}

	return ptr == end ? nullptr : ptr;
}

//! Skip over a single JSON value, leaving pos just after it
static bool SkipJSONValue(const char *buffer_ptr, const idx_t buffer_size, idx_t &pos) {
	const auto skip_string = [&]() {
		pos++; // opening quote
		while (pos < buffer_size && buffer_ptr[pos] != '"') {
			pos += buffer_ptr[pos] == '\\' ? 2 : 1;
		}
		if (pos >= buffer_size) {
			return false;
		}
		pos++; // closing quote
		return true;
	};

	if (pos >= buffer_size) {
		return false;
	}
	if (buffer_ptr[pos] == '"') {
		return skip_string();
	}
	if (buffer_ptr[pos] == '{' || buffer_ptr[pos] == '[') {
		idx_t depth = 0;
		while (pos < buffer_size) {
			const auto c = buffer_ptr[pos];
			if (c == '"') {
				if (!skip_string()) {
					return false;
				}
				continue;
			}
			if (c == '{' || c == '[') {
				depth++;
			} else if (c == '}' || c == ']') {
				if (--depth == 0) {
					pos++;
					return true;
				}
			}
			pos++;
		}
		return false;
	}
	// A number, or one of true/false/null
	while (pos < buffer_size && buffer_ptr[pos] != ',' && buffer_ptr[pos] != '}' && !IsSpace(buffer_ptr[pos])) {
		pos++;
	}
	return true;
}

//! Recognize a GeoJSON FeatureCollection and locate the '[' that opens its "features" array. This is a textual
//! scan rather than a parse because the array is usually far bigger than the buffer we have here.
static bool FindFeatureCollectionArray(const char *buffer_ptr, const idx_t buffer_size, idx_t &features_offset) {
	idx_t pos = 0;
	SkipWhitespace(buffer_ptr, pos, buffer_size);
	if (pos >= buffer_size || buffer_ptr[pos] != '{') {
		return false;
	}
	pos++;

	while (pos < buffer_size) {
		SkipWhitespace(buffer_ptr, pos, buffer_size);
		if (pos >= buffer_size || buffer_ptr[pos] == '}') {
			return false;
		}
		if (buffer_ptr[pos] == ',') {
			pos++;
			continue;
		}
		if (buffer_ptr[pos] != '"') {
			return false;
		}

		const auto key_start = ++pos;
		while (pos < buffer_size && buffer_ptr[pos] != '"') {
			pos += buffer_ptr[pos] == '\\' ? 2 : 1;
		}
		if (pos >= buffer_size) {
			return false;
		}
		const auto key_size = pos++ - key_start;

		SkipWhitespace(buffer_ptr, pos, buffer_size);
		if (pos >= buffer_size || buffer_ptr[pos] != ':') {
			return false;
		}
		pos++;
		SkipWhitespace(buffer_ptr, pos, buffer_size);
		if (pos >= buffer_size) {
			return false;
		}

		if (key_size == 8 && memcmp(buffer_ptr + key_start, "features", 8) == 0) {
			if (buffer_ptr[pos] != '[') {
				return false;
			}
			// "type" usually precedes "features", but accept the other order as long as nothing contradicts us
			features_offset = pos;
			return true;
		}
		if (key_size == 4 && memcmp(buffer_ptr + key_start, "type", 4) == 0) {
			static constexpr auto FEATURE_COLLECTION = "\"FeatureCollection\"";
			static constexpr idx_t FEATURE_COLLECTION_SIZE = 19;
			if (pos + FEATURE_COLLECTION_SIZE > buffer_size ||
			    memcmp(buffer_ptr + pos, FEATURE_COLLECTION, FEATURE_COLLECTION_SIZE) != 0) {
				return false; // Some other kind of object
			}
		}
		if (!SkipJSONValue(buffer_ptr, buffer_size, pos)) {
			return false;
		}
	}
	return false;
}

static bool SkipBOM(const char *ptr, idx_t size, idx_t &bom_size) {
	bom_size = 0;
	if (size >= 3 && static_cast<unsigned char>(ptr[0]) == 0xEF && static_cast<unsigned char>(ptr[1]) == 0xBB &&
	    static_cast<unsigned char>(ptr[2]) == 0xBF) {
		bom_size = 3;
		return true;
	}
	return false;
}

static std::pair<JSONFormat, JSONRecordType> DetectFormatAndRecordType(char *const buffer_ptr, const idx_t buffer_size,
                                                                       yyjson_alc *alc) {
	// First we do the easy check whether it's NEWLINE_DELIMITED
	auto line_end = NextNewline(buffer_ptr, buffer_size);
	if (line_end != nullptr) {
		idx_t line_size = static_cast<idx_t>(line_end - buffer_ptr);
		SkipWhitespace(buffer_ptr, line_size, buffer_size);

		yyjson_read_err error;
		auto doc = JSONCommon::ReadDocumentUnsafe(buffer_ptr, line_size, JSONCommon::READ_FLAG, alc, &error);
		if (error.code == YYJSON_READ_SUCCESS) { // We successfully read the line
			if (yyjson_is_arr(doc->root) && line_size == buffer_size) {
				// It's just one array, let's actually assume ARRAY, not NEWLINE_DELIMITED
				if (yyjson_arr_size(doc->root) == 0 || yyjson_is_obj(yyjson_arr_get(doc->root, 0))) {
					// Either an empty array (assume records), or an array of objects
					return std::make_pair(JSONFormat::ARRAY, JSONRecordType::RECORDS);
				} else {
					return std::make_pair(JSONFormat::ARRAY, JSONRecordType::VALUES);
				}
			} else if (yyjson_is_obj(doc->root)) {
				return std::make_pair(JSONFormat::NEWLINE_DELIMITED, JSONRecordType::RECORDS);
			} else {
				return std::make_pair(JSONFormat::NEWLINE_DELIMITED, JSONRecordType::VALUES);
			}
		}
	}

	// Skip whitespace
	idx_t buffer_offset = 0;
	SkipWhitespace(buffer_ptr, buffer_offset, buffer_size);
	auto remaining = buffer_size - buffer_offset;

	// We know it's not NEWLINE_DELIMITED at this point, if there's a '{', we know it's not ARRAY either
	// Also if it's fully whitespace we just return something because we don't know
	if (remaining == 0 || buffer_ptr[buffer_offset] == '{') {
		return std::make_pair(JSONFormat::UNSTRUCTURED, JSONRecordType::RECORDS);
	}

	// We know it's not top-level records, if it's not '[', it's not ARRAY either
	if (buffer_ptr[buffer_offset] != '[') {
		return std::make_pair(JSONFormat::UNSTRUCTURED, JSONRecordType::VALUES);
	}

	// It's definitely an ARRAY, but now we have to figure out if there's more than one top-level array
	yyjson_read_err error;
	auto doc =
	    JSONCommon::ReadDocumentUnsafe(buffer_ptr + buffer_offset, remaining, JSONCommon::READ_STOP_FLAG, alc, &error);
	if (error.code == YYJSON_READ_SUCCESS) {
		D_ASSERT(yyjson_is_arr(doc->root));

		// We successfully read something!
		buffer_offset += yyjson_doc_get_read_size(doc);
		SkipWhitespace(buffer_ptr, buffer_offset, buffer_size);
		remaining = buffer_size - buffer_offset;

		if (remaining != 0) { // There's more
			return std::make_pair(JSONFormat::UNSTRUCTURED, JSONRecordType::VALUES);
		}

		// Just one array, check what's in there
		if (yyjson_arr_size(doc->root) == 0 || yyjson_is_obj(yyjson_arr_get(doc->root, 0))) {
			// Either an empty array (assume records), or an array of objects
			return std::make_pair(JSONFormat::ARRAY, JSONRecordType::RECORDS);
		} else {
			return std::make_pair(JSONFormat::ARRAY, JSONRecordType::VALUES);
		}
	}

	// We weren't able to parse an array, could be broken or an array larger than our buffer size, let's skip over '['
	SkipWhitespace(buffer_ptr, ++buffer_offset, buffer_size);
	remaining = buffer_size - buffer_offset;

	// If it's '{' we know there's RECORDS in the ARRAY, else it's VALUES
	if (remaining == 0 || buffer_ptr[buffer_offset] == '{') {
		return std::make_pair(JSONFormat::ARRAY, JSONRecordType::RECORDS);
	}

	// It's not RECORDS, so it must be VALUES
	return std::make_pair(JSONFormat::ARRAY, JSONRecordType::VALUES);
}

static bool HasCompressedExtension(const string &path) {
	auto ends_with = [&](const char *suffix) {
		const idx_t len = std::strlen(suffix);
		if (path.size() < len) {
			return false;
		}
		return StringUtil::CIEquals(path.data() + path.size() - len, len, suffix, len);
	};
	return ends_with(".gz") || ends_with(".zst");
}

//===--------------------------------------------------------------------===//
// JSONReader
//===--------------------------------------------------------------------===//
JSONReader::JSONReader(const JSONReaderOptions &options_p, string file_name_p, JSONFormat format_p,
                       JSONRecordType record_type_p, bool skip_feature_collection_prefix_p)
    : options(options_p), file_name(std::move(file_name_p)), format(format_p), record_type(record_type_p),
      skip_feature_collection_prefix(skip_feature_collection_prefix_p),
      read_flag(JSONCommon::GetReadFlag(JSONCommon::READ_STOP_FLAG, JSONCommon::NumbersAsRaw(options.sql_type_list))) {
}

static bool IsCompressed(const JSONReaderOptions &options, const string &file_name) {
	switch (options.compression) {
	case JSONCompression::GZIP:
	case JSONCompression::ZSTD:
		return true;
	case JSONCompression::UNCOMPRESSED:
		return false;
	default:
		return HasCompressedExtension(file_name);
	}
}

static cxx::FileCompression ToFileCompression(JSONCompression compression) {
	switch (compression) {
	case JSONCompression::GZIP:
		return cxx::FileCompression::GZIP;
	case JSONCompression::ZSTD:
		return cxx::FileCompression::ZSTD;
	case JSONCompression::UNCOMPRESSED:
		return cxx::FileCompression::NONE;
	default:
		return cxx::FileCompression::AUTO_DETECT;
	}
}

static FileHandle OpenFileHandle(Context &context, const Value &file, const string &file_name,
                                 const JSONReaderOptions &options) {
	auto fs = context.GetFileSystem();
	auto open_options = fs.CreateOpenOptions();
	open_options.SetFlag(cxx::FileFlags::READ).SetFlag(cxx::FileFlags::PARALLEL_ACCESS).SetValues(file);
	open_options.SetCompression(ToFileCompression(options.compression));
	return fs.OpenFile(file_name, open_options);
}

static idx_t TryGetFileSize(FileHandle &handle) {
	try {
		return handle.Size();
	} catch (std::exception &) {
		return 0;
	}
}

void JSONReader::Open(Context &context, const Value &file, bool allow_partial) {
	handle.emplace(OpenFileHandle(context, file, file_name, options));
	file_size = TryGetFileSize(*handle);
	// a compressed file is read front to back, and its size does not bound the size of what is read
	compressed = IsCompressed(options, file_name);
	partial = allow_partial && format == JSONFormat::NEWLINE_DELIMITED && file_size != 0 && !compressed;
	buffer_capacity = options.maximum_object_size * 2;
	if (partial) {
		chunk_size = options.maximum_object_size;
		chunk_count = (file_size + chunk_size - 1) / chunk_size;
	} else if (file_size != 0 && !compressed) {
		buffer_capacity = std::min<idx_t>(buffer_capacity, file_size + 4096 + 2 * YYJSON_PADDING_SIZE);
	}
}

void JSONReader::Close() {
	handle.reset();
}

void JSONReader::DetectFormat(Context &context, const Value &file, const string &file_name,
                              const JSONReaderOptions &options, JSONFormat &format, JSONRecordType &record_type,
                              bool &skip_feature_collection_prefix) {
	skip_feature_collection_prefix = false;
	auto handle = OpenFileHandle(context, file, file_name, options);
	const idx_t capacity = options.maximum_object_size * 2;
	const auto file_size = IsCompressed(options, file_name) ? 0 : TryGetFileSize(handle);
	const idx_t request = std::min<idx_t>(capacity - YYJSON_PADDING_SIZE, file_size == 0 ? capacity : file_size);
	std::unique_ptr<char, decltype(&std::free)> buffer(static_cast<char *>(std::malloc(request + YYJSON_PADDING_SIZE)),
	                                                   &std::free);
	if (!buffer) {
		throw std::bad_alloc();
	}
	idx_t read_size = 0;
	while (read_size < request) {
		const auto n = handle.Read(buffer.get() + read_size, request - read_size);
		if (n == 0) {
			break;
		}
		read_size += n;
	}
	if (read_size == 0) {
		// file is empty - nothing to detect
		return;
	}
	auto buffer_ptr = buffer.get();
	JSONAllocator json_allocator;
	idx_t features_offset;
	if (options.geojson.value_or(false) && format == JSONFormat::AUTO_DETECT &&
	    FindFeatureCollectionArray(buffer_ptr, read_size, features_offset)) {
		// A FeatureCollection is one big object whose "features" array holds the rows, so read it as an array
		skip_feature_collection_prefix = true;
		format = JSONFormat::ARRAY;
		if (record_type == JSONRecordType::AUTO_DETECT) {
			record_type = JSONRecordType::RECORDS;
		}
	} else {
		auto detected = DetectFormatAndRecordType(buffer_ptr, read_size, json_allocator.GetYYAlc());
		if (format == JSONFormat::AUTO_DETECT) {
			format = detected.first;
		}
		if (record_type == JSONRecordType::AUTO_DETECT) {
			record_type = detected.second;
		}
	}
}

idx_t JSONReader::BatchCount() const {
	return partial ? chunk_count : 1;
}

idx_t JSONReader::GetBufferIndex() {
	buffer_line_or_object_counts.push_back(-1);
	return next_buffer_index++;
}

void JSONReader::SetBufferLineOrObjectCount(idx_t buffer_index, idx_t count) {
	std::lock_guard<std::mutex> guard(lock);
	D_ASSERT(buffer_index < buffer_line_or_object_counts.size());
	D_ASSERT(buffer_line_or_object_counts[buffer_index] == -1);
	buffer_line_or_object_counts[buffer_index] = static_cast<int64_t>(count);
	// if we have any errors - try to report them after finishing a buffer
	ThrowErrorsIfPossible();
}

void JSONReader::AddParseError(JSONReaderScanState &state, idx_t line_or_object_in_buf, yyjson_read_err &err,
                               const string &extra) {
	string unit = format == JSONFormat::NEWLINE_DELIMITED ? "line" : "record/value";
	auto error_msg = Format("Malformed JSON in file \"%s\", at byte %llu in %s {line}: %s. %s", file_name,
	                        static_cast<unsigned long long>(err.pos + 1), unit, err.msg, extra);
	std::lock_guard<std::mutex> guard(lock);
	AddError(state.buffer_index, line_or_object_in_buf + 1, error_msg);
	ThrowErrorsIfPossible();
	// if we could not throw immediately - finish processing this buffer
	state.buffer_offset = 0;
	state.scan_count = 0;
}

void JSONReader::AddTransformError(JSONReaderScanState &state, idx_t object_index, const string &error_message) {
	D_ASSERT(object_index != static_cast<idx_t>(-1));
	auto line_or_object_in_buffer = state.lines_or_objects_in_buffer - state.scan_count + object_index;
	string unit = format == JSONFormat::NEWLINE_DELIMITED ? "line" : "record/value";
	auto error_msg = Format("JSON transform error in file \"%s\", in %s {line}: %s", file_name, unit, error_message);
	std::lock_guard<std::mutex> guard(lock);
	AddError(state.buffer_index, line_or_object_in_buffer, error_msg);
	ThrowErrorsIfPossible();
	// if we could not throw immediately - finish processing this buffer
	state.buffer_offset = state.buffer_size;
	state.scan_count = 0;
}

void JSONReader::AddError(idx_t buf_index, idx_t line_or_object_in_buf, const string &error_msg) {
	if (error) {
		// we already have an error - check if it happened before this error
		if (error->buf_index < buf_index ||
		    (error->buf_index == buf_index && error->line_or_object_in_buf < line_or_object_in_buf)) {
			// it did! don't record this error
			return;
		}
	} else {
		error = std::make_unique<JSONError>();
	}
	error->buf_index = buf_index;
	error->line_or_object_in_buf = line_or_object_in_buf;
	error->error_msg = error_msg;
}

std::optional<idx_t> JSONReader::TryGetLineNumber(idx_t buf_index, idx_t line_or_object_in_buf) {
	idx_t line = line_or_object_in_buf;
	for (idx_t b_idx = 0; b_idx < buf_index; b_idx++) {
		if (buffer_line_or_object_counts[b_idx] == -1) {
			// this buffer has not been parsed yet - we cannot throw
			return std::nullopt;
		}
		line += static_cast<idx_t>(buffer_line_or_object_counts[b_idx]);
	}
	return line;
}

void JSONReader::ThrowErrorsIfPossible() {
	if (!error) {
		return;
	}
	// check if we finished all buffers before the error buffer
	auto line = TryGetLineNumber(error->buf_index, error->line_or_object_in_buf);
	if (!line) {
		return;
	}
	// we can throw!
	auto formatted_error = StringUtil::Replace(error->error_msg, "{line}", std::to_string(*line + 1));
	throw InvalidInputException(formatted_error);
}

double JSONReader::GetProgress() const {
	if (file_size == 0) {
		return 0;
	}
	return std::min(static_cast<double>(bytes_read.load()) / static_cast<double>(file_size), 1.0);
}

idx_t JSONReader::TakeBytesScanned() {
	return bytes_scanned.exchange(0);
}

void JSONReader::ThrowObjectSizeError(const idx_t object_size) {
	throw InvalidInputException("\"maximum_object_size\" of %llu bytes exceeded while reading file \"%s\" (>%llu "
	                            "bytes).\n Try increasing \"maximum_object_size\".",
	                            static_cast<unsigned long long>(options.maximum_object_size), file_name,
	                            static_cast<unsigned long long>(object_size));
}

void JSONReader::SkipOverArrayStart(JSONReaderScanState &state) {
	// First read of this buffer, check if it's actually an array and skip over the bytes
	auto &buffer_ptr = state.buffer_ptr;
	auto &buffer_offset = state.buffer_offset;
	auto &buffer_size = state.buffer_size;
	SkipWhitespace(buffer_ptr, buffer_offset, buffer_size);
	if (buffer_offset == buffer_size) {
		return; // Empty file
	}
	if (skip_feature_collection_prefix) {
		// Position ourselves on the '[' of the "features" array, then continue as a regular array
		idx_t features_offset;
		if (!FindFeatureCollectionArray(buffer_ptr, buffer_size, features_offset)) {
			throw InvalidInputException("Expected a GeoJSON FeatureCollection in file \"%s\"", file_name);
		}
		buffer_offset = features_offset;
	}
	if (buffer_ptr[buffer_offset] != '[') {
		throw InvalidInputException(
		    "Expected top-level JSON array with format='array', but first character is '%c' in file \"%s\"."
		    "\n Try setting format='auto' or format='newline_delimited'.",
		    buffer_ptr[buffer_offset], file_name);
	}
	SkipWhitespace(buffer_ptr, ++buffer_offset, buffer_size);
	if (buffer_offset >= buffer_size) {
		throw InvalidInputException("Missing closing brace ']' in JSON array with format='array' in file \"%s\"",
		                            file_name);
	}
	if (buffer_ptr[buffer_offset] == ']') {
		// Empty array
		SkipWhitespace(buffer_ptr, ++buffer_offset, buffer_size);
		if (buffer_offset != buffer_size && !skip_feature_collection_prefix) {
			throw InvalidInputException(
			    "Empty array with trailing data when parsing JSON array with format='array' in file \"%s\"", file_name);
		}
		// The rest of the FeatureCollection is not ours to read
		buffer_offset = buffer_size;
		state.skip_remainder_of_file = skip_feature_collection_prefix;
	}
}

bool JSONReader::ParseJSON(JSONReaderScanState &state, char *const json_start, const idx_t json_size,
                           const idx_t remaining) {
	yyjson_doc *doc;
	yyjson_read_err err;
	// If we return strings, we cannot parse INSITU. If we ignore errors, we cannot parse INSITU either: a malformed
	// document could extend into (and modify) the next one
	const bool ignorable_errors = options.ignore_errors && format == JSONFormat::NEWLINE_DELIMITED;
	// A number that does not fit is read as a raw string, which INSITU parsing null-terminates in place - at the root
	// that overwrites the delimiter that follows the document
	idx_t first_char = 0;
	while (first_char < json_size && IsSpace(json_start[first_char])) {
		first_char++;
	}
	const bool number_root =
	    first_char < json_size && (IsDigit(json_start[first_char]) || json_start[first_char] == '-');
	if (options.type == JSONScanType::READ_JSON_OBJECTS || ignorable_errors || number_root) {
		doc = JSONCommon::ReadDocumentUnsafe(json_start, json_size, read_flag, state.allocator.GetYYAlc(), &err);
	} else {
		doc = JSONCommon::ReadDocumentUnsafe(json_start, remaining, read_flag | YYJSON_READ_INSITU,
		                                     state.allocator.GetYYAlc(), &err);
	}
	if (err.code != YYJSON_READ_SUCCESS) {
		auto can_ignore_this_error = options.ignore_errors;
		string extra;
		if (format != JSONFormat::NEWLINE_DELIMITED) {
			can_ignore_this_error = false;
			extra = options.ignore_errors
			            ? "Parse errors cannot be ignored for JSON formats other than 'newline_delimited'"
			            : "";
		}
		if (!can_ignore_this_error) {
			AddParseError(state, state.lines_or_objects_in_buffer, err, extra);
			return false;
		}
	}

	// We parse with YYJSON_STOP_WHEN_DONE, so we need to check this by hand
	const auto read_size = yyjson_doc_get_read_size(doc);
	if (read_size > json_size) {
		// Can't go past the boundary, even with ignore_errors
		err.code = YYJSON_READ_ERROR_UNEXPECTED_END;
		err.msg = "unexpected end of data";
		err.pos = json_size;
		AddParseError(state, state.lines_or_objects_in_buffer, err, "Try auto-detecting the JSON format");
		return false;
	}
	if (read_size < json_size) {
		idx_t off = read_size;
		idx_t rem = json_size;
		SkipWhitespace(json_start, off, rem);
		if (off != rem) { // Between end of document and boundary should be whitespace only
			if (!options.ignore_errors) {
				err.code = YYJSON_READ_ERROR_UNEXPECTED_CONTENT;
				err.msg = "unexpected content after document";
				err.pos = read_size;
				AddParseError(state, state.lines_or_objects_in_buffer, err, "Try auto-detecting the JSON format");
				return false;
			}
			doc = nullptr;
		}
	}

	state.lines_or_objects_in_buffer++;
	if (!doc) {
		state.values[state.scan_count] = nullptr;
		return true;
	}

	// Set the JSONLine and trim
	state.units[state.scan_count] = JSONString {json_start, json_size};
	TrimWhitespace(state.units[state.scan_count]);
	state.values[state.scan_count] = doc->root;
	return true;
}

bool JSONReader::ParseNextChunk(JSONReaderScanState &state) {
	auto &buffer_ptr = state.buffer_ptr;
	auto &buffer_offset = state.buffer_offset;
	auto &buffer_size = state.buffer_size;
	auto &scan_count = state.scan_count;
	for (; scan_count < STANDARD_VECTOR_SIZE; scan_count++) {
		SkipWhitespace(buffer_ptr, buffer_offset, buffer_size);
		auto json_start = buffer_ptr + buffer_offset;
		idx_t remaining = buffer_size - buffer_offset;
		if (remaining == 0) {
			break;
		}
		D_ASSERT(format != JSONFormat::AUTO_DETECT);
		const char *json_end = format == JSONFormat::NEWLINE_DELIMITED ? NextNewline(json_start, remaining)
		                                                               : NextJSON(json_start, remaining);
		if (json_end == nullptr) {
			if (remaining > options.maximum_object_size) {
				ThrowObjectSizeError(remaining);
			}
			// We reached the end of the buffer
			if (!state.is_last) {
				// Last bit of data belongs to the next batch
				state.prev_buffer_remainder = remaining;
				state.prev_buffer_offset = static_cast<idx_t>(json_start - buffer_ptr);
				buffer_offset = buffer_size;
				break;
			}
			json_end = json_start + remaining;
		}

		idx_t json_size = static_cast<idx_t>(json_end - json_start);
		if (!ParseJSON(state, json_start, json_size, remaining)) {
			return false;
		}
		buffer_offset += json_size;

		if (format == JSONFormat::ARRAY) {
			SkipWhitespace(buffer_ptr, buffer_offset, buffer_size);
			if (buffer_ptr[buffer_offset] == ']' && skip_feature_collection_prefix) {
				// Anything after the "features" array (the closing brace, "bbox", ...) is not ours to read.
				// Count the value we just parsed, since breaking here skips the loop's increment.
				buffer_offset = buffer_size;
				state.skip_remainder_of_file = true;
				scan_count++;
				break;
			}
			if (buffer_ptr[buffer_offset] == ',' || buffer_ptr[buffer_offset] == ']') {
				buffer_offset++;
			} else { // We can't ignore this error, even with 'ignore_errors'
				yyjson_read_err err;
				err.code = YYJSON_READ_ERROR_UNEXPECTED_CHARACTER;
				err.msg = "unexpected character";
				err.pos = json_size;
				AddParseError(state, state.lines_or_objects_in_buffer, err);
				return false;
			}
		}
		SkipWhitespace(buffer_ptr, buffer_offset, buffer_size);
	}
	return true;
}

//===--------------------------------------------------------------------===//
// Batches
//===--------------------------------------------------------------------===//
bool JSONReader::ClaimBatch(JSONReaderScanState &state) {
	state.FinishBuffer();
	std::lock_guard<std::mutex> guard(lock);
	state.current_reader = this;
	state.is_first_scan = true;
	state.skip_remainder_of_file = false;
	state.scan_count = 0;
	state.buffer_size = 0;
	state.buffer_offset = 0;
	state.prev_buffer_remainder = 0;
	state.prev_buffer_offset = 0;
	state.is_last = false;
	if (partial) {
		if (next_chunk >= chunk_count) {
			return false;
		}
		state.entire_file = false;
		state.chunk_index = next_chunk++;
		state.buffer_index = GetBufferIndex();
		state.buffer_open = true;
		state.lines_or_objects_in_buffer = 0;
		state.needs_to_read = true;
		return true;
	}
	if (entire_file_claimed) {
		return false;
	}
	entire_file_claimed = true;
	state.entire_file = true;
	state.needs_to_read = false;
	return true;
}

void JSONReader::PrepareForScan(JSONReaderScanState &state) {
	if (!state.is_first_scan) {
		// we have already scanned from this buffer before - just reset and scan the next batch
		state.ResetForNextParse();
		return;
	}
	state.is_first_scan = false;
	state.scan_count = 0;
	if (state.entire_file) {
		return;
	}
	ReadChunk(state);
	state.needs_to_read = false;
	FinalizeBuffer(state);
}

void JSONReader::ReadChunk(JSONReaderScanState &state) {
	const idx_t start = state.chunk_index * chunk_size;
	const idx_t end = std::min(start + chunk_size, file_size);
	// one byte before the chunk tells whether the chunk starts at the start of a line
	const idx_t read_from = start == 0 ? 0 : start - 1;
	idx_t size = end - read_from;
	state.EnsureCapacity(size, 0);
	handle->ReadAt(state.buffer_ptr, size, read_from);
	bytes_read += end - start;
	// the chunks partition the file, so the lines read past the end of a chunk are not counted again
	bytes_scanned += end - start;
	state.buffer_size = size;
	state.buffer_offset = 0;
	// the buffer holds complete lines only
	state.is_last = true;
	if (start > 0) {
		auto newline = NextNewline(state.buffer_ptr, size);
		if (!newline) {
			// no line starts in this chunk
			state.buffer_offset = size;
			memset(state.buffer_ptr + size, 0, YYJSON_PADDING_SIZE);
			return;
		}
		state.buffer_offset = static_cast<idx_t>(newline - state.buffer_ptr) + 1;
	}
	if (end < file_size && state.buffer_offset < size && state.buffer_ptr[size - 1] != '\n') {
		// the last line that starts in this chunk ends after it
		idx_t line_start = state.buffer_offset;
		for (idx_t i = size; i > state.buffer_offset; i--) {
			if (state.buffer_ptr[i - 1] == '\n') {
				line_start = i;
				break;
			}
		}
		// leading whitespace is not part of the line
		SkipWhitespace(state.buffer_ptr, line_start, size);
		idx_t block = 64 * 1024;
		bool found = false;
		while (!found && read_from + size < file_size) {
			const idx_t n = std::min<idx_t>(block, file_size - (read_from + size));
			state.EnsureCapacity(size + n, 0);
			handle->ReadAt(state.buffer_ptr + size, n, read_from + size);
			bytes_read += n;
			auto newline = NextNewline(state.buffer_ptr + size, n);
			if (newline) {
				size += static_cast<idx_t>(newline - (state.buffer_ptr + size)) + 1;
				found = true;
			} else {
				size += n;
			}
			SkipWhitespace(state.buffer_ptr, line_start, size);
			if (!found && size - line_start > options.maximum_object_size) {
				ThrowObjectSizeError(size - line_start);
			}
			block = std::min<idx_t>(block * 2, 4 * 1024 * 1024);
		}
		if (size - line_start > options.maximum_object_size) {
			ThrowObjectSizeError(size - line_start);
		}
		state.buffer_size = size;
	}
	memset(state.buffer_ptr + state.buffer_size, 0, YYJSON_PADDING_SIZE);
}

idx_t JSONReader::ReadSequential(char *pointer, idx_t size) {
	idx_t total_read_size = 0;
	while (total_read_size < size) {
		auto read_size = handle->Read(pointer + total_read_size, size - total_read_size);
		if (read_size == 0) {
			break;
		}
		total_read_size += read_size;
	}
	return total_read_size;
}

bool JSONReader::ReadNextBuffer(JSONReaderScanState &state) {
	state.FinishBuffer();
	if (last_read_requested || !handle) {
		return false;
	}
	const idx_t read_offset = state.prev_buffer_remainder;
	const idx_t request_size = buffer_capacity - read_offset - YYJSON_PADDING_SIZE;
	D_ASSERT(buffer_capacity > read_offset + YYJSON_PADDING_SIZE);
	state.EnsureCapacity(buffer_capacity - YYJSON_PADDING_SIZE, 0);
	// Copy last bit of previous buffer to the beginning
	if (read_offset != 0) {
		memmove(state.buffer_ptr, state.buffer_ptr + state.prev_buffer_offset, read_offset);
	}
	const auto read_size = ReadSequential(state.buffer_ptr + read_offset, request_size);
	bytes_read += read_size;
	if (compressed) {
		const auto stored = handle->GetProgress();
		const auto counted = stored_bytes_counted.exchange(stored);
		bytes_scanned += stored > counted ? stored - counted : 0;
	} else {
		bytes_scanned += read_size;
	}
	{
		std::lock_guard<std::mutex> guard(lock);
		state.buffer_index = GetBufferIndex();
	}
	state.buffer_open = true;
	state.lines_or_objects_in_buffer = 0;
	state.is_last = read_size == 0;
	if (state.is_last) {
		last_read_requested = true;
	}
	state.buffer_size = read_offset + read_size;
	state.buffer_offset = 0;
	state.prev_buffer_remainder = 0;
	state.prev_buffer_offset = 0;
	FinalizeBuffer(state);
	return true;
}

void JSONReader::FinalizeBuffer(JSONReaderScanState &state) {
	// skip over the BOM and the array start if required
	if (state.buffer_index == 0 && state.buffer_size >= state.buffer_offset) {
		idx_t bom_size = 0;
		SkipBOM(state.buffer_ptr + state.buffer_offset, state.buffer_size - state.buffer_offset, bom_size);
		state.buffer_offset += bom_size;
		if (format == JSONFormat::ARRAY) {
			SkipOverArrayStart(state);
		}
	}
	// YYJSON needs this
	memset(state.buffer_ptr + state.buffer_size, 0, YYJSON_PADDING_SIZE);
}

idx_t JSONReader::Scan(JSONReaderScanState &state) {
	PrepareForScan(state);
	while (state.scan_count == 0) {
		while (state.buffer_offset >= state.buffer_size) {
			// we have exhausted the current buffer
			if (!state.entire_file) {
				state.FinishBuffer();
				return 0;
			}
			// read the next buffer
			if (state.skip_remainder_of_file || !ReadNextBuffer(state)) {
				// we have exhausted the file
				state.FinishBuffer();
				return 0;
			}
		}
		if (!ParseNextChunk(state)) {
			// found an error but we can't handle it - return
			state.FinishBuffer();
			return 0;
		}
	}
	return state.scan_count;
}

} // namespace json
} // namespace duckdb
