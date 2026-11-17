// Ports legacy/json_functions/json_transform.cpp: JSONTransform, which fills the rows of a vector of any type with
// parsed JSON values. The registration of json_transform / from_json and the casts is in register_transform.cpp.
//
// Strings and numbers are not converted by hand: they are put in a vector of the matching source type (VARCHAR,
// BOOLEAN, UBIGINT, BIGINT, DOUBLE) and converted by the engine's own cast (Vector::TryCast), which makes the
// conversions the same as the SQL casts. Rows that fail to convert are the rows that are valid in the source and
// NULL in the target.

#include "json_transform.hpp"

#include <charconv>

namespace duckdb {
namespace json {

JSONTransformOptions::JSONTransformOptions(Factory &factory_p) : factory(factory_p) {
}

JSONTransformOptions::JSONTransformOptions(Factory &factory_p, bool strict_cast_p, bool error_duplicate_key_p,
                                           bool error_missing_key_p, bool error_unknown_key_p)
    : factory(factory_p), strict_cast(strict_cast_p), error_duplicate_key(error_duplicate_key_p),
      error_missing_key(error_missing_key_p), error_unknown_key(error_unknown_key_p) {
}

static JSONGeoJSONParser geojson_parser = nullptr;

void JSONTransform::SetGeoJSONParser(JSONGeoJSONParser parser) {
	geojson_parser = parser;
}

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//
static inline bool IsNullVal(yyjson_val *val) {
	return !val || unsafe_yyjson_is_null(val);
}

static inline string_view GetString(yyjson_val *val) {
	return string_view(unsafe_yyjson_get_str(val), unsafe_yyjson_get_len(val));
}

//! A number kept as text (for a DECIMAL column) reads as the default flags would have read it
static inline yyjson_val *ResolveRaw(yyjson_val *val, yyjson_val &number) {
	if (unsafe_yyjson_is_raw(val) && JSONCommon::ReadRawNumber(val, number)) {
		return &number;
	}
	return val;
}

//! The error of a failed cast comes back rendered ("Conversion Error: ..."); the callers add their own prefix
static string StripConversionPrefix(const string &message) {
	static const string PREFIX = "Conversion Error: ";
	if (message.compare(0, PREFIX.size(), PREFIX) == 0) {
		return message.substr(PREFIX.size());
	}
	return message;
}

static LogicalType MakeType(JSONTransformOptions &options, LogicalTypeId id) {
	return options.factory.CreateType(id);
}

//! A vector that lives in a chunk of its own, with the number of rows it is going to hold
class TempVector {
public:
	TempVector(Factory &factory, const LogicalType &type, idx_t count) : chunk(factory, MakeTypes(type)) {
		vec.emplace(chunk.GetVector(0));
		vec->SetSize(count);
	}

	Vector &Get() {
		return *vec;
	}

private:
	static vector<LogicalType> MakeTypes(const LogicalType &type) {
		vector<LogicalType> types;
		types.push_back(type.Copy());
		return types;
	}

private:
	cxx::DataChunk chunk;
	std::optional<Vector> vec;
};

//! Whether the type is a DECIMAL wider than the fixed-size ones, which stores its digits as a bignum
static bool IsWideDecimal(const LogicalType &type) {
	return type.GetTypeId() == LogicalTypeId::DECIMAL && type.GetDecimalInternalTypeId() == LogicalTypeId::BIGNUM;
}

//! The size in bytes of an element of a vector of a fixed-width type
static idx_t ElementSize(const LogicalType &type) {
	auto id = type.GetTypeId();
	if (id == LogicalTypeId::DECIMAL) {
		id = type.GetDecimalInternalTypeId();
	} else if (id == LogicalTypeId::ENUM) {
		id = type.GetEnumInternalTypeId();
	}
	switch (id) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::UTINYINT:
		return 1;
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::USMALLINT:
		return 2;
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DATE:
		return 4;
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return 8;
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::UUID:
	case LogicalTypeId::INTERVAL:
		return 16;
	default:
		throw InternalException("Unexpected type in ElementSize: %s", type.ToText());
	}
}

static bool IsIntegralType(LogicalTypeId id) {
	switch (id) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
		return true;
	default:
		return false;
	}
}

//===--------------------------------------------------------------------===//
// Numbers
//===--------------------------------------------------------------------===//
enum class SourceKind : uint8_t { NONE = 0, STRING = 1, BOOL = 2, UINT = 3, SINT = 4, REAL = 5 };
static constexpr idx_t SOURCE_KIND_COUNT = 6;

static LogicalTypeId SourceTypeId(SourceKind kind) {
	switch (kind) {
	case SourceKind::STRING:
		return LogicalTypeId::VARCHAR;
	case SourceKind::BOOL:
		return LogicalTypeId::BOOLEAN;
	case SourceKind::UINT:
		return LogicalTypeId::UBIGINT;
	case SourceKind::SINT:
		return LogicalTypeId::BIGINT;
	case SourceKind::REAL:
		return LogicalTypeId::DOUBLE;
	default:
		throw InternalException("Unexpected source kind");
	}
}

//! Which source vector the value goes to. Arrays and objects have none (they never convert to a number)
static SourceKind ClassifyNumeric(yyjson_val *val, bool decimal, uint64_t &bits, bool strict_integral) {
	D_ASSERT(!IsNullVal(val));
	switch (unsafe_yyjson_get_tag(val)) {
	case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NOESC:
	case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NONE: {
		if (strict_integral) {
			// A strict integer cast does not take a decimal point or an exponent
			auto str = GetString(val);
			if (str.find_first_of(".eE") != string_view::npos) {
				return SourceKind::NONE;
			}
		}
		return SourceKind::STRING;
	}
	case YYJSON_TYPE_RAW | YYJSON_SUBTYPE_NONE: {
		yyjson_val number;
		if (!decimal && JSONCommon::ReadRawNumber(val, number)) {
			return ClassifyNumeric(&number, decimal, bits, strict_integral);
		}
		return SourceKind::STRING;
	}
	case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE:
	case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE:
		return SourceKind::NONE;
	case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_TRUE:
	case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_FALSE:
		bits = unsafe_yyjson_get_bool(val) ? 1 : 0;
		return SourceKind::BOOL;
	case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT:
		bits = unsafe_yyjson_get_uint(val);
		return SourceKind::UINT;
	case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_SINT:
		bits = static_cast<uint64_t>(unsafe_yyjson_get_sint(val));
		return SourceKind::SINT;
	case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL: {
		auto real = unsafe_yyjson_get_real(val);
		std::memcpy(&bits, &real, sizeof(bits));
		return SourceKind::REAL;
	}
	default:
		throw InternalException("Unknown yyjson tag in GetValueNumerical");
	}
}

//! The string behind a value classified as STRING
static inline string_view StringOfSource(yyjson_val *val) {
	return GetString(val);
}

static void FillSource(Vector &source, SourceKind kind, yyjson_val *vals[], const vector<SourceKind> &kinds,
                       const vector<uint64_t> &bits, idx_t count) {
	auto mask = source.GetValidityMutable();
	mask.SetAllInvalid(count);
	switch (kind) {
	case SourceKind::STRING: {
		auto data = source.GetDataMutable<varchar_t>();
		for (idx_t i = 0; i < count; i++) {
			if (kinds[i] == kind) {
				auto str = StringOfSource(vals[i]);
				data[i] = varchar_t(str.data(), static_cast<uint32_t>(str.size()));
				mask.SetValid(i);
			}
		}
		break;
	}
	case SourceKind::BOOL: {
		auto data = source.GetDataMutable<bool>();
		for (idx_t i = 0; i < count; i++) {
			if (kinds[i] == kind) {
				data[i] = bits[i] != 0;
				mask.SetValid(i);
			}
		}
		break;
	}
	case SourceKind::UINT: {
		auto data = source.GetDataMutable<uint64_t>();
		for (idx_t i = 0; i < count; i++) {
			if (kinds[i] == kind) {
				data[i] = bits[i];
				mask.SetValid(i);
			}
		}
		break;
	}
	case SourceKind::SINT: {
		auto data = source.GetDataMutable<int64_t>();
		for (idx_t i = 0; i < count; i++) {
			if (kinds[i] == kind) {
				data[i] = static_cast<int64_t>(bits[i]);
				mask.SetValid(i);
			}
		}
		break;
	}
	case SourceKind::REAL: {
		auto data = source.GetDataMutable<double>();
		for (idx_t i = 0; i < count; i++) {
			if (kinds[i] == kind) {
				std::memcpy(&data[i], &bits[i], sizeof(double));
				mask.SetValid(i);
			}
		}
		break;
	}
	default:
		throw InternalException("Unexpected source kind");
	}
}

//! Numbers, booleans, and DECIMAL
static bool TransformNumeric(yyjson_val *vals[], Vector &result, const idx_t count, JSONTransformOptions &options,
                             const LogicalType &result_type, bool decimal) {
	Sized(result, count);
	if (count == 0) {
		return true;
	}
	const bool strict_integral = options.strict_cast && IsIntegralType(result_type.GetTypeId());
	vector<SourceKind> kinds(count, SourceKind::NONE);
	vector<uint64_t> bits(count, 0);
	idx_t kind_counts[SOURCE_KIND_COUNT] = {0};
	for (idx_t i = 0; i < count; i++) {
		if (IsNullVal(vals[i])) {
			continue;
		}
		kinds[i] = ClassifyNumeric(vals[i], decimal, bits[i], strict_integral);
		kind_counts[static_cast<idx_t>(kinds[i])]++;
	}

	bool first = true;
	for (idx_t k = 1; k < SOURCE_KIND_COUNT; k++) {
		if (kind_counts[k] == 0) {
			continue;
		}
		const auto kind = static_cast<SourceKind>(k);
		TempVector source(options.factory, MakeType(options, SourceTypeId(kind)), count);
		FillSource(source.Get(), kind, vals, kinds, bits, count);
		if (first) {
			first = false;
			source.Get().TryCast(options.factory, result);
			result.Flatten();
			continue;
		}
		// Merge the rows of this kind into the result
		TempVector converted(options.factory, result_type, count);
		source.Get().TryCast(options.factory, converted.Get());
		converted.Get().Flatten();
		auto converted_view = converted.Get().GetView();
		auto mask = result.GetValidityMutable();
		if (IsWideDecimal(result_type)) {
			// the digits of a wide decimal are a bignum that lives in the heap of its vector
			auto source = converted_view.Data<bignum_t>();
			for (idx_t i = 0; i < count; i++) {
				if (kinds[i] == kind && converted_view.IsValid(i)) {
					result.AssignString(i, source[i].view());
					mask.SetValid(i);
				}
			}
			continue;
		}
		const auto size = ElementSize(result_type);
		auto target = static_cast<uint8_t *>(result.GetDataMutable());
		for (idx_t i = 0; i < count; i++) {
			if (kinds[i] == kind && converted_view.IsValid(i)) {
				std::memcpy(target + i * size, converted_view.Data<uint8_t>() + i * size, size);
				mask.SetValid(i);
			}
		}
	}
	if (first) {
		// Nothing to convert
		result.GetValidityMutable().SetAllInvalid(count);
	}

	bool success = true;
	if (options.strict_cast) {
		auto view = result.GetView();
		for (idx_t i = 0; i < count; i++) {
			if (!IsNullVal(vals[i]) && !view.IsValid(i)) {
				// The message is the one of the last failing value, the index the one of the first
				options.error_message = Format("Failed to cast value to %s: %s", decimal ? "decimal" : "numerical",
				                               JSONCommon::ValToString(vals[i], 50));
				if (success) {
					options.object_index = i;
				}
				success = false;
			}
		}
	}
	return success;
}

//===--------------------------------------------------------------------===//
// Strings
//===--------------------------------------------------------------------===//
bool JSONTransform::GetStringVector(yyjson_val *vals[], const idx_t count, const LogicalType &target,
                                    Vector &string_vector, JSONTransformOptions &options) {
	string_vector.SetSize(count);
	auto data = string_vector.GetDataMutable<varchar_t>();
	auto mask = string_vector.GetValidityMutable();
	mask.SetAllValid(count);

	bool success = true;
	for (idx_t i = 0; i < count; i++) {
		const auto &val = vals[i];
		if (IsNullVal(val)) {
			mask.SetInvalid(i);
			continue;
		}

		if (!unsafe_yyjson_is_str(val)) {
			mask.SetInvalid(i);
			if (success && options.strict_cast) {
				options.error_message =
				    Format("Unable to cast '%s' to %s", JSONCommon::ValToString(val, 50), string(target.GetName()));
				options.object_index = i;
				success = false;
			}
			continue;
		}

		auto str = GetString(val);
		data[i] = varchar_t(str.data(), static_cast<uint32_t>(str.size()));
	}
	return success;
}

static bool TransformFromString(yyjson_val *vals[], Vector &result, const idx_t count, JSONTransformOptions &options) {
	Sized(result, count);
	if (count == 0) {
		return true;
	}
	TempVector string_vector(options.factory, MakeType(options, LogicalTypeId::VARCHAR), count);

	bool success = true;
	auto result_type = result.GetLogicalType();
	if (!JSONTransform::GetStringVector(vals, count, result_type, string_vector.Get(), options)) {
		success = false;
	}

	string cast_error;
	if (!string_vector.Get().TryCast(options.factory, result, &cast_error) && options.strict_cast) {
		options.error_message = StripConversionPrefix(cast_error);
		options.object_index = 0; // Can't get line number information here
		options.error_message +=
		    "\n If this error occurred during read_json, line/object number information is approximate";
		success = false;
	}
	return success;
}

//! The text of the values that are not strings, as the VARCHAR cast writes it
class ScalarTextRenderer {
public:
	ScalarTextRenderer(JSONTransformOptions &options_p, yyjson_alc *alc_p)
	    : options(options_p), alc(alc_p), raw_writer(alc_p) {
	}

	//! Renders the doubles among the values in one batch
	void Prepare(yyjson_val *vals[], idx_t count) {
		vector<double> doubles;
		for (idx_t i = 0; i < count; i++) {
			if (IsNullVal(vals[i])) {
				continue;
			}
			yyjson_val number;
			auto val = ResolveRaw(vals[i], number);
			if (unsafe_yyjson_get_tag(val) == (YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL)) {
				doubles.push_back(unsafe_yyjson_get_real(val));
			}
		}
		if (doubles.empty()) {
			return;
		}
		TempVector source(options.factory, MakeType(options, LogicalTypeId::DOUBLE), doubles.size());
		auto data = source.Get().GetDataMutable<double>();
		for (idx_t i = 0; i < doubles.size(); i++) {
			data[i] = doubles[i];
		}
		TempVector text(options.factory, MakeType(options, LogicalTypeId::VARCHAR), doubles.size());
		source.Get().Cast(options.factory, text.Get());
		ValueView<varchar_t> texts(text.Get());
		for (idx_t i = 0; i < doubles.size(); i++) {
			auto value = texts.Get(i);
			real_texts.emplace_back(value.data(), value.size());
		}
	}

	//! Writes the next value. Values have to be written in the order they were prepared in
	void Write(StringWriter &writer, yyjson_val *val) {
		yyjson_val number;
		val = ResolveRaw(val, number);
		switch (unsafe_yyjson_get_tag(val)) {
		case YYJSON_TYPE_RAW | YYJSON_SUBTYPE_NONE:
		case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NOESC:
		case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NONE:
			writer.WriteValue(GetString(val));
			return;
		case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE:
		case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE:
			writer.WriteValue(options.numbers_as_raw ? raw_writer.Write(val)
			                                         : JSONCommon::WriteVal<yyjson_val>(val, alc));
			return;
		case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_TRUE:
		case YYJSON_TYPE_BOOL | YYJSON_SUBTYPE_FALSE:
			writer.WriteValue(unsafe_yyjson_get_bool(val) ? "true" : "false");
			return;
		case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_UINT:
			WriteInteger(writer, unsafe_yyjson_get_uint(val));
			return;
		case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_SINT:
			WriteInteger(writer, unsafe_yyjson_get_sint(val));
			return;
		case YYJSON_TYPE_NUM | YYJSON_SUBTYPE_REAL:
			D_ASSERT(real_cursor < real_texts.size());
			writer.WriteValue(real_texts[real_cursor++]);
			return;
		default:
			throw InternalException("Unknown yyjson tag in GetValueString");
		}
	}

private:
	template <class T>
	static void WriteInteger(StringWriter &writer, T value) {
		char buffer[32];
		auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
		writer.WriteValue(string_view(buffer, static_cast<size_t>(result.ptr - buffer)));
	}

private:
	JSONTransformOptions &options;
	yyjson_alc *alc;
	JSONRawNumberWriter raw_writer;
	vector<string> real_texts;
	idx_t real_cursor = 0;
};

static bool TransformToString(yyjson_val *vals[], yyjson_alc *alc, Vector &result, const idx_t count,
                              JSONTransformOptions &options) {
	ScalarTextRenderer renderer(options, alc);
	renderer.Prepare(vals, count);
	StringWriter writer(result, count);
	for (idx_t i = 0; i < count; i++) {
		if (IsNullVal(vals[i])) {
			writer.WriteNull();
		} else {
			renderer.Write(writer, vals[i]);
		}
	}
	// Can always transform to string
	return true;
}

static bool TransformToJSON(yyjson_val *vals[], yyjson_alc *alc, Vector &result, const idx_t count,
                            JSONTransformOptions &options) {
	StringWriter writer(result, count);
	JSONRawNumberWriter raw_writer(alc);
	for (idx_t i = 0; i < count; i++) {
		const auto &val = vals[i];
		if (IsNullVal(val)) {
			writer.WriteNull();
		} else if (options.numbers_as_raw) {
			writer.WriteValue(raw_writer.Write(val));
		} else {
			writer.WriteValue(JSONCommon::WriteVal(val, alc));
		}
	}
	// Can always transform to JSON
	return true;
}

//! BLOB reads as the existing VARCHAR rendering, except JSON strings go through the VARCHAR -> BLOB cast so their
//! \xNN escapes decode into bytes. Rendering non-strings through that cast would misread ordinary JSON escapes
//! (e.g. \n is not a valid BLOB escape), so their rendered text is written directly as bytes instead.
static bool TransformToBlob(yyjson_val *vals[], yyjson_alc *alc, Vector &result, const idx_t count,
                            JSONTransformOptions &options) {
	Sized(result, count);
	if (count == 0) {
		return true;
	}
	bool success = true;

	// Decode the strings
	vector<yyjson_val *> strings(count, nullptr);
	bool any_string = false;
	for (idx_t i = 0; i < count; i++) {
		if (!IsNullVal(vals[i]) && unsafe_yyjson_is_str(vals[i])) {
			strings[i] = vals[i];
			any_string = true;
		}
	}
	std::optional<TempVector> decoded;
	if (any_string) {
		TempVector string_vector(options.factory, MakeType(options, LogicalTypeId::VARCHAR), count);
		JSONTransform::GetStringVector(strings.data(), count, result.GetLogicalType(), string_vector.Get(), options);
		decoded.emplace(options.factory, result.GetLogicalType(), count);
		string cast_error;
		string_vector.Get().TryCast(options.factory, decoded->Get(), &cast_error);
		auto view = decoded->Get().GetView();
		for (idx_t i = 0; i < count; i++) {
			if (strings[i] && !view.IsValid(i) && success && options.strict_cast) {
				options.error_message = StripConversionPrefix(cast_error);
				options.object_index = i;
				success = false;
			}
		}
	}

	ScalarTextRenderer renderer(options, alc);
	renderer.Prepare(vals, count);
	StringWriter writer(result, count);
	std::optional<ValueView<blob_t>> decoded_values;
	if (decoded) {
		decoded_values.emplace(decoded->Get());
	}
	for (idx_t i = 0; i < count; i++) {
		if (IsNullVal(vals[i])) {
			writer.WriteNull();
		} else if (strings[i]) {
			if (decoded_values->IsValid(i)) {
				writer.WriteValue(decoded_values->Get(i).view());
			} else {
				writer.WriteNull();
			}
		} else {
			renderer.Write(writer, vals[i]);
		}
	}
	return success;
}

//! GEOMETRY accepts both WKT strings and GeoJSON geometry fragments. A JSON object is never valid WKT, so the two
//! are unambiguous and no option is needed to tell them apart.
static bool TransformGeometry(yyjson_val *vals[], Vector &result, const idx_t count, JSONTransformOptions &options) {
	vector<idx_t> objects;
	for (idx_t i = 0; i < count; i++) {
		if (vals[i] && unsafe_yyjson_is_obj(vals[i])) {
			objects.push_back(i);
		}
	}
	if (objects.empty()) {
		// Nothing but WKT here, so use the regular string cast
		return TransformFromString(vals, result, count, options);
	}

	// The objects are converted below, the rest is cast from a string
	vector<yyjson_val *> others(vals, vals + count);
	for (auto i : objects) {
		others[i] = nullptr;
	}
	bool success = TransformFromString(others.data(), result, count, options);
	result.Flatten();
	auto mask = result.GetValidityMutable();
	for (auto i : objects) {
		string wkb;
		string error;
		if (geojson_parser && geojson_parser(vals[i], wkb, error)) {
			result.AssignString(i, wkb);
			mask.SetValid(i);
			continue;
		}
		if (error.empty()) {
			error = Format("Unable to cast '%s' to GEOMETRY", JSONCommon::ValToString(vals[i], 50));
		}
		mask.SetInvalid(i);
		if (success && options.strict_cast) {
			options.error_message = error;
			options.object_index = i;
			success = false;
		}
	}
	return success;
}

//===--------------------------------------------------------------------===//
// Dates with a detected format
//===--------------------------------------------------------------------===//
static bool TryOneFormat(const JSONDateFormats &formats, idx_t format_idx, LogicalTypeId type_id, string_view input,
                         Vector &result, idx_t i, string &error_message) {
	int64_t value;
	if (!formats.TryParse(type_id, format_idx, input, value, error_message)) {
		return false;
	}
	if (type_id == LogicalTypeId::DATE) {
		result.GetDataMutable<int32_t>()[i] = static_cast<int32_t>(value);
	} else {
		result.GetDataMutable<int64_t>()[i] = value;
	}
	return true;
}

static bool TransformFromStringWithFormat(yyjson_val *vals[], Vector &result, const idx_t count,
                                          JSONTransformOptions &options) {
	Sized(result, count);
	TempVector string_vector(options.factory, MakeType(options, LogicalTypeId::VARCHAR), count);
	auto result_type_id = result.GetLogicalType().GetTypeId();
	bool success = true;
	if (!JSONTransform::GetStringVector(vals, count, result.GetLogicalType(), string_vector.Get(), options)) {
		success = false;
	}

	D_ASSERT(result_type_id == LogicalTypeId::DATE || result_type_id == LogicalTypeId::TIMESTAMP);
	const auto &formats = *options.date_formats;
	const auto format_count = formats.FormatCount(result_type_id);
	ValueView<varchar_t> sources(string_vector.Get());
	LazyValidity target_validity(result, count);

	// Probe first non-null value to find the most-specific matching format for this vector
	idx_t matched_idx = static_cast<idx_t>(-1);
	for (idx_t i = 0; i < count && matched_idx == static_cast<idx_t>(-1); i++) {
		if (!sources.IsValid(i)) {
			continue;
		}
		string error_message;
		auto source = sources.Get(i);
		for (idx_t j = format_count; j > 0; j--) {
			if (TryOneFormat(formats, j - 1, result_type_id, source.view(), result, i, error_message)) {
				matched_idx = j - 1;
				break;
			}
		}
	}

	const idx_t parse_limit = (matched_idx != static_cast<idx_t>(-1)) ? matched_idx + 1 : format_count;
	for (idx_t i = 0; i < count; i++) {
		if (!sources.IsValid(i)) {
			target_validity.SetInvalid(i);
			continue;
		}
		bool parsed = false;
		string error_message;
		auto source = sources.Get(i);
		// Reverse iter formats, beginning at matched_idx until one parses
		for (idx_t j = parse_limit; j > 0 && !parsed; j--) {
			parsed = TryOneFormat(formats, j - 1, result_type_id, source.view(), result, i, error_message);
		}
		if (!parsed) {
			target_validity.SetInvalid(i);
			if (success && options.strict_cast) {
				options.object_index = i;
				success = false;
			}
		}
	}
	return success;
}

//===--------------------------------------------------------------------===//
// Objects
//===--------------------------------------------------------------------===//
//! Assigns the values of each object's keys to the columns; templated on the key map so that the exact match keeps
//! its own hash and equality
template <class KEY_MAP>
static bool AssignObjectValues(yyjson_val *objects[], yyjson_alc *alc, const idx_t count, const vector<string> &names,
                               vector<yyjson_val **> &nested_vals, JSONTransformOptions &options,
                               bool error_unknown_key) {
	const idx_t column_count = names.size();
	// Build hash map from key to column index so we don't have to linearly search using the key
	KEY_MAP key_map;
	for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
		key_map.insert({{names[col_idx].c_str(), names[col_idx].length()}, col_idx});
	}

	idx_t found_key_count;
	auto found_keys = JSONCommon::AllocateArray<bool>(alc, column_count);

	bool success = true;

	size_t idx, max;
	yyjson_val *key, *val;
	for (idx_t i = 0; i < count; i++) {
		const auto &obj = objects[i];
		if (IsNullVal(obj)) {
			// Set nested val to null so the recursion doesn't break
			for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
				nested_vals[col_idx][i] = nullptr;
			}
			continue;
		}

		if (!unsafe_yyjson_is_obj(obj)) {
			// Set nested val to null so the recursion doesn't break
			for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
				nested_vals[col_idx][i] = nullptr;
			}
			if (success && options.strict_cast && obj) {
				options.error_message = Format("Expected OBJECT, but got %s: %s", JSONCommon::ValTypeToString(obj),
				                               JSONCommon::ValToString(obj, 50));
				options.object_index = i;
				success = false;
			}
			continue;
		}

		found_key_count = 0;
		std::memset(found_keys, false, column_count);
		yyjson_obj_foreach(objects[i], idx, max, key, val) {
			auto key_ptr = unsafe_yyjson_get_str(key);
			auto key_len = unsafe_yyjson_get_len(key);
			auto it = key_map.find({key_ptr, key_len});
			if (it != key_map.end()) {
				const auto &col_idx = it->second;
				if (found_keys[col_idx]) {
					if (success && options.error_duplicate_key) {
						options.error_message =
						    Format("Object %s has duplicate key \"%s\"", JSONCommon::ValToString(objects[i], 50),
						           string(key_ptr, key_len));
						options.object_index = i;
						success = false;
					}
				} else {
					nested_vals[col_idx][i] = val;
					found_keys[col_idx] = true;
					found_key_count++;
				}
			} else if (success && error_unknown_key && options.error_unknown_key) {
				options.error_message = Format("Object %s has unknown key \"%s\"",
				                               JSONCommon::ValToString(objects[i], 50), string(key_ptr, key_len));
				options.object_index = i;
				success = false;
			}
		}

		if (found_key_count != column_count) {
			// If 'error_missing_key, we throw an error if one of the keys was not found.
			// If not, we set the nested val to null so the recursion doesn't break
			for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
				if (found_keys[col_idx]) {
					continue;
				}
				nested_vals[col_idx][i] = nullptr;

				if (success && options.error_missing_key) {
					options.error_message = Format("Object %s does not have key \"%s\"",
					                               JSONCommon::ValToString(objects[i], 50), names[col_idx]);
					options.object_index = i;
					success = false;
				}
			}
		}
	}

	return success;
}

bool JSONTransform::TransformObject(yyjson_val *objects[], yyjson_alc *alc, const idx_t count,
                                    const vector<string> &names, vector<Vector> &result_vectors,
                                    JSONTransformOptions &options, const vector<JSONColumnProjection> *column_indices,
                                    bool error_unknown_key) {
	if (column_indices && column_indices->empty()) {
		column_indices = nullptr;
	}
	D_ASSERT(alc);
	D_ASSERT(names.size() == result_vectors.size());
	D_ASSERT(!column_indices || column_indices->size() == names.size());
	const idx_t column_count = names.size();

	vector<yyjson_val **> nested_vals;
	nested_vals.reserve(column_count);
	for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
		nested_vals.push_back(JSONCommon::AllocateArray<yyjson_val *>(alc, count));
	}

	bool success = options.key_match == JSONKeyMatch::CASE_INSENSITIVE
	                   ? AssignObjectValues<json_key_ci_map_t<idx_t>>(objects, alc, count, names, nested_vals, options,
	                                                                  error_unknown_key)
	                   : AssignObjectValues<json_key_map_t<idx_t>>(objects, alc, count, names, nested_vals, options,
	                                                               error_unknown_key);

	for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
		auto child_column_index = column_indices ? &(*column_indices)[col_idx] : nullptr;
		if (!JSONTransform::Transform(nested_vals[col_idx], alc, result_vectors[col_idx], count, options,
		                              child_column_index)) {
			success = false;
		}
	}

	if (!options.delay_error && !success) {
		throw InvalidInputException(options.error_message);
	}

	return success;
}

static bool TransformObjectInternal(yyjson_val *objects[], yyjson_alc *alc, Vector &result, const idx_t count,
                                    JSONTransformOptions &options, const JSONColumnProjection *column_index) {
	if (column_index && column_index->children.empty()) {
		column_index = nullptr;
	}

	// Set validity first
	LazyValidity result_validity(Sized(result, count), count);
	for (idx_t i = 0; i < count; i++) {
		if (IsNullVal(objects[i])) {
			result_validity.SetInvalid(i);
		}
	}

	// Get child vectors and names
	auto result_type = result.GetLogicalType();
	const auto total_children = result_type.GetStructChildCount();
	vector<string> child_names;
	vector<Vector> child_vectors;

	const auto child_count = column_index ? column_index->children.size() : total_children;
	child_names.reserve(child_count);
	child_vectors.reserve(child_count);

	vector<bool> projected(total_children, false);
	for (idx_t child_i = 0; child_i < child_count; child_i++) {
		const auto actual_i = column_index ? column_index->children[child_i].index : child_i;
		projected[actual_i] = true;
		child_names.emplace_back(result_type.GetStructChildName(actual_i));
		child_vectors.push_back(result.GetChild(actual_i));
	}

	for (idx_t child_i = 0; child_i < total_children; child_i++) {
		if (!projected[child_i]) {
			auto child = result.GetChild(child_i);
			child.SetSize(count);
			child.GetValidityMutable().SetAllInvalid(count);
		}
	}

	auto child_indices = column_index ? &column_index->children : nullptr;
	const auto error_unknown_key = child_count == total_children; // Nothing projected out, error if unknown
	return JSONTransform::TransformObject(objects, alc, count, child_names, child_vectors, options, child_indices,
	                                      error_unknown_key);
}

//===--------------------------------------------------------------------===//
// Arrays
//===--------------------------------------------------------------------===//
static bool TransformArrayToList(yyjson_val *arrays[], yyjson_alc *alc, Vector &result, const idx_t count,
                                 JSONTransformOptions &options) {
	bool success = true;

	// Initialize list vector
	Sized(result, count);
	auto list_entries = result.GetDataMutable<list_entry_t>();
	LazyValidity list_validity(result, count);
	vector<bool> row_valid(count, false);
	idx_t offset = 0;
	for (idx_t i = 0; i < count; i++) {
		const auto &arr = arrays[i];
		list_entries[i] = list_entry_t {offset, 0};
		if (IsNullVal(arr)) {
			list_validity.SetInvalid(i);
			continue;
		}

		if (!unsafe_yyjson_is_arr(arr)) {
			list_validity.SetInvalid(i);
			if (success && options.strict_cast) {
				options.error_message = Format("Expected ARRAY, but got %s: %s", JSONCommon::ValTypeToString(arrays[i]),
				                               JSONCommon::ValToString(arrays[i], 50));
				options.object_index = i;
				success = false;
			}
			continue;
		}

		row_valid[i] = true;
		auto &entry = list_entries[i];
		entry.length = unsafe_yyjson_get_len(arr);
		offset += entry.length;
	}
	auto child = result.GetChild(0);
	child.SetSize(offset);

	// Initialize array for the nested values
	auto nested_vals = JSONCommon::AllocateArray<yyjson_val *>(alc, offset);

	// Get array values
	size_t idx, max;
	yyjson_val *val;
	idx_t list_i = 0;
	for (idx_t i = 0; i < count; i++) {
		if (!row_valid[i]) {
			continue; // We already marked this as invalid
		}
		yyjson_arr_foreach(arrays[i], idx, max, val) {
			nested_vals[list_i] = val;
			list_i++;
		}
	}
	D_ASSERT(list_i == offset);

	if (!success) {
		// Set object index in case of error in nested list so we can get accurate line number information
		for (idx_t i = 0; i < count; i++) {
			if (!row_valid[i]) {
				continue;
			}
			auto &entry = list_entries[i];
			if (options.object_index >= entry.offset && options.object_index < entry.offset + entry.length) {
				options.object_index = i;
			}
		}
	}

	// Transform array values
	if (!JSONTransform::Transform(nested_vals, alc, child, offset, options, nullptr)) {
		success = false;
	}

	if (!options.delay_error && !success) {
		throw InvalidInputException(options.error_message);
	}

	return success;
}

static bool TransformArrayToTuple(yyjson_val *arrays[], yyjson_alc *alc, Vector &result, const idx_t count,
                                  JSONTransformOptions &options) {
	// A TUPLE is serialized as a JSON array - read each array element positionally into the matching struct child
	bool success = true;
	LazyValidity result_validity(Sized(result, count), count);
	const idx_t child_count = result.GetChildCount();

	// nested_vals[child_i * count + i] holds the JSON value for child child_i of row i
	auto nested_vals = JSONCommon::AllocateArray<yyjson_val *>(alc, child_count * count);
	for (idx_t i = 0; i < count; i++) {
		const auto &arr = arrays[i];
		bool valid = arr && !unsafe_yyjson_is_null(arr) && unsafe_yyjson_is_arr(arr);
		if (!valid) {
			result_validity.SetInvalid(i);
			if (success && options.strict_cast && arr && !unsafe_yyjson_is_null(arr)) {
				options.error_message = Format("Expected ARRAY, but got %s: %s", JSONCommon::ValTypeToString(arr),
				                               JSONCommon::ValToString(arr, 50));
				options.object_index = i;
				success = false;
			}
			for (idx_t child_i = 0; child_i < child_count; child_i++) {
				nested_vals[child_i * count + i] = nullptr;
			}
			continue;
		}
		// gather up to child_count elements - missing elements become NULL, extra elements are ignored
		size_t idx, max;
		yyjson_val *val;
		idx_t child_i = 0;
		yyjson_arr_foreach(arr, idx, max, val) {
			if (child_i < child_count) {
				nested_vals[child_i * count + i] = val;
			}
			child_i++;
		}
		for (; child_i < child_count; child_i++) {
			nested_vals[child_i * count + i] = nullptr;
		}
	}

	for (idx_t child_i = 0; child_i < child_count; child_i++) {
		auto child = result.GetChild(child_i);
		if (!JSONTransform::Transform(nested_vals + child_i * count, alc, child, count, options, nullptr)) {
			success = false;
		}
	}

	if (!options.delay_error && !success) {
		throw InvalidInputException(options.error_message);
	}
	return success;
}

static bool TransformArrayToArray(yyjson_val *arrays[], yyjson_alc *alc, Vector &result, const idx_t count,
                                  JSONTransformOptions &options) {
	bool success = true;

	// Initialize array vector
	LazyValidity result_validity(Sized(result, count), count);
	vector<bool> row_valid(count, false);
	auto array_size = result.GetLogicalType().GetArraySize();
	auto child_count = count * array_size;

	for (idx_t i = 0; i < count; i++) {
		const auto &arr = arrays[i];
		if (IsNullVal(arr)) {
			result_validity.SetInvalid(i);
			continue;
		}

		if (!unsafe_yyjson_is_arr(arr)) {
			result_validity.SetInvalid(i);
			if (success && options.strict_cast) {
				options.error_message = Format("Expected ARRAY, but got %s: %s", JSONCommon::ValTypeToString(arrays[i]),
				                               JSONCommon::ValToString(arrays[i], 50));
				options.object_index = i;
				success = false;
			}
			continue;
		}

		auto json_arr_size = unsafe_yyjson_get_len(arr);
		if (json_arr_size != array_size) {
			result_validity.SetInvalid(i);
			if (success && options.strict_cast) {
				options.error_message =
				    Format("Expected array of size %u, but got '%s' with size %u", static_cast<unsigned>(array_size),
				           JSONCommon::ValToString(arrays[i], 50), static_cast<unsigned>(json_arr_size));
				options.object_index = i;
				success = false;
			}
			continue;
		}
		row_valid[i] = true;
	}

	// Initialize array for the nested values
	auto nested_vals = JSONCommon::AllocateArray<yyjson_val *>(alc, child_count);

	// Get array values
	size_t idx, max;
	yyjson_val *val;
	idx_t nested_elem_idx = 0;
	for (idx_t i = 0; i < count; i++) {
		if (!row_valid[i]) {
			// We already marked this as invalid, but we still need to increment nested_elem_idx
			// and set the nullptrs (otherwise indexing will break after compaction)
			for (idx_t j = 0; j < array_size; j++) {
				nested_vals[nested_elem_idx] = nullptr;
				nested_elem_idx++;
			}
		} else {
			yyjson_arr_foreach(arrays[i], idx, max, val) {
				nested_vals[nested_elem_idx] = val;
				nested_elem_idx++;
			}
		}
	}

	if (!success) {
		// Set object index in case of error in nested array so we can get accurate line number information
		for (idx_t i = 0; i < count; i++) {
			if (!row_valid[i]) {
				continue;
			}
			auto offset = i * array_size;
			if (options.object_index >= offset && options.object_index < offset + array_size) {
				options.object_index = i;
			}
		}
	}

	// Transform array values
	auto child = result.GetChild(0);
	if (!JSONTransform::Transform(nested_vals, alc, child, child_count, options, nullptr)) {
		success = false;
	}

	if (!options.delay_error && !success) {
		throw InvalidInputException(options.error_message);
	}

	return success;
}

//===--------------------------------------------------------------------===//
// Maps
//===--------------------------------------------------------------------===//
static bool TransformObjectToMap(yyjson_val *objects[], yyjson_alc *alc, Vector &result, const idx_t count,
                                 JSONTransformOptions &options) {
	// Pre-allocate list vector
	idx_t list_size = 0;
	for (idx_t i = 0; i < count; i++) {
		const auto &obj = objects[i];
		if (!obj || !unsafe_yyjson_is_obj(obj)) {
			continue;
		}
		list_size += unsafe_yyjson_get_len(obj);
	}
	Sized(result, count);
	auto entries = result.GetChild(0);
	entries.SetSize(list_size);

	auto list_entries = result.GetDataMutable<list_entry_t>();
	LazyValidity list_validity(result, count);
	vector<bool> row_valid(count, false);

	auto keys = JSONCommon::AllocateArray<yyjson_val *>(alc, list_size);
	auto vals = JSONCommon::AllocateArray<yyjson_val *>(alc, list_size);

	bool success = true;
	idx_t list_offset = 0;

	size_t idx, max;
	yyjson_val *key, *val;
	for (idx_t i = 0; i < count; i++) {
		const auto &obj = objects[i];
		list_entries[i] = list_entry_t {list_offset, 0};
		if (IsNullVal(obj)) {
			list_validity.SetInvalid(i);
			continue;
		}

		if (!unsafe_yyjson_is_obj(obj)) {
			list_validity.SetInvalid(i);
			if (success && options.strict_cast && !unsafe_yyjson_is_obj(obj)) {
				options.error_message = Format("Expected OBJECT, but got %s: %s", JSONCommon::ValTypeToString(obj),
				                               JSONCommon::ValToString(obj, 50));
				options.object_index = i;
				success = false;
			}
			continue;
		}

		row_valid[i] = true;
		auto &list_entry = list_entries[i];
		list_entry.offset = list_offset;
		list_entry.length = unsafe_yyjson_get_len(obj);

		yyjson_obj_foreach(obj, idx, max, key, val) {
			keys[list_offset] = key;
			vals[list_offset] = val;
			list_offset++;
		}
	}
	D_ASSERT(list_offset == list_size);

	// Transform keys
	auto map_keys = entries.GetChild(0);
	if (!JSONTransform::Transform(keys, alc, map_keys, list_size, options, nullptr)) {
		success = false;
	}

	// Transform values
	auto map_values = entries.GetChild(1);
	if (!JSONTransform::Transform(vals, alc, map_values, list_size, options, nullptr)) {
		success = false;
	}

	// objects can have duplicate keys, and keys that fail to transform in non-strict mode are NULL
	const bool varchar_keys = map_keys.GetLogicalType().GetTypeId() == LogicalTypeId::VARCHAR;
	std::optional<ValueView<varchar_t>> string_keys;
	if (varchar_keys) {
		string_keys.emplace(map_keys);
	}
	auto key_view = map_keys.GetView();
	unordered_set<string> unique_keys;
	for (idx_t i = 0; i < count; i++) {
		if (!row_valid[i]) {
			continue;
		}
		const auto &list_entry = list_entries[i];
		const char *invalid_reason = nullptr;
		unique_keys.clear();
		for (idx_t key_idx = list_entry.offset; key_idx < list_entry.offset + list_entry.length; key_idx++) {
			if (!key_view.IsValid(key_idx)) {
				invalid_reason = "Map keys can not be NULL";
				break;
			}
			// The values of the keys are compared by their text
			bool inserted;
			if (varchar_keys) {
				inserted = unique_keys.insert(string(string_keys->Get(key_idx).view())).second;
			} else {
				inserted = unique_keys.insert(map_keys.GetValue(key_idx).ToText()).second;
			}
			if (!inserted) {
				invalid_reason = "Map keys must be unique";
				break;
			}
		}
		if (!invalid_reason) {
			continue;
		}
		list_validity.SetInvalid(i);
		if (success && options.strict_cast) {
			options.error_message = invalid_reason;
			options.object_index = i;
			success = false;
		}
	}

	if (!options.delay_error && !success) {
		throw InvalidInputException(options.error_message);
	}

	return success;
}

//===--------------------------------------------------------------------===//
// Unions
//===--------------------------------------------------------------------===//
static bool TransformValueIntoUnion(yyjson_val **vals, yyjson_alc *alc, Vector &result, const idx_t count,
                                    JSONTransformOptions &options) {
	auto type = result.GetLogicalType();
	const idx_t member_count = type.GetUnionMemberCount();
	vector<string> names;
	for (idx_t i = 0; i < member_count; i++) {
		names.emplace_back(type.GetUnionMemberName(i));
	}

	bool success = true;

	LazyValidity validity(Sized(result, count), count);
	auto tag_vector = result.GetChild(0);
	auto tags = Sized(tag_vector, count).GetDataMutable<uint8_t>();
	LazyValidity tag_validity(tag_vector, count);

	vector<yyjson_val **> member_vals;
	for (idx_t k = 0; k < member_count; k++) {
		auto member = JSONCommon::AllocateArray<yyjson_val *>(alc, count);
		for (idx_t i = 0; i < count; i++) {
			member[i] = nullptr;
		}
		member_vals.push_back(member);
	}

	auto set_invalid = [&](idx_t i) {
		validity.SetInvalid(i);
		tag_validity.SetInvalid(i);
		tags[i] = 0;
	};
	auto set_error = [&](idx_t i, const string &message) {
		set_invalid(i);
		if (success && options.strict_cast) {
			options.error_message = message;
			options.object_index = i;
			success = false;
		}
	};

	for (idx_t i = 0; i < count; i++) {
		const auto &obj = vals[i];

		if (IsNullVal(obj)) {
			set_invalid(i);
			continue;
		}

		if (!unsafe_yyjson_is_obj(obj)) {
			set_error(i, Format("Expected an object representing a union, got %s", yyjson_get_type_desc(obj)));
			continue;
		}

		auto len = unsafe_yyjson_get_len(obj);
		if (len > 1) {
			set_error(i, "Found object containing more than one key, instead of union");
			continue;
		} else if (len == 0) {
			set_error(i, "Found empty object, instead of union");
			continue;
		}

		auto key = unsafe_yyjson_get_first(obj);
		auto val = yyjson_obj_iter_get_val(key);

		auto tag = std::find(names.begin(), names.end(), string(GetString(key)));
		if (tag == names.end()) {
			set_error(i,
			          Format("Found object containing unknown key, instead of union: %s", unsafe_yyjson_get_str(key)));
			continue;
		}

		idx_t actual_tag = static_cast<idx_t>(tag - names.begin());
		tags[i] = static_cast<uint8_t>(actual_tag);
		member_vals[actual_tag][i] = val;
	}

	for (idx_t k = 0; k < member_count; k++) {
		auto member = result.GetChild(1 + k);
		if (!JSONTransform::Transform(member_vals[k], alc, member, count, options, nullptr)) {
			success = false;
		}
	}

	return success;
}

//===--------------------------------------------------------------------===//
// Transform
//===--------------------------------------------------------------------===//
bool JSONTransform::Transform(yyjson_val *vals[], yyjson_alc *alc, Vector &result, const idx_t count,
                              JSONTransformOptions &options, const JSONColumnProjection *column_index) {
	auto result_type = result.GetLogicalType();
	const auto id = result_type.GetTypeId();
	if ((id == LogicalTypeId::TIMESTAMP || id == LogicalTypeId::DATE) && options.date_formats &&
	    options.date_formats->HasFormats(id)) {
		// Auto-detected date/timestamp format during sampling
		return TransformFromStringWithFormat(vals, result, count, options);
	}

	if (IsJSONType(result_type)) {
		return TransformToJSON(vals, alc, result, count, options);
	}

	switch (id) {
	case LogicalTypeId::SQLNULL:
		Sized(result, count);
		if (count > 0) {
			result.GetValidityMutable().SetAllInvalid(count);
		}
		return true;
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		return TransformNumeric(vals, result, count, options, result_type, false);
	case LogicalTypeId::DECIMAL:
		return TransformNumeric(vals, result, count, options, result_type, true);
	case LogicalTypeId::ENUM:
	case LogicalTypeId::DATE:
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::UUID:
		return TransformFromString(vals, result, count, options);
	case LogicalTypeId::GEOMETRY:
		return TransformGeometry(vals, result, count, options);
	case LogicalTypeId::VARCHAR:
		return TransformToString(vals, alc, result, count, options);
	case LogicalTypeId::BLOB:
		return TransformToBlob(vals, alc, result, count, options);
	case LogicalTypeId::STRUCT:
		return TransformObjectInternal(vals, alc, result, count, options, column_index);
	case LogicalTypeId::TUPLE:
		return TransformArrayToTuple(vals, alc, result, count, options);
	case LogicalTypeId::LIST:
		return TransformArrayToList(vals, alc, result, count, options);
	case LogicalTypeId::MAP:
		return TransformObjectToMap(vals, alc, result, count, options);
	case LogicalTypeId::UNION:
		return TransformValueIntoUnion(vals, alc, result, count, options);
	case LogicalTypeId::ARRAY:
		return TransformArrayToArray(vals, alc, result, count, options);
	default:
		throw NotImplementedException("Cannot read a value of type %s from a json file", result_type.ToText());
	}
}

} // namespace json
} // namespace duckdb
