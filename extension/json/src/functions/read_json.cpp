#include "json_scan.hpp"

#include <algorithm>
#include <cstring>

namespace duckdb {
namespace json {

//===--------------------------------------------------------------------===//
// Bind data
//===--------------------------------------------------------------------===//
void JSONScanData::InitializeFormats() {
	InitializeFormats(options.auto_detect);
}

void JSONScanData::InitializeFormats(bool auto_detect_p) {
	auto formats = std::make_shared<JSONDateFormatMap>();
	// Initialize the formats if anything was specified
	if (!options.date_format.empty()) {
		formats->AddFormat(LogicalTypeId::DATE, options.date_format);
	}
	if (!options.timestamp_format.empty()) {
		formats->AddFormat(LogicalTypeId::TIMESTAMP, options.timestamp_format);
	}
	if (auto_detect_p) {
		// Populate possible date/timestamp formats, assume this is consistent across columns
		formats->AddDetectionFormats();
	}
	date_formats = std::move(formats);
}

static JSONGeometryDetector geometry_detector = nullptr;

void JSONScan::SetGeoJSONGeometryDetector(JSONGeometryDetector detector) {
	geometry_detector = detector;
}

//===--------------------------------------------------------------------===//
// Options
//===--------------------------------------------------------------------===//
static string ValueToString(Context &context, const Value &value) {
	auto cast = value.Cast(context.GetFactory(), context.GetFactory().CreateType(LogicalTypeId::VARCHAR));
	return string(cast.Get<varchar_t>().view());
}

static bool ValueToBool(Context &context, const Value &value) {
	return value.Cast(context.GetFactory(), context.GetFactory().CreateType(LogicalTypeId::BOOLEAN)).Get<bool>();
}

static int64_t ValueToBigInt(Context &context, const Value &value) {
	return value.Cast(context.GetFactory(), context.GetFactory().CreateType(LogicalTypeId::BIGINT)).Get<int64_t>();
}

static JSONCompression ParseCompression(const string &input) {
	auto lower = StringUtil::Lower(input);
	if (lower == "infer" || lower == "auto" || lower == "auto_detect") {
		return JSONCompression::AUTO_DETECT;
	} else if (lower == "uncompressed" || lower == "none" || lower.empty()) {
		return JSONCompression::UNCOMPRESSED;
	} else if (lower == "gzip") {
		return JSONCompression::GZIP;
	} else if (lower == "zstd") {
		return JSONCompression::ZSTD;
	}
	throw NotImplementedException("Unrecognized file compression type \"%s\"", input);
}

bool JSONScan::ParseOption(Context &context, const string &key, const Value &value, JSONReaderOptions &options) {
	auto &factory = context.GetFactory();
	if (value.IsNull()) {
		throw BinderException("Cannot use NULL as argument to key %s", key);
	}
	if (key == "ignore_errors") {
		options.ignore_errors = ValueToBool(context, value);
		return true;
	}
	if (key == "maximum_object_size") {
		auto size = value.Cast(factory, factory.CreateType(LogicalTypeId::UINTEGER)).Get<uint32_t>();
		options.maximum_object_size = std::max<idx_t>(size, options.maximum_object_size);
		return true;
	}
	if (key == "format") {
		auto arg = StringUtil::Lower(ValueToString(context, value));
		static const std::pair<const char *, JSONFormat> FORMAT_OPTIONS[] = {
		    {"auto", JSONFormat::AUTO_DETECT},
		    {"unstructured", JSONFormat::UNSTRUCTURED},
		    {"newline_delimited", JSONFormat::NEWLINE_DELIMITED},
		    {"nd", JSONFormat::NEWLINE_DELIMITED},
		    {"array", JSONFormat::ARRAY}};
		for (auto &option : FORMAT_OPTIONS) {
			if (arg == option.first) {
				options.format = option.second;
				options.format_specified = true;
				return true;
			}
		}
		string valid_options;
		for (auto &option : FORMAT_OPTIONS) {
			valid_options += valid_options.empty() ? "" : ", ";
			valid_options += Format("'%s'", option.first);
		}
		throw BinderException("format must be one of [%s], not '%s'", valid_options, arg);
	}
	if (key == "compression") {
		options.compression = ParseCompression(ValueToString(context, value));
		return true;
	}
	if (key == "array") {
		// "array" is a shorthand for the format - a JSON array of values, or newline-delimited values
		options.format = ValueToBool(context, value) ? JSONFormat::ARRAY : JSONFormat::NEWLINE_DELIMITED;
		options.format_specified = true;
		return true;
	}
	if (key == "columns") {
		auto type = value.GetLogicalType();
		if (type.GetTypeId() != LogicalTypeId::STRUCT) {
			throw BinderException("read_json \"columns\" parameter requires a struct as input.");
		}
		const auto child_count = type.GetStructChildCount();
		for (idx_t i = 0; i < child_count; i++) {
			auto name = type.GetStructChildName(i);
			auto val = value.GetChild(i);
			if (val.IsNull()) {
				throw BinderException("read_json \"columns\" parameter type specification cannot be NULL.");
			}
			options.name_list.push_back(name);
			if (val.GetLogicalType().GetTypeId() != LogicalTypeId::VARCHAR) {
				throw BinderException("read_json \"columns\" parameter type specification must be VARCHAR.");
			}
			options.sql_type_list.push_back(factory.ParseType(val.Get<varchar_t>().view()));
		}
		if (options.name_list.empty()) {
			throw BinderException("read_json \"columns\" parameter needs at least one column.");
		}
		return true;
	}
	if (key == "auto_detect") {
		options.auto_detect = ValueToBool(context, value);
		options.auto_detect_specified = true;
		return true;
	}
	if (key == "geojson") {
		options.geojson = ValueToBool(context, value);
		return true;
	}
	if (key == "sample_size") {
		auto arg = ValueToBigInt(context, value);
		if (arg == -1) {
			options.sample_size = static_cast<idx_t>(-1);
		} else if (arg > 0) {
			options.sample_size = static_cast<idx_t>(arg);
		} else {
			throw BinderException("read_json \"sample_size\" parameter must be positive, or -1 to sample all input "
			                      "files entirely, up to \"maximum_sample_files\" files.");
		}
		return true;
	}
	if (key == "maximum_depth") {
		auto arg = ValueToBigInt(context, value);
		options.max_depth = arg == -1 ? static_cast<idx_t>(-1) : static_cast<idx_t>(arg);
		return true;
	}
	if (key == "field_appearance_threshold") {
		auto arg = value.Cast(factory, factory.CreateType(LogicalTypeId::DOUBLE)).Get<double>();
		if (arg < 0 || arg > 1) {
			throw BinderException("read_json_auto \"field_appearance_threshold\" parameter must be between 0 and 1");
		}
		options.field_appearance_threshold = arg;
		return true;
	}
	if (key == "map_inference_threshold") {
		auto arg = ValueToBigInt(context, value);
		if (arg == -1) {
			options.map_inference_threshold = static_cast<idx_t>(-1);
		} else if (arg >= 0) {
			options.map_inference_threshold = static_cast<idx_t>(arg);
		} else {
			throw BinderException("read_json_auto \"map_inference_threshold\" parameter must be 0 or positive, "
			                      "or -1 to disable map inference for consistent objects.");
		}
		return true;
	}
	if (key == "dateformat" || key == "date_format") {
		auto format_string = ValueToString(context, value);
		if (StringUtil::CIEquals(format_string.data(), format_string.size(), "iso", 3)) {
			format_string = "%Y-%m-%d";
		}
		options.date_format = format_string;

		JSONStrpTimeFormat format;
		auto error = JSONStrpTimeFormat::ParseFormatSpecifier(format_string, format);
		if (!error.empty()) {
			throw BinderException("read_json could not parse \"dateformat\": '%s'.", error);
		}
		return true;
	}
	if (key == "timestampformat" || key == "timestamp_format") {
		auto format_string = ValueToString(context, value);
		if (StringUtil::CIEquals(format_string.data(), format_string.size(), "iso", 3)) {
			format_string = "%Y-%m-%dT%H:%M:%S.%fZ";
		}
		options.timestamp_format = format_string;

		JSONStrpTimeFormat format;
		auto error = JSONStrpTimeFormat::ParseFormatSpecifier(format_string, format);
		if (!error.empty()) {
			throw BinderException("read_json could not parse \"timestampformat\": '%s'.", error);
		}
		return true;
	}
	if (key == "records") {
		// records is tri-state: 'auto', 'true' or 'false'. Accept a boolean for the latter two, since
		// records = false reads more naturally than records = 'false'.
		string arg;
		const auto type_id = value.GetLogicalType().GetTypeId();
		if (type_id == LogicalTypeId::BOOLEAN) {
			arg = value.Get<bool>() ? "true" : "false";
		} else if (type_id == LogicalTypeId::VARCHAR) {
			arg = string(value.Get<varchar_t>().view());
		} else {
			throw BinderException("read_json \"records\" parameter must be a string ('auto', 'true' or 'false') or "
			                      "a boolean, not %s",
			                      value.GetLogicalType().ToText());
		}
		if (arg == "auto") {
			options.record_type = JSONRecordType::AUTO_DETECT;
		} else if (arg == "true") {
			options.record_type = JSONRecordType::RECORDS;
		} else if (arg == "false") {
			options.record_type = JSONRecordType::VALUES;
		} else {
			throw BinderException("read_json requires \"records\" to be one of ['auto', 'true', 'false'].");
		}
		return true;
	}
	if (key == "maximum_sample_files") {
		auto arg = ValueToBigInt(context, value);
		if (arg == -1) {
			options.maximum_sample_files = static_cast<idx_t>(-1);
		} else if (arg > 0) {
			options.maximum_sample_files = static_cast<idx_t>(arg);
		} else {
			throw BinderException("read_json \"maximum_sample_files\" parameter must be positive, or -1 to remove "
			                      "the limit on the number of files used to sample \"sample_size\" rows.");
		}
		return true;
	}
	if (key == "convert_strings_to_integers") {
		options.convert_strings_to_integers = ValueToBool(context, value);
		return true;
	}
	if (key == "case_insensitive_keys") {
		options.case_insensitive_keys = ValueToBool(context, value);
		return true;
	}
	return false;
}

void JSONScan::ParseNamedArguments(TableFunction::BindInput &input, JSONReaderOptions &options) {
	auto &context = input.GetContext();
	const auto counts = input.GetArgumentCounts();
	for (idx_t i = counts.NamedOffset(); i < counts.Total(); i++) {
		auto name = StringUtil::Lower(input.GetArgName(i));
		auto value = input.GetConstantArgument(i);
		if (value.IsNull()) {
			// not specified
			continue;
		}
		if (!ParseOption(context, name, value, options)) {
			throw NotImplementedException("Unimplemented option %s", name);
		}
	}
}

//===--------------------------------------------------------------------===//
// Schema detection
//===--------------------------------------------------------------------===//
static LogicalType RemoveDuplicateStructKeys(Factory &factory, const LogicalType &type, const bool ignore_errors) {
	switch (type.GetTypeId()) {
	case LogicalTypeId::STRUCT: {
		std::unordered_set<string> child_names;
		vector<cxx::TypeParam> params;
		for (idx_t i = 0; i < type.GetStructChildCount(); i++) {
			auto name = type.GetStructChildName(i);
			if (!child_names.insert(StringUtil::Lower(name)).second) {
				if (ignore_errors) {
					continue;
				}
				throw NotImplementedException(
				    "Duplicate name %s in struct auto-detected in JSON, try ignore_errors=true", name);
			}
			params.emplace_back(name, factory.CreateValue(RemoveDuplicateStructKeys(factory, type.GetStructChildType(i),
			                                                                        ignore_errors)));
		}
		return factory.CreateType(LogicalTypeId::STRUCT, params);
	}
	case LogicalTypeId::MAP: {
		vector<cxx::TypeParam> params;
		params.emplace_back(
		    factory.CreateValue(RemoveDuplicateStructKeys(factory, type.GetMapKeyType(), ignore_errors)));
		params.emplace_back(
		    factory.CreateValue(RemoveDuplicateStructKeys(factory, type.GetMapValueType(), ignore_errors)));
		return factory.CreateType(LogicalTypeId::MAP, params);
	}
	case LogicalTypeId::LIST: {
		vector<cxx::TypeParam> params;
		params.emplace_back(
		    factory.CreateValue(RemoveDuplicateStructKeys(factory, type.GetListChildType(), ignore_errors)));
		return factory.CreateType(LogicalTypeId::LIST, params);
	}
	default:
		return type.Copy();
	}
}

//! A GeoJSON Feature: {"type": "Feature", "geometry": ..., "properties": {...}}, optionally with "id" and "bbox".
//! Only these members are allowed, so that ordinary objects that happen to have a "geometry" are not unnested.
static bool IsGeoJSONFeatureType(const LogicalType &type) {
	if (type.GetTypeId() != LogicalTypeId::STRUCT) {
		return false;
	}
	bool has_type = false;
	bool has_geometry = false;
	bool has_properties = false;
	for (idx_t i = 0; i < type.GetStructChildCount(); i++) {
		auto name = type.GetStructChildName(i);
		if (name == "type") {
			has_type = true;
		} else if (name == "geometry") {
			has_geometry = true;
		} else if (name == "properties") {
			has_properties = true;
		} else if (name != "id" && name != "bbox") {
			return false;
		}
	}
	return has_type && has_geometry && has_properties;
}

//! Flatten a Feature into columns: the geometry and the "id"/"bbox" members come from the Feature itself, and
//! every member of "properties" becomes a column of its own.
//! The Feature's own members are emitted first, so that a property of the same name is the one that gets
//! deduplicated by the usual column name collision handling.
static void UnnestGeoJSONFeature(Factory &factory, const LogicalType &type, const bool ignore_errors,
                                 vector<LogicalType> &return_types, vector<string> &names,
                                 vector<JSONFeatureColumn> &feature_columns) {
	const auto add_column = [&](const string &name, const LogicalType &column_type, const bool from_properties,
	                            string key) {
		names.push_back(name);
		return_types.push_back(RemoveDuplicateStructKeys(factory, column_type, ignore_errors));
		feature_columns.push_back(JSONFeatureColumn {from_properties, std::move(key)});
	};

	std::optional<LogicalType> geometry_type, id_type, bbox_type, properties_type;
	for (idx_t i = 0; i < type.GetStructChildCount(); i++) {
		auto name = type.GetStructChildName(i);
		if (name == "geometry") {
			geometry_type = type.GetStructChildType(i);
		} else if (name == "id") {
			id_type = type.GetStructChildType(i);
		} else if (name == "bbox") {
			bbox_type = type.GetStructChildType(i);
		} else if (name == "properties") {
			properties_type = type.GetStructChildType(i);
		}
	}
	D_ASSERT(geometry_type && properties_type);

	add_column("geometry", *geometry_type, false, "geometry");
	if (id_type) {
		add_column("id", *id_type, false, "id");
	}
	if (bbox_type) {
		add_column("bbox", *bbox_type, false, "bbox");
	}
	if (properties_type->GetTypeId() == LogicalTypeId::STRUCT) {
		for (idx_t i = 0; i < properties_type->GetStructChildCount(); i++) {
			auto name = properties_type->GetStructChildName(i);
			add_column(name, properties_type->GetStructChildType(i), true, name);
		}
	} else {
		// Properties were not inferred as a struct (e.g. they were too heterogeneous and became a MAP), so we
		// cannot give each one its own column - keep them together instead
		add_column("properties", *properties_type, false, "properties");
	}
}

//! COPY ... FROM takes its columns from the target table, so their names decide what is read from the Feature
//! itself and what is looked up in its properties. Only members the Features actually have are bound to the
//! Feature, so that e.g. a column named "id" reads an "id" property when there is no Feature-level id - which is
//! where auto-detection would have found it too.
static void BindGeoJSONFeatureColumns(const LogicalType &type, const vector<string> &names,
                                      vector<JSONFeatureColumn> &feature_columns) {
	std::unordered_set<string> feature_members;
	for (idx_t i = 0; i < type.GetStructChildCount(); i++) {
		auto name = type.GetStructChildName(i);
		if (name == "geometry" || name == "id" || name == "bbox") {
			feature_members.insert(StringUtil::Lower(name));
		}
	}
	for (const auto &name : names) {
		const auto from_properties = feature_members.find(StringUtil::Lower(name)) == feature_members.end();
		feature_columns.push_back(JSONFeatureColumn {from_properties, name});
	}
}

static JSONInferenceOptions MakeInferenceOptions(const JSONReaderOptions &options) {
	JSONInferenceOptions inference;
	inference.rename_duplicate_root_members = options.record_type != JSONRecordType::VALUES;
	inference.max_depth = options.max_depth;
	inference.field_appearance_threshold = options.field_appearance_threshold;
	inference.map_inference_threshold = options.map_inference_threshold;
	inference.sample_size = options.sample_size;
	inference.ignore_errors = options.ignore_errors;
	inference.case_insensitive_keys = options.case_insensitive_keys;
	inference.convert_strings_to_integers = options.convert_strings_to_integers;
	// options.timestamp_format is only ever set from the user-supplied timestampformat= parameter,
	// so non-emptiness is exactly "the user specified a timestamp format"
	inference.user_specified_timestamp_format = !options.timestamp_format.empty();
	inference.geometry_detector = options.geojson.value_or(false) ? geometry_detector : nullptr;
	return inference;
}

//! Samples the file and builds the JSON structure that describes it
static unique_ptr<JSONStructureNode> DetectStructure(Context &context, JSONScanData &data) {
	auto &options = data.options;
	auto inference = MakeInferenceOptions(options);
	// duplicate keys are not an error while sampling
	inference.ignore_errors = true;
	auto node = make_uniq<JSONStructureNode>(options.case_insensitive_keys);

	JSONReader reader(options, data.file_path, data.format, options.record_type, data.skip_feature_collection_prefix);
	reader.Open(context, *data.file, false);
	JSONReaderScanState state;
	JSONStringTypeTester tester(context.GetFactory(), data.date_formats.get(), inference);

	idx_t remaining = options.sample_size;
	idx_t total_tuple_count = 0;
	idx_t bytes_consumed = 0;
	if (reader.ClaimBatch(state)) {
		while (remaining != 0) {
			const auto read_count = reader.Scan(state);
			if (read_count == 0) {
				break;
			}
			// the position in the file of the documents that were parsed
			bytes_consumed = reader.BytesRead() - (state.buffer_size - state.buffer_offset);
			total_tuple_count += read_count;

			const auto next = std::min<idx_t>(read_count, remaining);
			for (idx_t i = 0; i < next; i++) {
				const auto &val = state.values[i];
				if (val) {
					JSONStructure::ExtractStructure(val, *node, inference);
				}
			}
			remaining -= next;
			if (!node->ContainsVarchar()) { // Can't refine non-VARCHAR types
				continue;
			}
			node->InitializeCandidateTypes(inference);
			node->RefineCandidateTypes(state.values, next, tester);
		}
		state.FinishBuffer();
	}
	if (total_tuple_count > 0) {
		const auto average_tuple_size = std::max<idx_t>(bytes_consumed / total_tuple_count, 1);
		data.estimated_cardinality = reader.FileSize() / average_tuple_size;
	}
	return node;
}

void JSONScan::StructureToColumns(Context &context, JSONScanData &data, const JSONStructureNode &node,
                                  vector<LogicalType> &return_types, vector<string> &names, vector<string> &keys) {
	auto &factory = context.GetFactory();
	auto &options = data.options;
	// Convert structure to logical type
	auto type = JSONStructure::StructureToType(factory, node, MakeInferenceOptions(options));

	// Auto-detect record type
	if (options.record_type == JSONRecordType::AUTO_DETECT) {
		if (options.geojson.value_or(false) && IsGeoJSONFeatureType(type)) {
			options.record_type = JSONRecordType::FEATURES;
		} else if (type.GetTypeId() == LogicalTypeId::STRUCT) {
			options.record_type = JSONRecordType::RECORDS;
		} else {
			options.record_type = JSONRecordType::VALUES;
		}
	}

	if (options.record_type == JSONRecordType::FEATURES) {
		if (names.empty()) {
			UnnestGeoJSONFeature(factory, type, options.ignore_errors, return_types, names, data.feature_columns);
		} else {
			// COPY - we already have names/types, but we still need to know where each of them is read from
			BindGeoJSONFeatureColumns(type, names, data.feature_columns);
		}
		keys = names;
		return;
	}

	if (!names.empty()) {
		// COPY - we already have names/types
		keys = names;
		return;
	}

	// Auto-detect columns
	if (options.record_type == JSONRecordType::RECORDS) {
		if (type.GetTypeId() == LogicalTypeId::STRUCT) {
			// the reader looks columns up by their exact key, while the names of the columns are unique
			const bool keys_known = node.descriptions.size() == 1 &&
			                        node.descriptions[0].type == LogicalTypeId::STRUCT &&
			                        node.descriptions[0].children.size() == type.GetStructChildCount();
			for (idx_t i = 0; i < type.GetStructChildCount(); i++) {
				return_types.push_back(
				    RemoveDuplicateStructKeys(factory, type.GetStructChildType(i), options.ignore_errors));
				names.push_back(type.GetStructChildName(i));
				keys.push_back(keys_known ? *node.descriptions[0].children[i].key : names.back());
			}
		} else {
			throw BinderException("json_read expected records, but got non-record JSON instead."
			                      "\n Try setting records='auto' or records='false'.");
		}
	} else {
		D_ASSERT(options.record_type == JSONRecordType::VALUES);
		return_types.push_back(RemoveDuplicateStructKeys(factory, type, options.ignore_errors));
		// Newline-delimited bare geometries produce a single GEOMETRY column, which "json" would be a poor name for
		names.push_back(type.GetTypeId() == LogicalTypeId::GEOMETRY ? "geometry" : "json");
		keys.push_back(names.back());
	}
}

void JSONScan::BindSchema(Context &context, JSONScanData &data, vector<LogicalType> &return_types,
                          vector<string> &names) {
	auto &options = data.options;
	names = options.name_list;
	for (auto &type : options.sql_type_list) {
		return_types.push_back(type.Copy());
	}
	if (options.record_type == JSONRecordType::AUTO_DETECT && return_types.size() > 1) {
		// More than one specified column implies records
		options.record_type = JSONRecordType::RECORDS;
	}

	// Specifying column names overrides auto-detect
	if (!return_types.empty()) {
		options.auto_detect = false;
	}

	if (!options.auto_detect) {
		// Need to specify columns if RECORDS and not auto-detecting
		if (return_types.empty()) {
			throw BinderException("When auto_detect=false, read_json requires columns to be specified through the "
			                      "\"columns\" parameter.");
		}
		// If we are reading VALUES, we can only have one column
		if (options.record_type == JSONRecordType::VALUES && return_types.size() != 1) {
			throw BinderException("read_json requires a single column to be specified through the \"columns\" "
			                      "parameter when \"records\" is set to 'false'.");
		}
	}

	// Reading GeoJSON is opt-in, but a .geojson / .geojsonl file name is opt-in enough
	if (!options.geojson.has_value()) {
		const auto &path = data.file_path;
		const auto ends_with = [&](const char *suffix) {
			const idx_t len = std::strlen(suffix);
			return path.size() >= len && StringUtil::CIEquals(path.data() + path.size() - len, len, suffix, len);
		};
		options.geojson = ends_with(".geojson") || ends_with(".geojsonl");
	}
	// COPY ... FROM hardcodes RECORDS because it takes its columns from the target table. For GeoJSON we still
	// need to know whether those columns live in a Feature's "properties", so let the structure decide.
	if (options.geojson.value() && !options.auto_detect && !options.name_list.empty() &&
	    options.record_type == JSONRecordType::RECORDS) {
		options.record_type = JSONRecordType::AUTO_DETECT;
	}

	data.InitializeFormats();

	// The format of the file
	data.format = options.format;
	if (data.format == JSONFormat::AUTO_DETECT) {
		auto record_type = options.record_type;
		JSONReader::DetectFormat(context, *data.file, data.file_path, options, data.format, record_type,
		                         data.skip_feature_collection_prefix);
	}

	vector<string> keys;
	if (options.auto_detect || options.record_type == JSONRecordType::AUTO_DETECT) {
		data.record_type_auto_detected = options.record_type == JSONRecordType::AUTO_DETECT;
		const bool keep_structure = options.name_list.empty();
		auto node = DetectStructure(context, data);
		StructureToColumns(context, data, *node, return_types, names, keys);
		D_ASSERT(return_types.size() == names.size());
		if (keep_structure) {
			data.structure = std::move(node);
		}
	} else {
		keys = names;
	}
	data.key_names = keys;
}

void JSONScan::FinalizeBind(JSONScanData &data) {
	auto &options = data.options;
	data.strict_cast = !options.ignore_errors;
	data.error_duplicate_key = !options.ignore_errors;
	data.error_unknown_key = options.auto_detect && !options.ignore_errors;
	data.key_match = options.case_insensitive_keys ? JSONKeyMatch::CASE_INSENSITIVE : JSONKeyMatch::EXACT;
	// the readers of this scan parse numbers as text when a column is a DECIMAL (JSONReader::read_flag)
	data.numbers_as_raw = JSONCommon::NumbersAsRaw(options.sql_type_list);
}

void JSONScan::DeduplicateColumnNames(vector<string> &names) {
	std::unordered_map<string, idx_t> name_collision_count;
	for (auto &col_name : names) {
		// Taken from CSV header_detection.cpp
		while (name_collision_count.find(StringUtil::Lower(col_name)) != name_collision_count.end()) {
			name_collision_count[StringUtil::Lower(col_name)] += 1;
			col_name = col_name + "_" + std::to_string(name_collision_count[StringUtil::Lower(col_name)]);
		}
		name_collision_count[StringUtil::Lower(col_name)] = 0;
	}
}

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
JSONScanGlobalState::JSONScanGlobalState(const JSONScanData &data_p, unique_ptr<JSONReader> reader_p,
                                         vector<string> names_p, vector<JSONFeatureColumn> feature_columns_p)
    : data(data_p), reader(std::move(reader_p)), names(std::move(names_p)),
      feature_columns(std::move(feature_columns_p)), error_unknown_key(data_p.error_unknown_key) {
	// if we don't need all columns present in the file we don't error on unseen columns
	if (names.size() < data.key_names.size()) {
		error_unknown_key = false;
	}
}

//! GeoJSON Features are read as two groups of columns: those taken from the Feature itself, and those taken from
//! its "properties" object
static bool TransformGeoJSONFeatures(yyjson_val *values[], yyjson_alc *alc, const idx_t count,
                                     JSONScanGlobalState &gstate, vector<Vector> &result_vectors,
                                     JSONTransformOptions &transform_options) {
	D_ASSERT(gstate.feature_columns.size() == gstate.names.size());

	// Group the columns by where they read from, using the JSON key rather than the (deduplicated) column name.
	// A Feature member and a property of the same name are separate columns, so this cannot be done by name.
	vector<string> feature_names, property_names;
	vector<Vector> feature_vectors, property_vectors;
	for (idx_t i = 0; i < gstate.feature_columns.size(); i++) {
		const auto &feature_column = gstate.feature_columns[i];
		const auto from_properties = feature_column.from_properties;
		(from_properties ? property_names : feature_names).push_back(feature_column.key);
		(from_properties ? property_vectors : feature_vectors).push_back(std::move(result_vectors[i]));
	}

	// "type", "properties" and "bbox" are consumed by the unnesting, so unknown keys are expected in both groups
	bool success = true;
	if (!feature_names.empty()) {
		success = JSONTransform::TransformObject(values, alc, count, feature_names, feature_vectors, transform_options,
		                                         nullptr, false);
	}
	if (success && !property_names.empty()) {
		vector<yyjson_val *> property_values(count);
		for (idx_t i = 0; i < count; i++) {
			auto &val = values[i];
			property_values[i] = val && unsafe_yyjson_is_obj(val) ? yyjson_obj_get(val, "properties") : nullptr;
		}
		success = JSONTransform::TransformObject(property_values.data(), alc, count, property_names, property_vectors,
		                                         transform_options, nullptr, false);
	}
	return success;
}

idx_t JSONScanToVectors(Context &context, JSONScanGlobalState &gstate, JSONScanLocalState &lstate,
                        vector<Vector> &output) {
	auto &state = lstate.scan_state;
	auto &reader = *gstate.reader;
	auto &data = gstate.data;
	state.ReserveBuffer(context, reader.BufferCapacity());
	const auto count = reader.Scan(state);
	if (count == 0) {
		return 0;
	}
	if (data.options.type == JSONScanType::READ_JSON_OBJECTS) {
		if (!output.empty()) {
			StringWriter strings(output[0], count);
			for (idx_t i = 0; i < count; i++) {
				if (state.values[i]) {
					strings.WriteValue(string_view(state.units[i].pointer, state.units[i].size));
				} else {
					strings.WriteNull();
				}
			}
		}
		return count;
	}
	if (gstate.names.empty()) {
		return count;
	}

	JSONTransformOptions transform_options(context.GetFactory());
	transform_options.strict_cast = data.strict_cast;
	transform_options.error_duplicate_key = data.error_duplicate_key;
	transform_options.error_missing_key = false;
	transform_options.error_unknown_key = gstate.error_unknown_key;
	transform_options.key_match = data.key_match;
	transform_options.date_formats = data.date_formats.get();
	transform_options.delay_error = true;
	transform_options.numbers_as_raw = data.numbers_as_raw;

	bool success;
	auto alc = state.allocator.GetYYAlc();
	D_ASSERT(data.options.record_type != JSONRecordType::AUTO_DETECT);
	if (data.options.record_type == JSONRecordType::RECORDS) {
		success = JSONTransform::TransformObject(state.values, alc, count, gstate.names, output, transform_options,
		                                         nullptr, transform_options.error_unknown_key);
	} else if (data.options.record_type == JSONRecordType::FEATURES) {
		success = TransformGeoJSONFeatures(state.values, alc, count, gstate, output, transform_options);
	} else {
		D_ASSERT(data.options.record_type == JSONRecordType::VALUES);
		success = JSONTransform::Transform(state.values, alc, output[0], count, transform_options, nullptr);
	}
	if (!success) {
		string hint =
		    data.options.auto_detect
		        ? "\nTry increasing 'sample_size', reducing 'maximum_depth', specifying 'columns', 'format' or "
		          "'records' manually, setting 'ignore_errors' to true, or setting 'union_by_name' to true when "
		          "reading multiple files with a different structure."
		        : "\nTry setting 'auto_detect' to true, specifying 'format' or 'records' manually, or setting "
		          "'ignore_errors' to true.";
		reader.AddTransformError(state, transform_options.object_index, transform_options.error_message + hint);
		// the error could not be thrown yet: this batch is done
		state.FinishBuffer();
		return 0;
	}
	return count;
}

//===--------------------------------------------------------------------===//
// The single-file table functions
//===--------------------------------------------------------------------===//
//! Binds a file of a scan whose schema was already determined: it is read exactly the way that schema was determined
static void BindExpectedSchema(Context &context, TableFunction::BindInput &input, JSONScanData &data,
                               const JSONScanData &source, vector<LogicalType> &return_types, vector<string> &names) {
	auto &options = data.options;
	options.record_type = source.options.record_type;
	options.geojson = source.options.geojson;
	options.auto_detect = false;
	for (idx_t i = 0; i < input.GetExpectedColumnCount(); i++) {
		names.push_back(input.GetExpectedColumnName(i));
		return_types.push_back(input.GetExpectedColumnType(i));
	}
	options.name_list = names;
	options.sql_type_list.clear();
	for (auto &type : return_types) {
		options.sql_type_list.push_back(type.Copy());
	}
	data.key_names = source.key_names;
	data.feature_columns = source.feature_columns;
	data.date_formats = source.date_formats;
	data.format = options.format;
	if (data.format == JSONFormat::AUTO_DETECT) {
		auto record_type = options.record_type;
		JSONReader::DetectFormat(context, *data.file, data.file_path, options, data.format, record_type,
		                         data.skip_feature_collection_prefix);
	}
	JSONScan::FinalizeBind(data);
}

//! Combines the schemas of the sampled files by merging the structures that were detected for them: this gives the
//! schema auto-detection would have found over all of the sampled files at once
static void ReadJSONCombineSchema(TableFunction::CombineSchemaInput &input) {
	auto &context = input.GetContext();
	const auto file_count = input.GetFileCount();
	if (file_count == 0) {
		return;
	}
	auto &first = input.GetBindData<JSONScanData>(0);
	JSONStructureNode merged(first.options.case_insensitive_keys);
	for (idx_t i = 0; i < file_count; i++) {
		auto &data = input.GetBindData<JSONScanData>(i);
		if (!data.structure) {
			// the schema of this file was not auto-detected: the columns are combined by name
			return;
		}
		JSONStructure::MergeNodes(merged, *data.structure);
	}
	JSONScanData result;
	result.options = first.options.Copy();
	if (first.record_type_auto_detected) {
		// the record type is detected again on the merged structure
		result.options.record_type = JSONRecordType::AUTO_DETECT;
	}
	result.date_formats = first.date_formats;
	vector<LogicalType> return_types;
	vector<string> names;
	vector<string> keys;
	JSONScan::StructureToColumns(context, result, merged, return_types, names, keys);
	result.key_names = std::move(keys);
	JSONScan::DeduplicateColumnNames(names);
	result.options.name_list = names;
	for (auto &type : return_types) {
		result.options.sql_type_list.push_back(type.Copy());
	}
	result.options.auto_detect = false;
	JSONScan::FinalizeBind(result);
	for (idx_t i = 0; i < names.size(); i++) {
		input.AddResultColumn(names[i], return_types[i]);
	}
	input.SetBindData<JSONScanData>(std::move(result));
}

static void ReadJSONBind(TableFunction::BindInput &input) {
	auto &context = input.GetContext();
	auto &info = input.GetUserData<JSONScanInfo>();
	JSONScanData data;
	auto &options = data.options;
	options.type = info.type;
	options.format = info.format;
	options.record_type = info.record_type;
	options.auto_detect = info.auto_detect;
	if (info.type == JSONScanType::READ_JSON_OBJECTS) {
		// read_json_objects always emits a single JSON column called "json"
		options.sql_type_list.push_back(JSONType(context.GetFactory()));
		options.name_list.push_back("json");
	}
	JSONScan::ParseNamedArguments(input, options);
	if (options.auto_detect_specified && !options.auto_detect && !options.format_specified) {
		// auto-detection was explicitly turned off - we do not detect the format either
		options.format = JSONFormat::NEWLINE_DELIMITED;
	}

	auto file = input.GetConstantArgument(0);
	if (file.IsNull()) {
		throw BinderException("read_single_json_file requires a non-NULL file name");
	}
	const auto file_type = file.GetLogicalType().GetTypeId();
	if (file_type != LogicalTypeId::VARCHAR && file_type != LogicalTypeId::STRUCT &&
	    file_type != LogicalTypeId::VARIANT) {
		throw BinderException("%s reads a single file - given as a VARCHAR path or as a file struct, not as %s",
		                      info.name, file.GetLogicalType().ToText());
	}
	data.file_path = cxx::GetFilePath(file);
	data.file = std::move(file);

	vector<LogicalType> return_types;
	vector<string> names;
	auto source = input.TryGetExpectedBindData<JSONScanData>();
	if (input.GetExpectedColumnCount() > 0 && source) {
		BindExpectedSchema(context, input, data, *source, return_types, names);
	} else {
		JSONScan::BindSchema(context, data, return_types, names);
		JSONScan::FinalizeBind(data);
		if (options.auto_detect) {
			// the JSON reader looks columns up by their exact key, so the keys were kept before renaming the
			// columns that are duplicates for us (e.g. "id" and "Id")
			JSONScan::DeduplicateColumnNames(names);
		}
	}
	for (idx_t i = 0; i < names.size(); i++) {
		input.AddResultColumn(names[i], return_types[i]);
	}
	input.SetCardinality(data.estimated_cardinality.value_or(42), false);
	input.SetBindData<JSONScanData>(std::move(data));
}

static void ReadJSONInitGlobal(TableFunction::InitGlobalInput &input) {
	auto &data = input.GetBindData<JSONScanData>();
	auto reader = make_uniq<JSONReader>(data.options, data.file_path, data.format, data.options.record_type,
	                                    data.skip_feature_collection_prefix);
	reader->Open(input.GetContext(), *data.file, true);
	auto max_threads = reader->BatchCount();
	vector<string> names;
	vector<JSONFeatureColumn> feature_columns;
	// perform projection pushdown - the JSON reader extracts columns by name, so we only need the projected names
	for (idx_t col_idx = 0; col_idx < input.GetColumnCount(); col_idx++) {
		const auto col_id = input.GetColumnIndex(col_idx);
		names.push_back(data.key_names[col_id]);
		if (!data.feature_columns.empty()) {
			feature_columns.push_back(data.feature_columns[col_id]);
		}
	}
	input.SetMaxThreads(std::max<idx_t>(max_threads, 1));
	input.SetGlobalState<JSONScanGlobalState>(data, std::move(reader), std::move(names), std::move(feature_columns));
}

static void ReadJSONInitLocal(TableFunction::InitLocalInput &input) {
	input.SetLocalState<JSONScanLocalState>();
}

static void ReadJSONClaimBatch(TableFunction::ClaimBatchInput &input) {
	auto &gstate = input.GetGlobalState<JSONScanGlobalState>();
	auto &lstate = input.GetLocalState<JSONScanLocalState>();
	input.SetClaimed(gstate.reader->ClaimBatch(lstate.scan_state));
}

static void ReadJSONExec(TableFunction::ExecInput &input) {
	auto &gstate = input.GetGlobalState<JSONScanGlobalState>();
	auto &lstate = input.GetLocalState<JSONScanLocalState>();
	auto chunk = input.GetOutputChunk();
	vector<Vector> output;
	for (idx_t i = 0; i < gstate.names.size(); i++) {
		output.push_back(chunk.GetVector(i));
	}
	JSONScanToVectors(input.GetContext(), gstate, lstate, output);
}

static void ReadJSONProgress(TableFunction::ProgressInput &input) {
	// A file the scan has not started on has no state yet
	try {
		input.SetProgress(input.GetGlobalState<JSONScanGlobalState>().reader->GetProgress());
	} catch (cxx::Exception &) {
		input.SetProgress(0);
	}
}

static void ReadJSONMetrics(TableFunction::MetricsInput &input) {
	// a file the scan has not started on has no state yet
	auto gstate = input.GetGlobalState<JSONScanGlobalState>();
	if (gstate && gstate->reader) {
		input.SetMetric("operator.bytes_scanned", gstate->reader->TakeBytesScanned());
	}
}

//! The named parameters of the scan functions: unspecified ones are NULL
struct JSONNamedParameter {
	const char *name;
	const char *type;
};

static const JSONNamedParameter COMMON_PARAMETERS[] = {{"maximum_object_size", "UINTEGER"},
                                                       {"ignore_errors", "BOOLEAN"},
                                                       {"format", "VARCHAR"},
                                                       {"compression", "VARCHAR"}};
static const JSONNamedParameter READ_JSON_PARAMETERS[] = {{"columns", "ANY"},
                                                          {"auto_detect", "BOOLEAN"},
                                                          {"geojson", "BOOLEAN"},
                                                          {"sample_size", "BIGINT"},
                                                          {"dateformat", "VARCHAR"},
                                                          {"date_format", "VARCHAR"},
                                                          {"timestampformat", "VARCHAR"},
                                                          {"timestamp_format", "VARCHAR"},
                                                          {"records", "ANY"},
                                                          {"array", "BOOLEAN"},
                                                          {"maximum_sample_files", "BIGINT"},
                                                          {"case_insensitive_keys", "BOOLEAN"}};
static const JSONNamedParameter AUTO_DETECT_PARAMETERS[] = {{"maximum_depth", "BIGINT"},
                                                            {"field_appearance_threshold", "DOUBLE"},
                                                            {"convert_strings_to_integers", "BOOLEAN"},
                                                            {"map_inference_threshold", "BIGINT"}};

static void AddNamedParameters(Factory &factory, cxx::FunctionSignature &signature,
                               const JSONNamedParameter *parameters, idx_t count) {
	for (idx_t i = 0; i < count; i++) {
		const bool any = string(parameters[i].type) == "ANY";
		// An ANY parameter is not cast, so its (unspecified) default is the NULL type
		auto type = any ? factory.CreateType(LogicalTypeId::ANY) : factory.ParseType(parameters[i].type);
		auto null_type = any ? factory.ParseType("\"NULL\"") : factory.ParseType(parameters[i].type);
		signature.AddParameter(parameters[i].name, type, Value::CreateNull(factory, null_type),
		                       cxx::FunctionParameterKind::NAMED_ONLY);
	}
}

static void RegisterSingleFileFunction(Extension &extension, Context &context, JSONScanInfo info) {
	auto &factory = context.GetFactory();
	auto function = TableFunction::Create(factory);
	function.SetName(info.name);
	const bool objects = info.type == JSONScanType::READ_JSON_OBJECTS;
	function.WithSignature([&](cxx::FunctionSignature &signature) {
		signature.AddParameter("file", factory.CreateType(LogicalTypeId::ANY),
		                       cxx::FunctionParameterKind::POSITIONAL_ONLY);
		AddNamedParameters(factory, signature, COMMON_PARAMETERS,
		                   sizeof(COMMON_PARAMETERS) / sizeof(JSONNamedParameter));
		if (!objects) {
			// read_json_objects always emits a single JSON column - it has no schema options
			AddNamedParameters(factory, signature, READ_JSON_PARAMETERS,
			                   sizeof(READ_JSON_PARAMETERS) / sizeof(JSONNamedParameter));
			AddNamedParameters(factory, signature, AUTO_DETECT_PARAMETERS,
			                   sizeof(AUTO_DETECT_PARAMETERS) / sizeof(JSONNamedParameter));
		}
	});
	function.SetUserData<JSONScanInfo>(std::move(info));
	function.SetBindCallback(ReadJSONBind);
	function.SetCombineSchemaCallback(ReadJSONCombineSchema);
	function.SetInitGlobalCallback(ReadJSONInitGlobal);
	function.SetInitLocalCallback(ReadJSONInitLocal);
	function.SetClaimBatchCallback(ReadJSONClaimBatch);
	function.SetExecCallback(ReadJSONExec);
	function.SetProgressCallback(ReadJSONProgress);
	function.SetMetricsCallback(ReadJSONMetrics);
	function.SetProjectionPushdown(true);
	function.SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
	extension.Register(function);
}

static void RegisterMultiFileFunction(Extension &extension, Context &context, const string &name,
                                      const string &single_file_name) {
	auto function = cxx::MultiFileFunction::Create(context.GetFactory());
	function.SetName(name);
	function.SetSingleFileFunction(single_file_name);
	function.SetReaderType("JSON");
	// the schema is determined by combining the schemas of up to 32 files
	function.SetMaximumSampleFiles(32);
	function.SetFileExtension("json");
	extension.Register(function);
}

void RegisterReadJSON(Extension &extension, Context &context) {
	// The single-file functions: read_single_json_file is the public one, the others are what the variants wrap
	RegisterSingleFileFunction(extension, context,
	                           JSONScanInfo("read_single_json_file", JSONScanType::READ_JSON, JSONFormat::AUTO_DETECT,
	                                        JSONRecordType::AUTO_DETECT, true));
	RegisterSingleFileFunction(extension, context,
	                           JSONScanInfo("__read_single_ndjson_file", JSONScanType::READ_JSON,
	                                        JSONFormat::NEWLINE_DELIMITED, JSONRecordType::AUTO_DETECT, true));
	RegisterSingleFileFunction(extension, context,
	                           JSONScanInfo("__read_single_json_objects_file", JSONScanType::READ_JSON_OBJECTS,
	                                        JSONFormat::AUTO_DETECT, JSONRecordType::RECORDS, false));
	RegisterSingleFileFunction(extension, context,
	                           JSONScanInfo("__read_single_ndjson_objects_file", JSONScanType::READ_JSON_OBJECTS,
	                                        JSONFormat::NEWLINE_DELIMITED, JSONRecordType::RECORDS, false));

	RegisterMultiFileFunction(extension, context, "read_json", "read_single_json_file");
	RegisterMultiFileFunction(extension, context, "read_json_auto", "read_single_json_file");
	RegisterMultiFileFunction(extension, context, "read_ndjson", "__read_single_ndjson_file");
	RegisterMultiFileFunction(extension, context, "read_ndjson_auto", "__read_single_ndjson_file");
	RegisterMultiFileFunction(extension, context, "read_json_objects", "__read_single_json_objects_file");
	RegisterMultiFileFunction(extension, context, "read_json_objects_auto", "__read_single_json_objects_file");
	RegisterMultiFileFunction(extension, context, "read_ndjson_objects", "__read_single_ndjson_objects_file");
	RegisterMultiFileFunction(extension, context, "read_ndjson_objects_auto", "__read_single_ndjson_objects_file");
}

} // namespace json
} // namespace duckdb
