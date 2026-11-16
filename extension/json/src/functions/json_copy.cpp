// Ports legacy/json_functions/copy_json.cpp: COPY ... TO ... (FORMAT json | ndjson | jsonl | geojson | geojsonl).
//
// The engine cuts the rows into batches and handles the options that every COPY has (PARTITION_BY, FILE_SIZE_BYTES,
// PER_THREAD_OUTPUT, OVERWRITE, ...). A batch is turned into text on any thread, and the text of the batches is written
// to the file in order. Writing a file is: a prefix, the rows separated by a separator, and a suffix.

#include "json_geojson.hpp"
#include "json_scan.hpp"

#include <limits>

namespace duckdb {
namespace json {

using cxx::CopyFunction;
using cxx::FileCompression;
using cxx::FileFlags;
using cxx::FileHandle;

namespace {

//! What distinguishes the registered copy functions
struct JSONCopyUserData {
	JSONCopyUserData(bool geojson_p, bool default_array_p) : geojson(geojson_p), default_array(default_array_p) {
	}
	//! GeoJSON writes Features, JSON writes the rows as objects
	bool geojson;
	//! Whether the rows are wrapped in a JSON array (a FeatureCollection) unless ARRAY says otherwise
	bool default_array;
};

//! The state of the three-state GeoJSON column options: not given, explicitly none (NULL), or a column name
struct ColumnOption {
	bool given = false;
	string name;
};

struct JSONCopyBindData {
	bool operator==(const JSONCopyBindData &other) const {
		if (types.size() != other.types.size()) {
			return false;
		}
		for (idx_t i = 0; i < types.size(); i++) {
			if (types[i] != other.types[i]) {
				return false;
			}
		}
		return names == other.names && geojson == other.geojson && array == other.array &&
		       date_format_string == other.date_format_string &&
		       timestamp_format_string == other.timestamp_format_string && geometry_index == other.geometry_index &&
		       id_index == other.id_index && property_indices == other.property_indices && bbox == other.bbox &&
		       compression == other.compression;
	}

	vector<string> names;
	vector<LogicalType> types;
	bool geojson = false;
	bool array = false;
	string date_format_string;
	string timestamp_format_string;
	std::optional<JSONTimeFormat> date_format;
	std::optional<JSONTimeFormat> timestamp_format;
	//! GeoJSON: the geometry and id columns (none: null geometry / no id), the other columns are the properties
	std::optional<idx_t> geometry_index;
	std::optional<idx_t> id_index;
	vector<idx_t> property_indices;
	bool bbox = false;
	FileCompression compression = FileCompression::AUTO_DETECT;
};

//! The state of a file that is being written
struct JSONCopyFile {
	JSONCopyFile(FileHandle file_p, idx_t bytes_p) : file(std::move(file_p)), bytes(bytes_p) {
	}
	FileHandle file;
	bool written_anything = false;
	idx_t rows = 0;
	idx_t bytes;
};

//! The text of a batch: its rows separated by the separator
struct JSONCopyBatch {
	string text;
	idx_t rows = 0;
};

} // namespace

//===--------------------------------------------------------------------===//
// Options
//===--------------------------------------------------------------------===//
static const char *FormatName(const JSONCopyUserData &user) {
	return user.geojson ? "GEOJSON" : "JSON";
}

static string LowerCase(string text) {
	for (auto &c : text) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c + ('a' - 'A'));
		}
	}
	return text;
}

[[noreturn]] static void ThrowParameterNull(const JSONCopyUserData &user, const string &option) {
	throw BinderException("COPY (FORMAT %s) parameter %s cannot be NULL.", FormatName(user), option);
}

[[noreturn]] static void ThrowParameterType(const JSONCopyUserData &user, const string &option, const Value &value,
                                            const char *expected) {
	throw BinderException("COPY (FORMAT %s) parameter %s expects a %s argument, but got %s.", FormatName(user), option,
	                      expected, value.GetLogicalType().ToText());
}

static string GetStringOption(const JSONCopyUserData &user, const string &option, const Value &value) {
	if (value.IsNull()) {
		ThrowParameterNull(user, option);
	}
	if (value.GetLogicalType().GetTypeId() != LogicalTypeId::VARCHAR) {
		ThrowParameterType(user, option, value, "VARCHAR");
	}
	auto text = value.Get<varchar_t>();
	return string(text.data(), text.size());
}

static bool GetBooleanOption(Context &context, const JSONCopyUserData &user, const string &option, const Value &value) {
	if (value.IsNull()) {
		ThrowParameterNull(user, option);
	}
	return value.Cast(context.GetFactory(), context.GetFactory().CreateType(LogicalTypeId::BOOLEAN)).Get<bool>();
}

//! NULL means "explicitly none", a name selects a column
static ColumnOption GetColumnOption(const JSONCopyUserData &user, const string &option, const Value &value) {
	ColumnOption result;
	result.given = true;
	if (value.IsNull()) {
		return result;
	}
	result.name = GetStringOption(user, option, value);
	return result;
}

static idx_t FindColumn(const vector<string> &names, const string &name, const char *option_name) {
	for (idx_t i = 0; i < names.size(); i++) {
		if (names[i] == name) {
			return i;
		}
	}
	string available;
	for (auto &candidate : names) {
		if (!available.empty()) {
			available += ", ";
		}
		available += candidate;
	}
	throw BinderException("COPY ... TO ... (FORMAT GEOJSON) option %s refers to column \"%s\", which is not being "
	                      "written. Available columns: %s",
	                      option_name, name, available);
}

static FileCompression ParseCompression(const string &value) {
	const auto compression = LowerCase(value);
	if (compression == "uncompressed" || compression == "none" || compression.empty()) {
		return FileCompression::NONE;
	}
	if (compression == "auto" || compression == "auto_detect" || compression == "infer") {
		return FileCompression::AUTO_DETECT;
	}
	if (compression == "gzip") {
		return FileCompression::GZIP;
	}
	if (compression == "zstd") {
		return FileCompression::ZSTD;
	}
	throw InvalidInputException("Unrecognized file compression type \"%s\"", value);
}

//! Chooses the geometry, id and property columns of a GeoJSON Feature from the names and types of the bind data
static void ResolveGeoJSONLayout(JSONCopyBindData &data, const ColumnOption &geometry_column,
                                 const ColumnOption &id_column) {
	// GEOMETRY_COLUMN selects the feature geometry, defaulting to the first GEOMETRY column
	if (geometry_column.given) {
		if (!geometry_column.name.empty()) {
			const auto index = FindColumn(data.names, geometry_column.name, "GEOMETRY_COLUMN");
			if (data.types[index].GetTypeId() != LogicalTypeId::GEOMETRY) {
				throw BinderException("COPY ... TO ... (FORMAT GEOJSON) option GEOMETRY_COLUMN refers to column "
				                      "\"%s\", which has type %s instead of GEOMETRY",
				                      geometry_column.name, data.types[index].ToText());
			}
			data.geometry_index = index;
		}
	} else {
		for (idx_t i = 0; i < data.types.size(); i++) {
			if (data.types[i].GetTypeId() == LogicalTypeId::GEOMETRY) {
				data.geometry_index = i;
				break;
			}
		}
		if (!data.geometry_index) {
			throw BinderException(
			    "COPY ... TO ... (FORMAT GEOJSON) requires a GEOMETRY column, but none of the columns "
			    "being written has type GEOMETRY."
			    "\n Use GEOMETRY_COLUMN NULL to write features without a geometry instead.");
		}
	}
	// ID_COLUMN selects the Feature-level "id", defaulting to a column named "id" if there is one, so that a
	// GeoJSON file read by DuckDB round-trips without needing the option
	if (id_column.given) {
		if (!id_column.name.empty()) {
			data.id_index = FindColumn(data.names, id_column.name, "ID_COLUMN");
		}
	} else {
		for (idx_t i = 0; i < data.names.size(); i++) {
			if (data.names[i] == "id") {
				data.id_index = i;
				break;
			}
		}
	}
	if (data.geometry_index && data.id_index && *data.geometry_index == *data.id_index) {
		throw BinderException("COPY ... TO ... (FORMAT GEOJSON) cannot write the same column as both the geometry "
		                      "and the Feature id");
	}
	// Everything that is not the geometry or the id becomes a property
	for (idx_t i = 0; i < data.names.size(); i++) {
		if ((data.geometry_index && *data.geometry_index == i) || (data.id_index && *data.id_index == i)) {
			continue;
		}
		data.property_indices.push_back(i);
	}
}

//! Partition values are named with the date and timestamp formats: the engine formats the partition columns that have
//! one before it partitions the rows. The files themselves are written with the JSON time formats.
static void CopyToSelect(CopyFunction::CopyToSelectInput &input) {
	string date_format;
	string timestamp_format;
	for (idx_t i = 0; i < input.GetOptionCount(); i++) {
		const auto name = LowerCase(input.GetOptionName(i));
		const bool is_date = name == "dateformat" || name == "date_format";
		const bool is_timestamp = name == "timestampformat" || name == "timestamp_format";
		if (!is_date && !is_timestamp) {
			continue;
		}
		// the bind callback reports malformed values
		const auto value = input.GetOptionValue(i);
		if (value.IsNull() || value.GetLogicalType().GetTypeId() != LogicalTypeId::VARCHAR) {
			continue;
		}
		auto text = value.Get<varchar_t>();
		(is_date ? date_format : timestamp_format) = string(text.data(), text.size());
	}
	if (date_format.empty() && timestamp_format.empty()) {
		return;
	}
	auto &factory = input.GetContext().GetFactory();
	for (idx_t i = 0; i < input.GetColumnCount(); i++) {
		if (!input.IsPartitionColumn(i)) {
			continue;
		}
		const string *format = nullptr;
		switch (input.GetColumnType(i).GetTypeId()) {
		case LogicalTypeId::DATE:
			format = &date_format;
			break;
		case LogicalTypeId::TIMESTAMP:
		case LogicalTypeId::TIMESTAMP_NS:
		case LogicalTypeId::TIMESTAMP_TZ:
		case LogicalTypeId::TIMESTAMP_TZ_NS:
			format = &timestamp_format;
			break;
		default:
			break;
		}
		if (format && !format->empty()) {
			std::vector<Value> arguments;
			arguments.push_back(Value::Create(factory, varchar_t(string_view(*format))));
			input.ApplyFunction(i, "strftime", arguments);
		}
	}
}

static void CopyToBind(CopyFunction::CopyToBindInput &input) {
	auto &user = input.GetUserData<JSONCopyUserData>();
	auto &context = input.GetContext();

	JSONCopyBindData data;
	data.geojson = user.geojson;
	data.array = user.default_array;
	for (idx_t i = 0; i < input.GetColumnCount(); i++) {
		data.names.push_back(input.GetColumnName(i));
		data.types.push_back(input.GetColumnType(i));
	}

	ColumnOption geometry_column;
	ColumnOption id_column;
	for (idx_t i = 0; i < input.GetOptionCount(); i++) {
		const auto name = LowerCase(input.GetOptionName(i));
		const auto value = input.GetOptionValue(i);
		if (name == "dateformat" || name == "date_format") {
			data.date_format_string = GetStringOption(user, name, value);
			data.date_format = JSONTimeFormat::Parse(data.date_format_string, "date");
		} else if (name == "timestampformat" || name == "timestamp_format") {
			data.timestamp_format_string = GetStringOption(user, name, value);
			data.timestamp_format = JSONTimeFormat::Parse(data.timestamp_format_string, "timestamp");
		} else if (name == "array") {
			data.array = GetBooleanOption(context, user, name, value);
		} else if (user.geojson && name == "geometry_column") {
			geometry_column = GetColumnOption(user, name, value);
		} else if (user.geojson && name == "id_column") {
			id_column = GetColumnOption(user, name, value);
		} else if (user.geojson && name == "bbox") {
			data.bbox = GetBooleanOption(context, user, name, value);
		} else if (name == "compression") {
			data.compression = ParseCompression(GetStringOption(user, name, value));
		} else if (name == "encoding") {
			const auto encoding = LowerCase(GetStringOption(user, name, value));
			if (encoding != "utf-8" && encoding != "utf8") {
				throw NotImplementedException("COPY ... TO ... (FORMAT %s) only supports the UTF-8 encoding",
				                              FormatName(user));
			}
		} else {
			throw BinderException("Unknown option for COPY ... TO ... (FORMAT %s): %s.", FormatName(user), name);
		}
	}

	if (user.geojson) {
		ResolveGeoJSONLayout(data, geometry_column, id_column);
	}
	input.SetBindData<JSONCopyBindData>(std::move(data));
}

//===--------------------------------------------------------------------===//
// Writing
//===--------------------------------------------------------------------===//
static const char *Prefix(const JSONCopyBindData &bind) {
	if (!bind.array) {
		return "";
	}
	return bind.geojson ? "{\"type\":\"FeatureCollection\",\"features\":[\n\t" : "[\n\t";
}

static const char *Separator(const JSONCopyBindData &bind) {
	return bind.array ? ",\n\t" : "\n";
}

static void WriteAll(FileHandle &file, const string &text) {
	idx_t written = 0;
	while (written < text.size()) {
		auto n = file.Write(text.data() + written, text.size() - written);
		if (n == 0) {
			throw InvalidInputException("Failed to write to the file");
		}
		written += n;
	}
}

static void CopyToInit(CopyFunction::CopyToInitInput &input) {
	auto &bind = input.GetBindData<JSONCopyBindData>();
	auto fs = input.GetContext().GetFileSystem();
	auto open_options = fs.CreateOpenOptions();
	open_options.SetFlag(FileFlags::WRITE).SetFlag(FileFlags::FILE_CREATE_NEW).SetCompression(bind.compression);
	auto file = fs.OpenFile(input.GetFilePath(), open_options);
	const string prefix = Prefix(bind);
	WriteAll(file, prefix);
	input.SetInitData<JSONCopyFile>(std::move(file), prefix.size());
}

//! The objects of the rows of a chunk: {"column": value, ...}
static void CreateRowObjects(Context &context, const JSONCopyBindData &bind, yyjson_mut_doc *doc,
                             yyjson_mut_val *objs[], cxx::DataChunk &chunk, idx_t count,
                             const JSONCreateOptions &options) {
	auto vals = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, count);
	for (idx_t i = 0; i < count; i++) {
		objs[i] = yyjson_mut_obj(doc);
	}
	for (idx_t col = 0; col < bind.names.size(); col++) {
		auto vector = chunk.GetVector(col);
		CreateJSONValues(context, doc, vals, vector, count, options);
		const auto &name = bind.names[col];
		for (idx_t i = 0; i < count; i++) {
			yyjson_mut_obj_add(objs[i], yyjson_mut_strncpy(doc, name.data(), name.size()), vals[i]);
		}
	}
}

//! The GeoJSON Features of the rows of a chunk: the geometry column becomes "geometry", the id column "id" and the
//! other columns the members of "properties"
template <class GET_COLUMN>
static void CreateFeatureObjects(Context &context, const JSONCopyBindData &bind, yyjson_mut_doc *doc,
                                 yyjson_mut_val *features[], GET_COLUMN &&get_column, idx_t count,
                                 const JSONCreateOptions &options) {
	auto vals = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, count);
	auto properties = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, count);
	for (idx_t i = 0; i < count; i++) {
		features[i] = yyjson_mut_obj(doc);
		yyjson_mut_obj_add_str(doc, features[i], "type", "Feature");
		properties[i] = yyjson_mut_obj(doc);
	}
	if (bind.id_index) {
		auto id_vector = get_column(*bind.id_index);
		CreateJSONValues(context, doc, vals, id_vector, count, options);
		for (idx_t i = 0; i < count; i++) {
			yyjson_mut_obj_add_val(doc, features[i], "id", vals[i]);
		}
	}
	if (bind.geometry_index) {
		auto geometry_vector = get_column(*bind.geometry_index);
		CreateJSONValues(context, doc, vals, geometry_vector, count, options);
		for (idx_t i = 0; i < count; i++) {
			yyjson_mut_obj_add_val(doc, features[i], "geometry", vals[i]);
		}
		if (bind.bbox) {
			// GeoJSON orders the bbox as all the minimums followed by all the maximums, and includes Z when the
			// geometry has it. Geometries without a finite extent (NULL or empty) get no bbox at all.
			ValueView<blob_t> geometries(geometry_vector);
			for (idx_t i = 0; i < count; i++) {
				if (!geometries.IsValid(i)) {
					continue;
				}
				const auto extent = GetWKBExtent(geometries.Get(i).view());
				if (!extent.has_xy) {
					continue;
				}
				auto bbox = yyjson_mut_arr(doc);
				yyjson_mut_arr_add_real(doc, bbox, extent.x_min);
				yyjson_mut_arr_add_real(doc, bbox, extent.y_min);
				if (extent.has_z) {
					yyjson_mut_arr_add_real(doc, bbox, extent.z_min);
				}
				yyjson_mut_arr_add_real(doc, bbox, extent.x_max);
				yyjson_mut_arr_add_real(doc, bbox, extent.y_max);
				if (extent.has_z) {
					yyjson_mut_arr_add_real(doc, bbox, extent.z_max);
				}
				yyjson_mut_obj_add_val(doc, features[i], "bbox", bbox);
			}
		}
	} else {
		// A Feature without a geometry is still valid GeoJSON, it is simply unlocated
		for (idx_t i = 0; i < count; i++) {
			yyjson_mut_obj_add_null(doc, features[i], "geometry");
		}
	}
	for (auto property_index : bind.property_indices) {
		auto property_vector = get_column(property_index);
		CreateJSONValues(context, doc, vals, property_vector, count, options);
		const auto &name = bind.names[property_index];
		for (idx_t i = 0; i < count; i++) {
			yyjson_mut_obj_add(properties[i], yyjson_mut_strncpy(doc, name.data(), name.size()), vals[i]);
		}
	}
	for (idx_t i = 0; i < count; i++) {
		yyjson_mut_obj_add_val(doc, features[i], "properties", properties[i]);
	}
}

static void CopyToBatch(CopyFunction::CopyToBatchInput &input) {
	const auto &bind = input.GetBindData<JSONCopyBindData>();
	auto &context = input.GetContext();
	auto &factory = context.GetFactory();
	auto collection = input.TakeBatch();

	auto options = ClientJSONCreateOptions(context);
	if (bind.geojson) {
		// GeoJSON writes GeoJSON geometries regardless of the setting
		options.geometry_format = JSONGeometryFormat::GEOJSON;
	}
	options.date_format = bind.date_format ? &*bind.date_format : nullptr;
	options.timestamp_format = bind.timestamp_format ? &*bind.timestamp_format : nullptr;

	vector<LogicalType> types;
	for (auto &type : bind.types) {
		types.push_back(type.Copy());
	}
	auto chunk = factory.CreateDataChunk(types);
	auto shared = collection.CreateSharedScanState();
	auto worker = collection.CreateWorkerScanState();

	JSONCopyBatch batch;
	const string separator = Separator(bind);
	auto &allocator = ThreadLocalJSONAllocator();
	while (collection.Scan(shared, worker, chunk)) {
		const auto count = chunk.GetRowCount();
		if (count == 0) {
			continue;
		}
		allocator.Reset();
		auto alc = allocator.GetYYAlc();
		auto doc = JSONCommon::CreateDocument(alc);
		auto objs = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, count);
		if (bind.geojson) {
			CreateFeatureObjects(
			    context, bind, doc, objs, [&](idx_t col) { return chunk.GetVector(col); }, count, options);
		} else {
			CreateRowObjects(context, bind, doc, objs, chunk, count, options);
		}
		for (idx_t i = 0; i < count; i++) {
			if (batch.rows > 0) {
				batch.text += separator;
			}
			auto text = JSONCommon::WriteVal<yyjson_mut_val>(objs[i], alc);
			batch.text.append(text.data(), text.size());
			batch.rows++;
		}
	}
	input.SetBatchData<JSONCopyBatch>(std::move(batch));
}

static void CopyToFlush(CopyFunction::CopyToFlushInput &input) {
	const auto &bind = input.GetBindData<JSONCopyBindData>();
	auto &state = input.GetInitData<JSONCopyFile>();
	auto &batch = input.GetBatchData<JSONCopyBatch>();
	if (batch.rows == 0) {
		return;
	}
	if (state.written_anything) {
		const string separator = Separator(bind);
		WriteAll(state.file, separator);
		state.bytes += separator.size();
	}
	WriteAll(state.file, batch.text);
	state.bytes += batch.text.size();
	state.rows += batch.rows;
	state.written_anything = true;
}

static void CopyToFinalize(CopyFunction::CopyToFinalizeInput &input) {
	const auto &bind = input.GetBindData<JSONCopyBindData>();
	auto &state = input.GetInitData<JSONCopyFile>();
	string suffix;
	if (bind.array) {
		suffix = bind.geojson ? "\n]}\n" : "\n]\n";
	} else if (state.written_anything) {
		suffix = "\n";
	}
	if (!suffix.empty()) {
		WriteAll(state.file, suffix);
		state.bytes += suffix.size();
	}
	state.file.Close();
}

static void CopyToFileSize(CopyFunction::CopyToFileSizeInput &input) {
	input.SetFileSize(input.GetInitData<JSONCopyFile>().bytes);
}

static void CopyToStatistics(CopyFunction::CopyToStatisticsInput &input) {
	auto &state = input.GetInitData<JSONCopyFile>();
	input.SetRowCount(state.rows);
	input.SetFileSize(state.bytes);
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//
static void RegisterCopyFormat(Extension &extension, Factory &factory, const string &name, const string &file_extension,
                               bool geojson, bool default_array) {
	auto function = CopyFunction::Create(factory);
	function.SetName(name)
	    .SetFileExtension(file_extension)
	    .SetUserData<JSONCopyUserData>(geojson, default_array)
	    .SetCopyToSelectCallback(CopyToSelect)
	    .SetCopyToBindCallback(CopyToBind)
	    .SetCopyToInitCallback(CopyToInit)
	    .SetCopyToBatchCallback(CopyToBatch)
	    .SetCopyToFlushCallback(CopyToFlush)
	    .SetCopyToFinalizeCallback(CopyToFinalize)
	    .SetCopyToFileSizeCallback(CopyToFileSize)
	    .SetCopyToStatisticsCallback(CopyToStatistics);
	SetJSONCopyFromCallbacks(function, geojson);
	extension.Register(function);
}

//===--------------------------------------------------------------------===//
// Internal scalar functions
//===--------------------------------------------------------------------===//
//! The scalar that COPY ... TO ... (FORMAT JSON) used to be rewritten to: calling it by hand fails the way it did
static void InternalJSONBind(ScalarFunction::BindInput &) {
	throw BinderException("__internal_json_copy_to_json is for internal use only");
}

static void InternalJSONExec(ScalarFunction::ExecInput &) {
	throw InternalException("__internal_json_copy_to_json is for internal use only");
}

//! The constant argument, or nullopt for a NULL. `what` names the argument in the errors
static std::optional<string> GetConstantStringArgument(ScalarFunction::BindInput &input, idx_t index,
                                                       const char *what) {
	std::optional<Value> constant;
	try {
		constant = input.GetConstantArgument(index);
	} catch (cxx::Exception &ex) {
		if (what && string(ex.what()).find("must be a constant expression") != string::npos) {
			throw BinderException("__internal_json_copy_to_geojson %s must be constants", what);
		}
		throw;
	}
	auto &value = *constant;
	if (value.IsNull()) {
		return std::nullopt;
	}
	if (value.GetLogicalType().GetTypeId() != LogicalTypeId::VARCHAR) {
		throw BinderException("__internal_json_copy_to_geojson %s must be VARCHAR or NULL", what ? what : "arguments");
	}
	auto text = value.Get<varchar_t>();
	return string(text.data(), text.size());
}

//! The column-selecting options are three-state: NULL means the option was not given (use the default behavior), an
//! empty string means it was explicitly unset, and anything else names a column
static ColumnOption GetColumnArgument(ScalarFunction::BindInput &input, idx_t index) {
	ColumnOption result;
	auto text = GetConstantStringArgument(input, index, "column options");
	if (text) {
		result.given = true;
		result.name = *text;
	}
	return result;
}

static void InternalGeoJSONBind(ScalarFunction::BindInput &input) {
	if (input.GetArgCount() != 6) {
		throw BinderException("__internal_json_copy_to_geojson is for internal use only");
	}
	auto payload_type = input.GetArgType(0);
	if (payload_type.GetTypeId() != LogicalTypeId::STRUCT) {
		throw BinderException("__internal_json_copy_to_geojson is for internal use only");
	}
	JSONCopyBindData data;
	data.geojson = true;
	for (idx_t i = 0; i < payload_type.GetStructChildCount(); i++) {
		data.names.push_back(payload_type.GetStructChildName(i));
		data.types.push_back(payload_type.GetStructChildType(i));
	}
	const auto geometry_column = GetColumnArgument(input, 3);
	const auto id_column = GetColumnArgument(input, 4);
	ResolveGeoJSONLayout(data, geometry_column, id_column);
	// like the column options, a NULL write_bbox means the option was not given
	const auto write_bbox = input.GetConstantArgument(5);
	data.bbox = !write_bbox.IsNull() && write_bbox.Get<bool>();

	auto date_format = GetConstantStringArgument(input, 1, "formats");
	if (date_format) {
		data.date_format_string = *date_format;
		data.date_format = JSONTimeFormat::Parse(*date_format, "date");
	}
	auto timestamp_format = GetConstantStringArgument(input, 2, "formats");
	if (timestamp_format) {
		data.timestamp_format_string = *timestamp_format;
		data.timestamp_format = JSONTimeFormat::Parse(*timestamp_format, "timestamp");
	}
	input.SetBindData<JSONCopyBindData>(std::move(data));
}

//! Turns one row into a GeoJSON Feature: the geometry column becomes "geometry", the ID_COLUMN column becomes "id",
//! and every other column becomes a member of "properties"
static void InternalGeoJSONExec(ScalarFunction::ExecInput &input) {
	const auto &bind = input.GetBindData<JSONCopyBindData>();
	auto &lstate = JSONFunctionLocalState::ResetAndGet(input);
	auto alc = lstate.json_allocator.GetYYAlc();
	auto &context = input.GetContext();
	auto options = ClientJSONCreateOptions(context);
	options.geometry_format = JSONGeometryFormat::GEOJSON;
	options.date_format = bind.date_format ? &*bind.date_format : nullptr;
	options.timestamp_format = bind.timestamp_format ? &*bind.timestamp_format : nullptr;

	const idx_t count = input.GetRowCount();
	auto payload = input.GetArg(0);
	payload.Flatten();
	auto doc = JSONCommon::CreateDocument(alc);
	auto features = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, count);
	CreateFeatureObjects(
	    context, bind, doc, features, [&](idx_t col) { return payload.GetChild(col); }, count, options);

	ValueView<uint8_t> validity(payload);
	auto result = input.GetResult();
	StringWriter writer(result, count);
	for (idx_t i = 0; i < count; i++) {
		if (validity.IsValid(i)) {
			writer.WriteValue(JSONCommon::WriteVal<yyjson_mut_val>(features[i], alc));
		} else {
			writer.WriteNull();
		}
	}
}

void RegisterJSONCopy(Extension &extension, Context &context) {
	auto &factory = context.GetFactory();
	RegisterCopyFormat(extension, factory, "json", "json", false, false);
	RegisterCopyFormat(extension, factory, "ndjson", "ndjson", false, false);
	RegisterCopyFormat(extension, factory, "jsonl", "jsonl", false, false);
	RegisterCopyFormat(extension, factory, "geojson", "geojson", true, true);
	RegisterCopyFormat(extension, factory, "geojsonl", "geojsonl", true, false);

	auto any = factory.CreateType(LogicalTypeId::ANY);
	auto varchar = factory.CreateType(LogicalTypeId::VARCHAR);

	auto to_json = ScalarFunction::Create(factory);
	to_json.SetName("__internal_json_copy_to_json")
	    .WithSignature([&](cxx::FunctionSignature &sig) {
		    sig.AddParameter("value", any);
		    sig.AddParameter("date_format", varchar);
		    sig.AddParameter("timestamp_format", varchar);
		    sig.SetReturnType(JSONType(factory));
	    })
	    .SetBindCallback(InternalJSONBind)
	    .SetExecCallback(InternalJSONExec)
	    .SetNullHandling(cxx::FunctionNullHandling::SPECIAL)
	    .SetFallibility(FunctionFallibility::FALLIBLE);
	extension.Register(to_json);

	auto to_geojson = ScalarFunction::Create(factory);
	to_geojson.SetName("__internal_json_copy_to_geojson")
	    .WithSignature([&](cxx::FunctionSignature &sig) {
		    sig.AddParameter("value", any);
		    sig.AddParameter("date_format", varchar);
		    sig.AddParameter("timestamp_format", varchar);
		    sig.AddParameter("geometry_column", varchar);
		    sig.AddParameter("id_column", varchar);
		    sig.AddParameter("write_bbox", factory.CreateType(LogicalTypeId::BOOLEAN));
		    sig.SetReturnType(JSONType(factory));
	    })
	    .SetBindCallback(InternalGeoJSONBind)
	    .SetInitCallback(JSONFunctionLocalState::Init)
	    .SetExecCallback(InternalGeoJSONExec)
	    .SetNullHandling(cxx::FunctionNullHandling::SPECIAL)
	    .SetFallibility(FunctionFallibility::FALLIBLE);
	extension.Register(to_geojson);
}

} // namespace json
} // namespace duckdb
