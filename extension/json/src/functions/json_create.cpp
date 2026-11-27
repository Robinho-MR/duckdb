// json_object, json_array, to_json (json_quote), array_to_json,
// row_to_json and the casts from any type to JSON. The copy-specific entry points (COPY ... TO ... (FORMAT JSON))
// are in json_create.hpp.

#include "json_create.hpp"

namespace duckdb {
namespace json {

using cxx::DataChunk;

//===--------------------------------------------------------------------===//
// Options
//===--------------------------------------------------------------------===//
static JSONGeometryFormat ParseGeometryFormat(const Value &value) {
	if (value.IsNull()) {
		return JSONGeometryFormat::WKT;
	}
	auto text = value.Get<varchar_t>();
	string format(text.data(), text.size());
	for (auto &c : format) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c + ('a' - 'A'));
		}
	}
	if (format == "wkt") {
		return JSONGeometryFormat::WKT;
	}
	if (format == "geojson") {
		return JSONGeometryFormat::GEOJSON;
	}
	throw InvalidInputException("Invalid value \"%s\" for json_geometry_format, expected 'wkt' or 'geojson'",
	                            string(text.data(), text.size()));
}

static yyjson_mut_val *(*geometry_to_geojson_hook)(yyjson_mut_doc *, blob_t) = nullptr;

void SetGeometryToGeoJSON(yyjson_mut_val *(*converter)(yyjson_mut_doc *, blob_t)) {
	geometry_to_geojson_hook = converter;
}

JSONCreateOptions ClientJSONCreateOptions(Context &context) {
	JSONCreateOptions result;
	if (geometry_to_geojson_hook) {
		result.geometry_to_geojson = geometry_to_geojson_hook;
	}
	auto option = context.GetConfig().TryGetOption("json_geometry_format");
	if (option) {
		result.geometry_format = ParseGeometryFormat(option->value);
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Creating values
//===--------------------------------------------------------------------===//
namespace {

//! A vector that holds the result of a cast; the chunk owns the vector
struct TempVector {
	TempVector(DataChunk chunk_p) : chunk(std::move(chunk_p)), vec(chunk.GetVector(0)) {
	}
	DataChunk chunk;
	Vector vec;
};

struct CreateState {
	CreateState(Context &context_p, yyjson_mut_doc *doc_p, const JSONCreateOptions &options_p)
	    : context(context_p), factory(context_p.GetFactory()), doc(doc_p), options(options_p) {
	}

	//! Casts all rows of `source` to `target`
	TempVector Cast(Vector &source, const LogicalType &target) {
		vector<LogicalType> types;
		types.push_back(target.Copy());
		TempVector result(factory.CreateDataChunk(types));
		source.Cast(factory, result.vec);
		return result;
	}
	TempVector CastToVarchar(Vector &source) {
		return Cast(source, factory.CreateType(LogicalTypeId::VARCHAR));
	}

	Context &context;
	Factory &factory;
	yyjson_mut_doc *doc;
	const JSONCreateOptions &options;
};

} // namespace

static void CreateValues(CreateState &state, yyjson_mut_val *vals[], Vector &value_v, idx_t count);

static string Int128ToString(const cxx::int128_t &value) {
	const bool negative = value.upper < 0;
	unsigned __int128 v = (static_cast<unsigned __int128>(static_cast<uint64_t>(value.upper)) << 64) | value.lower;
	if (negative) {
		v = ~v + 1;
	}
	char buffer[48];
	auto pos = sizeof(buffer);
	do {
		buffer[--pos] = static_cast<char>('0' + static_cast<int>(v % 10));
		v /= 10;
	} while (v != 0);
	if (negative) {
		buffer[--pos] = '-';
	}
	return string(buffer + pos, sizeof(buffer) - pos);
}

static string UInt128ToString(const cxx::uint128_t &value) {
	unsigned __int128 v = (static_cast<unsigned __int128>(value.upper) << 64) | value.lower;
	char buffer[48];
	auto pos = sizeof(buffer);
	do {
		buffer[--pos] = static_cast<char>('0' + static_cast<int>(v % 10));
		v /= 10;
	} while (v != 0);
	return string(buffer + pos, sizeof(buffer) - pos);
}

static void CreateValuesNull(yyjson_mut_doc *doc, yyjson_mut_val *vals[], idx_t count) {
	for (idx_t i = 0; i < count; i++) {
		vals[i] = yyjson_mut_null(doc);
	}
}

//! Numbers and booleans: `create(doc, value)` makes the JSON value
template <class T, class CREATE>
static void CreateScalarValues(yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &value_v, idx_t count,
                               CREATE &&create) {
	ValueView<T> values(value_v);
	for (idx_t i = 0; i < count; i++) {
		if (!values.IsValid(i)) {
			vals[i] = yyjson_mut_null(doc);
		} else {
			vals[i] = create(doc, values.Get(i));
		}
	}
}

//! Strings: the text becomes a JSON string
static void CreateStringValues(yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &value_v, idx_t count) {
	ValueView<varchar_t> values(value_v);
	for (idx_t i = 0; i < count; i++) {
		if (!values.IsValid(i)) {
			vals[i] = yyjson_mut_null(doc);
		} else {
			auto value = values.Get(i);
			vals[i] = yyjson_mut_strncpy(doc, value.data(), value.size());
		}
	}
}

//! Strings that already are JSON: parsed and copied into the document
static void CreateJSONStringValues(yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &value_v, idx_t count) {
	ValueView<varchar_t> values(value_v);
	for (idx_t i = 0; i < count; i++) {
		if (!values.IsValid(i)) {
			vals[i] = yyjson_mut_null(doc);
		} else {
			auto value_doc = JSONCommon::ReadDocument(values.Get(i), JSONCommon::READ_FLAG, &doc->alc);
			vals[i] = yyjson_val_mut_copy(doc, value_doc->root);
		}
	}
}

//! Strings that are inserted as they are (numbers that do not fit a double)
static void CreateRawValues(yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &value_v, idx_t count) {
	ValueView<varchar_t> values(value_v);
	for (idx_t i = 0; i < count; i++) {
		if (!values.IsValid(i)) {
			vals[i] = yyjson_mut_null(doc);
		} else {
			auto value = values.Get(i);
			vals[i] = yyjson_mut_rawncpy(doc, value.data(), value.size());
		}
	}
}

//! DECIMAL(w, s) with w == s has no digits before the decimal point, so its VARCHAR form has no leading zero
//! (e.g., ".5" or "-.5"). That is not a valid JSON number, so we add the zero before inserting it as a raw value.
static void CreateRawDecimalValues(yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &value_v, idx_t count) {
	ValueView<varchar_t> values(value_v);
	string buffer;
	for (idx_t i = 0; i < count; i++) {
		if (!values.IsValid(i)) {
			vals[i] = yyjson_mut_null(doc);
			continue;
		}
		auto value = values.Get(i);
		const auto data = value.data();
		const auto size = value.size();
		const idx_t point_pos = (size > 0 && data[0] == '-') ? 1 : 0;
		if (point_pos < size && data[point_pos] == '.') {
			buffer.assign(data, point_pos);
			buffer += '0';
			buffer.append(data + point_pos, size - point_pos);
			vals[i] = yyjson_mut_rawncpy(doc, buffer.c_str(), buffer.size());
		} else {
			vals[i] = yyjson_mut_rawncpy(doc, data, size);
		}
	}
}

static void CreateValuesFromDefaultCast(CreateState &state, yyjson_mut_val *vals[], Vector &value_v, idx_t count) {
	auto strings = state.CastToVarchar(value_v);
	CreateStringValues(state.doc, vals, strings.vec, count);
}

static idx_t ListChildCount(Vector &value_v, idx_t count) {
	ValueView<list_entry_t> lists(value_v);
	idx_t child_count = 0;
	for (idx_t i = 0; i < count; i++) {
		if (!lists.IsValid(i)) {
			continue;
		}
		auto entry = lists.Get(i);
		child_count = std::max<idx_t>(child_count, entry.offset + entry.length);
	}
	return child_count;
}

static void FlattenNested(Vector &value_v) {
	if (value_v.GetVectorType() != cxx::VectorType::FLAT) {
		value_v.Flatten();
	}
}

static void NullInvalidRows(yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &value_v, idx_t count) {
	ValueView<uint8_t> validity(value_v);
	if (validity.AllValid()) {
		return;
	}
	for (idx_t i = 0; i < count; i++) {
		if (!validity.IsValid(i)) {
			vals[i] = yyjson_mut_null(doc);
		}
	}
}

static void CreateValuesStruct(CreateState &state, yyjson_mut_val *vals[], Vector &value_v, const LogicalType &type,
                               idx_t count) {
	auto doc = state.doc;
	FlattenNested(value_v);
	for (idx_t i = 0; i < count; i++) {
		vals[i] = yyjson_mut_obj(doc);
	}
	auto nested_vals = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, count);
	const auto child_count = type.GetStructChildCount();
	for (idx_t entry_i = 0; entry_i < child_count; entry_i++) {
		const auto name = type.GetStructChildName(entry_i);
		auto child = value_v.GetChild(entry_i);
		CreateValues(state, nested_vals, child, count);
		for (idx_t i = 0; i < count; i++) {
			auto key = yyjson_mut_strncpy(doc, name.data(), name.size());
			yyjson_mut_obj_add(vals[i], key, nested_vals[i]);
		}
	}
	// The whole struct can be NULL
	NullInvalidRows(doc, vals, value_v, count);
}

static void CreateValuesTuple(CreateState &state, yyjson_mut_val *vals[], Vector &value_v, const LogicalType &type,
                              idx_t count) {
	auto doc = state.doc;
	FlattenNested(value_v);
	// A TUPLE becomes a JSON array (matching how Python serializes tuples)
	for (idx_t i = 0; i < count; i++) {
		vals[i] = yyjson_mut_arr(doc);
	}
	auto nested_vals = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, count);
	const auto child_count = type.GetStructChildCount();
	for (idx_t entry_i = 0; entry_i < child_count; entry_i++) {
		auto child = value_v.GetChild(entry_i);
		CreateValues(state, nested_vals, child, count);
		for (idx_t i = 0; i < count; i++) {
			yyjson_mut_arr_append(vals[i], nested_vals[i]);
		}
	}
	NullInvalidRows(doc, vals, value_v, count);
}

//! The date and timestamp types: formatted with the copy format if there is one, the default text otherwise
static void CreateValuesFormattedStrings(CreateState &state, yyjson_mut_val *vals[], idx_t count,
                                         const vector<std::optional<string>> &strings) {
	auto doc = state.doc;
	for (idx_t i = 0; i < count; i++) {
		if (!strings[i].has_value()) {
			vals[i] = yyjson_mut_null(doc);
		} else {
			vals[i] = yyjson_mut_strncpy(doc, strings[i]->data(), strings[i]->size());
		}
	}
}

static void CreateValuesDate(CreateState &state, yyjson_mut_val *vals[], Vector &value_v, idx_t count) {
	auto format = state.options.date_format;
	if (!format) {
		CreateValuesFromDefaultCast(state, vals, value_v, count);
		return;
	}
	ValueView<cxx::date_t> values(value_v);
	vector<std::optional<string>> strings(count);
	for (idx_t i = 0; i < count; i++) {
		if (!values.IsValid(i)) {
			continue;
		}
		const auto days = values.Get(i).days;
		if (days == std::numeric_limits<int32_t>::max()) {
			strings[i] = "infinity";
		} else if (days == -std::numeric_limits<int32_t>::max()) {
			strings[i] = "-infinity";
		} else {
			strings[i] = format->FormatDate(days);
		}
	}
	CreateValuesFormattedStrings(state, vals, count, strings);
}

//! Formats timestamps; `to_micros` / `to_nanos` read the element of the vector (one of them is set)
template <class T, class GET>
static void FormatTimestamps(CreateState &state, yyjson_mut_val *vals[], Vector &value_v, idx_t count,
                             const JSONTimeFormat &format, bool nanos, GET &&get_value) {
	ValueView<T> values(value_v);
	vector<std::optional<string>> strings(count);
	for (idx_t i = 0; i < count; i++) {
		if (!values.IsValid(i)) {
			continue;
		}
		const int64_t value = get_value(values.Get(i));
		if (value == std::numeric_limits<int64_t>::max()) {
			strings[i] = "infinity";
		} else if (value == -std::numeric_limits<int64_t>::max()) {
			strings[i] = "-infinity";
		} else {
			strings[i] = nanos ? format.FormatTimestampNanos(value) : format.FormatTimestampMicros(value);
		}
	}
	CreateValuesFormattedStrings(state, vals, count, strings);
}

static void CreateValuesTimestamp(CreateState &state, yyjson_mut_val *vals[], Vector &value_v, idx_t count,
                                  LogicalTypeId type_id) {
	auto format = state.options.timestamp_format;
	if (!format) {
		CreateValuesFromDefaultCast(state, vals, value_v, count);
		return;
	}
	switch (type_id) {
	case LogicalTypeId::TIMESTAMP:
		FormatTimestamps<cxx::timestamp_t>(state, vals, value_v, count, *format, false,
		                                   [](const cxx::timestamp_t &ts) { return ts.micros; });
		break;
	case LogicalTypeId::TIMESTAMP_MS:
		FormatTimestamps<cxx::timestamp_ms_t>(state, vals, value_v, count, *format, false,
		                                      [](const cxx::timestamp_ms_t &ts) {
			                                      return ts.millis == std::numeric_limits<int64_t>::max() ||
			                                                     ts.millis == -std::numeric_limits<int64_t>::max()
			                                                 ? ts.millis
			                                                 : ts.millis * 1000;
		                                      });
		break;
	case LogicalTypeId::TIMESTAMP_SEC:
		FormatTimestamps<cxx::timestamp_s_t>(state, vals, value_v, count, *format, false,
		                                     [](const cxx::timestamp_s_t &ts) {
			                                     return ts.seconds == std::numeric_limits<int64_t>::max() ||
			                                                    ts.seconds == -std::numeric_limits<int64_t>::max()
			                                                ? ts.seconds
			                                                : ts.seconds * 1000000;
		                                     });
		break;
	case LogicalTypeId::TIMESTAMP_NS:
		FormatTimestamps<cxx::timestamp_ns_t>(state, vals, value_v, count, *format, true,
		                                      [](const cxx::timestamp_ns_t &ts) { return ts.nanos; });
		break;
	case LogicalTypeId::TIMESTAMP_TZ: {
		// The format is applied to the local time of the session time zone
		auto local = state.Cast(value_v, state.factory.CreateType(LogicalTypeId::TIMESTAMP));
		FormatTimestamps<cxx::timestamp_t>(state, vals, local.vec, count, *format, false,
		                                   [](const cxx::timestamp_t &ts) { return ts.micros; });
		break;
	}
	case LogicalTypeId::TIMESTAMP_TZ_NS: {
		auto local = state.Cast(value_v, state.factory.CreateType(LogicalTypeId::TIMESTAMP_NS));
		FormatTimestamps<cxx::timestamp_ns_t>(state, vals, local.vec, count, *format, true,
		                                      [](const cxx::timestamp_ns_t &ts) { return ts.nanos; });
		break;
	}
	default:
		throw InternalException("Unexpected timestamp type in JSON creation");
	}
}

static void CreateValuesMapKeys(CreateState &state, yyjson_mut_val *vals[], Vector &value_v, const LogicalType &type,
                                idx_t count) {
	switch (type.GetTypeId()) {
	case LogicalTypeId::DATE:
		CreateValuesDate(state, vals, value_v, count);
		break;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		CreateValuesTimestamp(state, vals, value_v, count, type.GetTypeId());
		break;
	case LogicalTypeId::VARCHAR:
		// Keys are always text, also if the type is JSON
		CreateStringValues(state.doc, vals, value_v, count);
		break;
	default:
		CreateValuesFromDefaultCast(state, vals, value_v, count);
		break;
	}
}

static void CreateValuesMap(CreateState &state, yyjson_mut_val *vals[], Vector &value_v, idx_t count) {
	auto doc = state.doc;
	FlattenNested(value_v);
	const auto child_count = ListChildCount(value_v, count);
	auto entries = value_v.GetChild(0);
	auto key_v = entries.GetChild(0);
	auto val_v = entries.GetChild(1);
	// Create nested keys
	auto nested_keys = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, child_count);
	CreateValuesMapKeys(state, nested_keys, key_v, key_v.GetLogicalType(), child_count);
	// Create nested values
	auto nested_vals = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, child_count);
	CreateValues(state, nested_vals, val_v, child_count);
	// Add the key/value pairs to the values
	ValueView<list_entry_t> lists(value_v);
	for (idx_t i = 0; i < count; i++) {
		if (!lists.IsValid(i)) {
			// The whole map can be NULL
			vals[i] = yyjson_mut_null(doc);
			continue;
		}
		vals[i] = yyjson_mut_obj(doc);
		const auto entry = lists.Get(i);
		for (idx_t child_i = entry.offset; child_i < entry.offset + entry.length; child_i++) {
			if (!unsafe_yyjson_is_null(nested_keys[child_i])) {
				yyjson_mut_obj_add(vals[i], nested_keys[child_i], nested_vals[child_i]);
			}
		}
	}
}

static void CreateValuesUnion(CreateState &state, yyjson_mut_val *vals[], Vector &value_v, const LogicalType &type,
                              idx_t count) {
	auto doc = state.doc;
	FlattenNested(value_v);
	ValueView<uint8_t> validity(value_v);
	for (idx_t i = 0; i < count; i++) {
		// The entry is NULL if the union value is NULL
		vals[i] = validity.IsValid(i) ? yyjson_mut_obj(doc) : yyjson_mut_null(doc);
	}
	auto nested_vals = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, count);
	auto tag_v = value_v.GetChild(0);
	ValueView<uint8_t> tags(tag_v);

	const auto member_count = type.GetUnionMemberCount();
	for (idx_t member_idx = 0; member_idx < member_count; member_idx++) {
		auto member_v = value_v.GetChild(1 + member_idx);
		const auto name = type.GetUnionMemberName(member_idx);
		// This is not optimal since we convert the entire member vector, and skip the rows not matching the tag
		CreateValues(state, nested_vals, member_v, count);
		for (idx_t i = 0; i < count; i++) {
			if (!validity.IsValid(i) || !tags.IsValid(i) || tags.Get(i) != member_idx) {
				continue;
			}
			auto key = yyjson_mut_strncpy(doc, name.data(), name.size());
			yyjson_mut_obj_add(vals[i], key, nested_vals[i]);
		}
	}
}

static void CreateValuesList(CreateState &state, yyjson_mut_val *vals[], Vector &value_v, idx_t count) {
	auto doc = state.doc;
	FlattenNested(value_v);
	const auto child_count = ListChildCount(value_v, count);
	auto child_v = value_v.GetChild(0);
	auto nested_vals = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, child_count);
	CreateValues(state, nested_vals, child_v, child_count);
	// Add the values to the appropriate JSON arrays
	ValueView<list_entry_t> lists(value_v);
	for (idx_t i = 0; i < count; i++) {
		if (!lists.IsValid(i)) {
			vals[i] = yyjson_mut_null(doc);
			continue;
		}
		vals[i] = yyjson_mut_arr(doc);
		const auto entry = lists.Get(i);
		for (idx_t child_i = entry.offset; child_i < entry.offset + entry.length; child_i++) {
			yyjson_mut_arr_append(vals[i], nested_vals[child_i]);
		}
	}
}

static void CreateValuesArray(CreateState &state, yyjson_mut_val *vals[], Vector &value_v, const LogicalType &type,
                              idx_t count) {
	auto doc = state.doc;
	FlattenNested(value_v);
	const auto array_size = type.GetArraySize();
	const auto child_count = count * array_size;
	auto child_v = value_v.GetChild(0);
	auto nested_vals = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, child_count);
	CreateValues(state, nested_vals, child_v, child_count);
	ValueView<uint8_t> validity(value_v);
	for (idx_t i = 0; i < count; i++) {
		if (!validity.IsValid(i)) {
			vals[i] = yyjson_mut_null(doc);
			continue;
		}
		vals[i] = yyjson_mut_arr(doc);
		const auto offset = i * array_size;
		for (idx_t child_i = offset; child_i < offset + array_size; child_i++) {
			yyjson_mut_arr_append(vals[i], nested_vals[child_i]);
		}
	}
}

static void CreateValuesGeometry(CreateState &state, yyjson_mut_val *vals[], Vector &value_v, idx_t count) {
	if (state.options.geometry_format != JSONGeometryFormat::GEOJSON) {
		CreateValuesFromDefaultCast(state, vals, value_v, count);
		return;
	}
	if (!state.options.geometry_to_geojson) {
		throw NotImplementedException("GeoJSON output of GEOMETRY values is not available here");
	}
	ValueView<blob_t> values(value_v);
	for (idx_t i = 0; i < count; i++) {
		if (!values.IsValid(i)) {
			vals[i] = yyjson_mut_null(state.doc);
		} else {
			vals[i] = state.options.geometry_to_geojson(state.doc, values.Get(i));
		}
	}
}

static void CreateValues(CreateState &state, yyjson_mut_val *vals[], Vector &value_v, idx_t count) {
	auto doc = state.doc;
	const auto type = value_v.GetLogicalType();
	switch (type.GetTypeId()) {
	case LogicalTypeId::SQLNULL:
		CreateValuesNull(doc, vals, count);
		break;
	case LogicalTypeId::BOOLEAN:
		CreateScalarValues<bool>(doc, vals, value_v, count,
		                         [](yyjson_mut_doc *d, bool v) { return yyjson_mut_bool(d, v); });
		break;
	case LogicalTypeId::TINYINT:
		CreateScalarValues<int8_t>(doc, vals, value_v, count,
		                           [](yyjson_mut_doc *d, int8_t v) { return yyjson_mut_sint(d, v); });
		break;
	case LogicalTypeId::SMALLINT:
		CreateScalarValues<int16_t>(doc, vals, value_v, count,
		                            [](yyjson_mut_doc *d, int16_t v) { return yyjson_mut_sint(d, v); });
		break;
	case LogicalTypeId::INTEGER:
		CreateScalarValues<int32_t>(doc, vals, value_v, count,
		                            [](yyjson_mut_doc *d, int32_t v) { return yyjson_mut_sint(d, v); });
		break;
	case LogicalTypeId::BIGINT:
		CreateScalarValues<int64_t>(doc, vals, value_v, count,
		                            [](yyjson_mut_doc *d, int64_t v) { return yyjson_mut_sint(d, v); });
		break;
	case LogicalTypeId::HUGEINT:
		CreateScalarValues<cxx::int128_t>(doc, vals, value_v, count, [](yyjson_mut_doc *d, const cxx::int128_t &v) {
			auto text = Int128ToString(v);
			return yyjson_mut_rawncpy(d, text.c_str(), text.length());
		});
		break;
	case LogicalTypeId::UHUGEINT:
		CreateScalarValues<cxx::uint128_t>(doc, vals, value_v, count, [](yyjson_mut_doc *d, const cxx::uint128_t &v) {
			auto text = UInt128ToString(v);
			return yyjson_mut_rawncpy(d, text.c_str(), text.length());
		});
		break;
	case LogicalTypeId::UTINYINT:
		CreateScalarValues<uint8_t>(doc, vals, value_v, count,
		                            [](yyjson_mut_doc *d, uint8_t v) { return yyjson_mut_uint(d, v); });
		break;
	case LogicalTypeId::USMALLINT:
		CreateScalarValues<uint16_t>(doc, vals, value_v, count,
		                             [](yyjson_mut_doc *d, uint16_t v) { return yyjson_mut_uint(d, v); });
		break;
	case LogicalTypeId::UINTEGER:
		CreateScalarValues<uint32_t>(doc, vals, value_v, count,
		                             [](yyjson_mut_doc *d, uint32_t v) { return yyjson_mut_uint(d, v); });
		break;
	case LogicalTypeId::UBIGINT:
		CreateScalarValues<uint64_t>(doc, vals, value_v, count,
		                             [](yyjson_mut_doc *d, uint64_t v) { return yyjson_mut_uint(d, v); });
		break;
	case LogicalTypeId::FLOAT:
		CreateScalarValues<float>(doc, vals, value_v, count,
		                          [](yyjson_mut_doc *d, float v) { return yyjson_mut_real(d, v); });
		break;
	case LogicalTypeId::DOUBLE:
		CreateScalarValues<double>(doc, vals, value_v, count,
		                           [](yyjson_mut_doc *d, double v) { return yyjson_mut_real(d, v); });
		break;
	case LogicalTypeId::VARCHAR:
		if (IsJSONType(type)) {
			CreateJSONStringValues(doc, vals, value_v, count);
		} else {
			CreateStringValues(doc, vals, value_v, count);
		}
		break;
	case LogicalTypeId::STRUCT:
		CreateValuesStruct(state, vals, value_v, type, count);
		break;
	case LogicalTypeId::TUPLE:
		CreateValuesTuple(state, vals, value_v, type, count);
		break;
	case LogicalTypeId::MAP:
		CreateValuesMap(state, vals, value_v, count);
		break;
	case LogicalTypeId::LIST:
		CreateValuesList(state, vals, value_v, count);
		break;
	case LogicalTypeId::UNION:
		CreateValuesUnion(state, vals, value_v, type, count);
		break;
	case LogicalTypeId::ARRAY:
		CreateValuesArray(state, vals, value_v, type, count);
		break;
	case LogicalTypeId::DATE:
		CreateValuesDate(state, vals, value_v, count);
		break;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		CreateValuesTimestamp(state, vals, value_v, count, type.GetTypeId());
		break;
	case LogicalTypeId::BIT:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::ENUM:
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::UUID:
		CreateValuesFromDefaultCast(state, vals, value_v, count);
		break;
	case LogicalTypeId::GEOMETRY:
	case LogicalTypeId::GEOGRAPHY:
		CreateValuesGeometry(state, vals, value_v, count);
		break;
	case LogicalTypeId::BIGNUM: {
		auto strings = state.CastToVarchar(value_v);
		CreateRawValues(doc, vals, strings.vec, count);
		break;
	}
	case LogicalTypeId::VARIANT: {
		auto json_vector = state.Cast(value_v, JSONType(state.factory));
		CreateJSONStringValues(doc, vals, json_vector.vec, count);
		break;
	}
	case LogicalTypeId::DECIMAL: {
		if (type.GetDecimalWidth() > 15) {
			auto strings = state.CastToVarchar(value_v);
			if (type.GetDecimalWidth() == type.GetDecimalScale()) {
				CreateRawDecimalValues(doc, vals, strings.vec, count);
			} else {
				CreateRawValues(doc, vals, strings.vec, count);
			}
		} else {
			auto doubles = state.Cast(value_v, state.factory.CreateType(LogicalTypeId::DOUBLE));
			CreateScalarValues<double>(doc, vals, doubles.vec, count,
			                           [](yyjson_mut_doc *d, double v) { return yyjson_mut_real(d, v); });
		}
		break;
	}
	default:
		throw InternalException("Unsupported type arrived at JSON create function");
	}
}

void CreateJSONValues(Context &context, yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &input, idx_t count,
                      const JSONCreateOptions &options) {
	CreateState state(context, doc, options);
	CreateValues(state, vals, input, count);
}

void CreateJSONStrings(Context &context, Vector &input, idx_t count, Vector &result, yyjson_alc *alc,
                       const JSONCreateOptions &options) {
	auto doc = JSONCommon::CreateDocument(alc);
	auto vals = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, count);
	CreateJSONValues(context, doc, vals, input, count, options);

	ValueView<uint8_t> validity(input);
	StringWriter writer(result, count);
	for (idx_t i = 0; i < count; i++) {
		if (validity.IsValid(i)) {
			writer.WriteValue(JSONCommon::WriteVal<yyjson_mut_val>(vals[i], alc));
		} else {
			writer.WriteNull();
		}
	}
}

//===--------------------------------------------------------------------===//
// Functions
//===--------------------------------------------------------------------===//
static void ObjectFunction(ScalarFunction::ExecInput &input) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(input);
	auto alc = lstate.json_allocator.GetYYAlc();
	auto &context = input.GetContext();
	auto options = ClientJSONCreateOptions(context);

	const idx_t count = input.GetRowCount();
	auto doc = JSONCommon::CreateDocument(alc);
	CreateState state(context, doc, options);
	auto objs = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, count);
	for (idx_t i = 0; i < count; i++) {
		objs[i] = yyjson_mut_obj(doc);
	}
	// A re-usable value array
	auto vals = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, count);
	// Loop through the key/value pairs
	for (idx_t pair_idx = 0; pair_idx < input.GetArgCount() / 2; pair_idx++) {
		auto key_v = input.GetArg(pair_idx * 2);
		auto value_v = input.GetArg(pair_idx * 2 + 1);
		CreateValues(state, vals, value_v, count);
		ValueView<varchar_t> keys(key_v);
		for (idx_t i = 0; i < count; i++) {
			if (!keys.IsValid(i)) {
				throw InvalidInputException("JSON key cannot be NULL");
			}
			auto key = keys.Get(i);
			yyjson_mut_obj_add(objs[i], yyjson_mut_strncpy(doc, key.data(), key.size()), vals[i]);
		}
	}
	// Write the JSON objects to strings
	auto result = input.GetResult();
	StringWriter writer(result, count);
	for (idx_t i = 0; i < count; i++) {
		writer.WriteValue(JSONCommon::WriteVal<yyjson_mut_val>(objs[i], alc));
	}
}

static void ArrayFunction(ScalarFunction::ExecInput &input) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(input);
	auto alc = lstate.json_allocator.GetYYAlc();
	auto &context = input.GetContext();
	auto options = ClientJSONCreateOptions(context);

	const idx_t count = input.GetRowCount();
	auto doc = JSONCommon::CreateDocument(alc);
	CreateState state(context, doc, options);
	auto arrs = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, count);
	for (idx_t i = 0; i < count; i++) {
		arrs[i] = yyjson_mut_arr(doc);
	}
	auto vals = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, count);
	for (idx_t arg_idx = 0; arg_idx < input.GetArgCount(); arg_idx++) {
		auto value_v = input.GetArg(arg_idx);
		CreateValues(state, vals, value_v, count);
		for (idx_t i = 0; i < count; i++) {
			yyjson_mut_arr_append(arrs[i], vals[i]);
		}
	}
	auto result = input.GetResult();
	StringWriter writer(result, count);
	for (idx_t i = 0; i < count; i++) {
		writer.WriteValue(JSONCommon::WriteVal<yyjson_mut_val>(arrs[i], alc));
	}
}

static void ToJSONFunction(ScalarFunction::ExecInput &input) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(input);
	auto alc = lstate.json_allocator.GetYYAlc();
	auto &context = input.GetContext();
	auto options = ClientJSONCreateOptions(context);
	auto value_v = input.GetArg(0);
	auto result = input.GetResult();
	CreateJSONStrings(context, value_v, input.GetRowCount(), result, alc, options);
}

//===--------------------------------------------------------------------===//
// Bind
//===--------------------------------------------------------------------===//
static bool IsPlainVarchar(const LogicalType &type) {
	return type.GetTypeId() == LogicalTypeId::VARCHAR && type.GetName() == "VARCHAR" && type.GetParamCount() == 0;
}

static void JSONObjectBind(ScalarFunction::BindInput &input) {
	const auto arg_count = input.GetArgCount();
	if (arg_count % 2 != 0) {
		throw BinderException("json_object() requires an even number of arguments");
	}
	for (idx_t i = 0; i < arg_count; i += 2) {
		if (!IsPlainVarchar(input.GetArgType(i))) {
			throw BinderException("json_object() keys must be VARCHAR, add an explicit cast to argument %d",
			                      static_cast<int>(i + 1));
		}
	}
}

static void ToJSONBind(ScalarFunction::BindInput &input) {
	if (input.GetArgCount() != 1) {
		throw BinderException("to_json() takes exactly one argument");
	}
}

static void ArrayToJSONBind(ScalarFunction::BindInput &input) {
	if (input.GetArgCount() != 1) {
		throw BinderException("array_to_json() takes exactly one argument");
	}
	auto id = input.GetArgType(0).GetTypeId();
	if (id != LogicalTypeId::LIST && id != LogicalTypeId::ARRAY && id != LogicalTypeId::SQLNULL) {
		throw BinderException("array_to_json() argument type must be LIST or ARRAY");
	}
}

static void RowToJSONBind(ScalarFunction::BindInput &input) {
	if (input.GetArgCount() != 1) {
		throw BinderException("row_to_json() takes exactly one argument");
	}
	auto id = input.GetArgType(0).GetTypeId();
	if (id != LogicalTypeId::STRUCT && id != LogicalTypeId::SQLNULL) {
		throw BinderException("row_to_json() argument type must be STRUCT");
	}
}

//===--------------------------------------------------------------------===//
// Casts
//===--------------------------------------------------------------------===//
static void CastAnyToJSON(CastFunction::ExecInput &input) {
	auto &allocator = ThreadLocalJSONAllocator();
	allocator.Reset();
	auto &context = input.GetContext();
	auto options = ClientJSONCreateOptions(context);
	auto source = input.GetInput();
	auto target = input.GetOutput();
	CreateJSONStrings(context, source, input.GetRowCount(), target, allocator.GetYYAlc(), options);
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//
static void RegisterCreateFunction(Extension &extension, Factory &factory, const vector<string> &names,
                                   ScalarFunction::ExecCallback exec, ScalarFunction::BindCallback bind,
                                   bool special_null_handling) {
	auto function = ScalarFunction::Create(factory);
	function.WithSignature([&](cxx::FunctionSignature &sig) {
		sig.AddArgs("args", factory.CreateType(LogicalTypeId::ANY));
		sig.SetReturnType(JSONType(factory));
	});
	if (bind) {
		function.SetBindCallback(bind);
	}
	function.SetInitCallback(JSONFunctionLocalState::Init);
	function.SetExecCallback(exec);
	if (special_null_handling) {
		function.SetNullHandling(cxx::FunctionNullHandling::SPECIAL);
	}
	function.SetFallibility(FunctionFallibility::FALLIBLE);
	for (auto &name : names) {
		function.SetName(name);
		extension.Register(function);
	}
}

//! The casts to JSON, one per source type. The nested types and the types with parameters are registered once per
//! kind, as a wildcard source
static void RegisterCreateCasts(Extension &extension, Factory &factory) {
	const auto json = JSONType(factory);

	vector<std::pair<LogicalType, int64_t>> sources;
	for (auto id : {LogicalTypeId::BOOLEAN,   LogicalTypeId::TINYINT,       LogicalTypeId::SMALLINT,
	                LogicalTypeId::INTEGER,   LogicalTypeId::BIGINT,        LogicalTypeId::DATE,
	                LogicalTypeId::TIME,      LogicalTypeId::TIMESTAMP_SEC, LogicalTypeId::TIMESTAMP_MS,
	                LogicalTypeId::TIMESTAMP, LogicalTypeId::TIMESTAMP_NS,  LogicalTypeId::FLOAT,
	                LogicalTypeId::DOUBLE,    LogicalTypeId::BLOB,          LogicalTypeId::INTERVAL,
	                LogicalTypeId::UTINYINT,  LogicalTypeId::USMALLINT,     LogicalTypeId::UINTEGER,
	                LogicalTypeId::UBIGINT,   LogicalTypeId::TIMESTAMP_TZ,  LogicalTypeId::TIMESTAMP_TZ_NS,
	                LogicalTypeId::TIME_TZ,   LogicalTypeId::TIME_NS,       LogicalTypeId::BIT,
	                LogicalTypeId::BIGNUM,    LogicalTypeId::UHUGEINT,      LogicalTypeId::HUGEINT,
	                LogicalTypeId::UUID,      LogicalTypeId::GEOMETRY}) {
		// We prefer going to JSON over going to VARCHAR if a function can do either: the cost of the implicit cast to
		// VARCHAR (the engine has none for these types) minus one, but at least 0
		try {
			sources.emplace_back(factory.CreateType(id), 0);
		} catch (cxx::Exception &) {
			sources.emplace_back(factory.ParseType("GEOMETRY"), 0);
		}
	}
	for (auto &source : sources) {
		auto cast = CastFunction::Create(factory);
		cast.SetSourceType(source.first)
		    .SetTargetType(json)
		    .SetImplicitCastCost(source.second)
		    .SetExecCallback(CastAnyToJSON);
		extension.Register(cast);
	}

	// The nested types and the types with parameters. ENUM has an implicit cast to VARCHAR (cost 149), the other
	// kinds have none. VARIANT is left to the engine's own cast to JSON
	const std::pair<cxx::CastWildcardKind, int64_t> kinds[] = {
	    {cxx::CastWildcardKind::STRUCT, 0},  {cxx::CastWildcardKind::TUPLE, 0}, {cxx::CastWildcardKind::LIST, 0},
	    {cxx::CastWildcardKind::MAP, 0},     {cxx::CastWildcardKind::UNION, 0}, {cxx::CastWildcardKind::ARRAY, 0},
	    {cxx::CastWildcardKind::DECIMAL, 0}, {cxx::CastWildcardKind::ENUM, 148}};
	for (auto &kind : kinds) {
		auto cast = CastFunction::Create(factory);
		cast.SetSourceKind(kind.first)
		    .SetTargetType(json)
		    .SetImplicitCastCost(kind.second)
		    .SetExecCallback(CastAnyToJSON);
		extension.Register(cast);
	}
}

void RegisterCreateFunctions(Extension &extension, Context &context) {
	auto &factory = context.GetFactory();
	RegisterCreateFunction(extension, factory, {"json_object"}, ObjectFunction, JSONObjectBind, true);
	RegisterCreateFunction(extension, factory, {"json_array"}, ArrayFunction, nullptr, true);
	RegisterCreateFunction(extension, factory, {"to_json", "json_quote"}, ToJSONFunction, ToJSONBind, false);
	RegisterCreateFunction(extension, factory, {"array_to_json"}, ToJSONFunction, ArrayToJSONBind, false);
	RegisterCreateFunction(extension, factory, {"row_to_json"}, ToJSONFunction, RowToJSONBind, false);
	RegisterCreateCasts(extension, factory);
}

} // namespace json
} // namespace duckdb
