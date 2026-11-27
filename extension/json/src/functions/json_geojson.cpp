// ST_AsGeoJSON, ST_GeomFromGeoJSON, the conversion between WKB and
// GeoJSON, and the json_geometry_format setting.

#include "json_geojson.hpp"
#include "json_transform.hpp"

#include <limits>

namespace duckdb {
namespace json {

namespace {

constexpr idx_t MAX_RECURSION_DEPTH = 16;

//! GEOMETRY values are little endian WKB and the extension only runs on little endian hosts
class WKBReader {
public:
	explicit WKBReader(std::string_view wkb) : pos(wkb.data()), end(wkb.data() + wkb.size()) {
	}

	template <class T>
	T Read() {
		if (pos + sizeof(T) > end) {
			throw InvalidInputException("Unexpected end of WKB data while converting to GeoJSON");
		}
		T value;
		memcpy(&value, pos, sizeof(T));
		pos += sizeof(T);
		return value;
	}

private:
	const char *pos;
	const char *end;
};

class WKBWriter {
public:
	template <class T>
	void Write(const T &value) {
		const auto ptr = reinterpret_cast<const char *>(&value);
		buffer.append(ptr, sizeof(T));
	}

	const string &GetBuffer() const {
		return buffer;
	}

private:
	string buffer;
};

enum class GeometryType : uint32_t {
	INVALID = 0,
	POINT = 1,
	LINESTRING = 2,
	POLYGON = 3,
	MULTIPOINT = 4,
	MULTILINESTRING = 5,
	MULTIPOLYGON = 6,
	GEOMETRYCOLLECTION = 7
};

//! Header of a single WKB geometry: the byte order byte plus the type/dimension word
struct WKBHeader {
	GeometryType type;
	bool has_z;
	bool has_m;
};

WKBHeader ReadHeader(WKBReader &reader) {
	const auto byte_order = reader.Read<uint8_t>();
	if (byte_order != 1) {
		throw InvalidInputException("Unsupported byte order %d in WKB", static_cast<int>(byte_order));
	}
	const auto meta = reader.Read<uint32_t>();
	const auto type_id = (meta & 0x0000FFFF) % 1000;
	const auto flag_id = (meta & 0x0000FFFF) / 1000;
	if (type_id < 1 || type_id > 7) {
		throw InvalidInputException("Unsupported geometry type %d in WKB", static_cast<int>(type_id));
	}
	if (flag_id > 3) {
		throw InvalidInputException("Unsupported geometry flag %d in WKB", static_cast<int>(flag_id));
	}
	WKBHeader header;
	header.type = static_cast<GeometryType>(type_id);
	header.has_z = (flag_id & 0x01) != 0;
	header.has_m = (flag_id & 0x02) != 0;
	return header;
}

void WriteHeader(WKBWriter &writer, const GeometryType type, const bool has_z) {
	writer.Write<uint8_t>(1);
	writer.Write<uint32_t>(static_cast<uint32_t>(type) + (has_z ? 1000 : 0));
}

const char *GeometryTypeToGeoJSONName(const GeometryType type) {
	switch (type) {
	case GeometryType::POINT:
		return "Point";
	case GeometryType::LINESTRING:
		return "LineString";
	case GeometryType::POLYGON:
		return "Polygon";
	case GeometryType::MULTIPOINT:
		return "MultiPoint";
	case GeometryType::MULTILINESTRING:
		return "MultiLineString";
	case GeometryType::MULTIPOLYGON:
		return "MultiPolygon";
	case GeometryType::GEOMETRYCOLLECTION:
		return "GeometryCollection";
	default:
		throw InvalidInputException("Unsupported geometry type in WKB");
	}
}

bool GeoJSONNameToGeometryType(const char *name, const size_t len, GeometryType &result) {
	for (idx_t i = 1; i <= 7; i++) {
		const auto candidate = static_cast<GeometryType>(i);
		const auto candidate_name = GeometryTypeToGeoJSONName(candidate);
		if (strlen(candidate_name) == len && memcmp(candidate_name, name, len) == 0) {
			result = candidate;
			return true;
		}
	}
	return false;
}

//===--------------------------------------------------------------------===//
// WKB -> GeoJSON
//===--------------------------------------------------------------------===//
struct Position {
	double x;
	double y;
	double z;
	//! An empty point is stored as all-NaN ordinates, and has no GeoJSON representation
	bool is_empty;
};

Position ReadPosition(WKBReader &reader, const WKBHeader &header) {
	Position result;
	result.x = reader.Read<double>();
	result.y = reader.Read<double>();
	result.z = header.has_z ? reader.Read<double>() : 0;
	if (header.has_m) {
		reader.Read<double>(); // GeoJSON has no M dimension
	}
	result.is_empty = std::isnan(result.x) && std::isnan(result.y);
	return result;
}

void AppendOrdinates(yyjson_mut_doc *doc, yyjson_mut_val *arr, const Position &pos, const bool has_z) {
	yyjson_mut_arr_add_real(doc, arr, pos.x);
	yyjson_mut_arr_add_real(doc, arr, pos.y);
	if (has_z) {
		yyjson_mut_arr_add_real(doc, arr, pos.z);
	}
}

//! Reads a length-prefixed vertex run, appending one position array per vertex
void ReadVertexArray(WKBReader &reader, const WKBHeader &header, yyjson_mut_doc *doc, yyjson_mut_val *arr) {
	const auto vert_count = reader.Read<uint32_t>();
	for (uint32_t i = 0; i < vert_count; i++) {
		const auto pos = ReadPosition(reader, header);
		auto coord = yyjson_mut_arr(doc);
		AppendOrdinates(doc, coord, pos, header.has_z);
		yyjson_mut_arr_append(arr, coord);
	}
}

void ToGeoJSONRecursive(WKBReader &reader, yyjson_mut_doc *doc, yyjson_mut_val *obj, const idx_t depth) {
	if (depth == MAX_RECURSION_DEPTH) {
		throw InvalidInputException("Geometry exceeds maximum recursion depth of %d",
		                            static_cast<int>(MAX_RECURSION_DEPTH));
	}

	const auto header = ReadHeader(reader);
	yyjson_mut_obj_add_str(doc, obj, "type", GeometryTypeToGeoJSONName(header.type));

	if (header.type == GeometryType::GEOMETRYCOLLECTION) {
		auto geometries = yyjson_mut_arr(doc);
		yyjson_mut_obj_add_val(doc, obj, "geometries", geometries);
		const auto geom_count = reader.Read<uint32_t>();
		for (uint32_t i = 0; i < geom_count; i++) {
			auto child = yyjson_mut_obj(doc);
			ToGeoJSONRecursive(reader, doc, child, depth + 1);
			yyjson_mut_arr_append(geometries, child);
		}
		return;
	}

	auto coords = yyjson_mut_arr(doc);
	yyjson_mut_obj_add_val(doc, obj, "coordinates", coords);

	switch (header.type) {
	case GeometryType::POINT: {
		const auto pos = ReadPosition(reader, header);
		if (!pos.is_empty) {
			AppendOrdinates(doc, coords, pos, header.has_z);
		}
	} break;
	case GeometryType::LINESTRING:
		ReadVertexArray(reader, header, doc, coords);
		break;
	case GeometryType::POLYGON: {
		const auto ring_count = reader.Read<uint32_t>();
		for (uint32_t i = 0; i < ring_count; i++) {
			auto ring = yyjson_mut_arr(doc);
			ReadVertexArray(reader, header, doc, ring);
			yyjson_mut_arr_append(coords, ring);
		}
	} break;
	case GeometryType::MULTIPOINT: {
		// GeoJSON stores MultiPoint coordinates as a flat array of positions
		const auto part_count = reader.Read<uint32_t>();
		for (uint32_t i = 0; i < part_count; i++) {
			const auto part = ReadHeader(reader);
			const auto pos = ReadPosition(reader, part);
			if (pos.is_empty) {
				continue;
			}
			auto coord = yyjson_mut_arr(doc);
			AppendOrdinates(doc, coord, pos, part.has_z);
			yyjson_mut_arr_append(coords, coord);
		}
	} break;
	case GeometryType::MULTILINESTRING: {
		const auto part_count = reader.Read<uint32_t>();
		for (uint32_t i = 0; i < part_count; i++) {
			const auto part = ReadHeader(reader);
			auto line = yyjson_mut_arr(doc);
			ReadVertexArray(reader, part, doc, line);
			yyjson_mut_arr_append(coords, line);
		}
	} break;
	case GeometryType::MULTIPOLYGON: {
		const auto part_count = reader.Read<uint32_t>();
		for (uint32_t i = 0; i < part_count; i++) {
			const auto part = ReadHeader(reader);
			auto poly = yyjson_mut_arr(doc);
			const auto ring_count = reader.Read<uint32_t>();
			for (uint32_t j = 0; j < ring_count; j++) {
				auto ring = yyjson_mut_arr(doc);
				ReadVertexArray(reader, part, doc, ring);
				yyjson_mut_arr_append(poly, ring);
			}
			yyjson_mut_arr_append(coords, poly);
		}
	} break;
	default:
		throw InvalidInputException("Unsupported geometry type in WKB");
	}
}

//===--------------------------------------------------------------------===//
// WKB extent
//===--------------------------------------------------------------------===//
void ExtentAdd(JSONGeometryExtent &extent, const Position &pos, bool has_z) {
	if (!pos.is_empty) {
		if (!extent.has_xy) {
			extent.x_min = extent.x_max = pos.x;
			extent.y_min = extent.y_max = pos.y;
			extent.has_xy = true;
		}
		extent.x_min = std::min(extent.x_min, pos.x);
		extent.x_max = std::max(extent.x_max, pos.x);
		extent.y_min = std::min(extent.y_min, pos.y);
		extent.y_max = std::max(extent.y_max, pos.y);
	}
	if (has_z && !std::isnan(pos.z)) {
		if (!extent.has_z) {
			extent.z_min = extent.z_max = pos.z;
			extent.has_z = true;
		}
		extent.z_min = std::min(extent.z_min, pos.z);
		extent.z_max = std::max(extent.z_max, pos.z);
	}
}

void ExtentVertices(WKBReader &reader, const WKBHeader &header, JSONGeometryExtent &extent) {
	const auto count = reader.Read<uint32_t>();
	for (uint32_t i = 0; i < count; i++) {
		ExtentAdd(extent, ReadPosition(reader, header), header.has_z);
	}
}

void ExtentRecursive(WKBReader &reader, JSONGeometryExtent &extent, const idx_t depth) {
	if (depth == MAX_RECURSION_DEPTH) {
		throw InvalidInputException("Geometry exceeds maximum recursion depth of %d",
		                            static_cast<int>(MAX_RECURSION_DEPTH));
	}
	const auto header = ReadHeader(reader);
	switch (header.type) {
	case GeometryType::POINT:
		ExtentAdd(extent, ReadPosition(reader, header), header.has_z);
		break;
	case GeometryType::LINESTRING:
		ExtentVertices(reader, header, extent);
		break;
	case GeometryType::POLYGON: {
		const auto ring_count = reader.Read<uint32_t>();
		for (uint32_t i = 0; i < ring_count; i++) {
			ExtentVertices(reader, header, extent);
		}
		break;
	}
	default: {
		const auto part_count = reader.Read<uint32_t>();
		for (uint32_t i = 0; i < part_count; i++) {
			ExtentRecursive(reader, extent, depth + 1);
		}
		break;
	}
	}
}

//===--------------------------------------------------------------------===//
// GeoJSON -> WKB
//===--------------------------------------------------------------------===//
//! A number, or a number kept as its text (YYJSON_READ_NUMBER_AS_RAW, when a column of the scan is a DECIMAL)
bool TryGetOrdinate(yyjson_val *val, double &result) {
	if (yyjson_is_num(val)) {
		result = yyjson_get_num(val);
		return true;
	}
	yyjson_val number;
	if (yyjson_is_raw(val) && JSONCommon::ReadRawNumber(val, number)) {
		result = yyjson_get_num(&number);
		return true;
	}
	return false;
}

//! Whether any position in this coordinate tree carries a third ordinate
bool CoordinatesHaveZ(yyjson_val *coords, const idx_t depth) {
	if (depth == MAX_RECURSION_DEPTH || !yyjson_is_arr(coords)) {
		return false;
	}
	size_t idx, max;
	yyjson_val *child;
	yyjson_arr_foreach(coords, idx, max, child) {
		if (yyjson_is_num(child) || yyjson_is_raw(child)) {
			// This array is a position rather than a nesting level
			return yyjson_arr_size(coords) >= 3;
		}
		if (CoordinatesHaveZ(child, depth + 1)) {
			return true;
		}
	}
	return false;
}

//! GeoJSON allows mixing 2D and 3D positions, but a geometry must have uniform dimensions, so a single
//! 3D position promotes the whole geometry to XYZ (missing ordinates are padded with 0)
bool GeometryHasZ(yyjson_val *val, const idx_t depth) {
	if (depth == MAX_RECURSION_DEPTH || !yyjson_is_obj(val)) {
		return false;
	}
	auto geometries = yyjson_obj_get(val, "geometries");
	if (geometries && yyjson_is_arr(geometries)) {
		size_t idx, max;
		yyjson_val *child;
		yyjson_arr_foreach(geometries, idx, max, child) {
			if (GeometryHasZ(child, depth + 1)) {
				return true;
			}
		}
		return false;
	}
	return CoordinatesHaveZ(yyjson_obj_get(val, "coordinates"), depth);
}

void WritePosition(WKBWriter &writer, yyjson_val *pos, const bool has_z, const char *context) {
	if (!yyjson_is_arr(pos) || yyjson_arr_size(pos) < 2) {
		throw InvalidInputException("Invalid GeoJSON: %s position must be an array of at least 2 numbers", context);
	}
	const auto dims = has_z ? 3u : 2u;
	double ordinates[3] = {0, 0, 0};
	size_t idx, max;
	yyjson_val *child;
	yyjson_arr_foreach(pos, idx, max, child) {
		if (idx >= dims) {
			break;
		}
		if (!TryGetOrdinate(child, ordinates[idx])) {
			throw InvalidInputException("Invalid GeoJSON: %s position must contain only numbers", context);
		}
	}
	writer.Write(ordinates[0]);
	writer.Write(ordinates[1]);
	if (has_z) {
		writer.Write(ordinates[2]);
	}
}

void WriteVertexArray(WKBWriter &writer, yyjson_val *arr, const bool has_z, const char *context) {
	if (!yyjson_is_arr(arr)) {
		throw InvalidInputException("Invalid GeoJSON: %s coordinates are nested too shallowly", context);
	}
	writer.Write<uint32_t>(static_cast<uint32_t>(yyjson_arr_size(arr)));
	size_t idx, max;
	yyjson_val *child;
	yyjson_arr_foreach(arr, idx, max, child) {
		WritePosition(writer, child, has_z, context);
	}
}

void FromGeoJSONRecursive(yyjson_val *val, WKBWriter &writer, const bool has_z, const idx_t depth) {
	if (depth == MAX_RECURSION_DEPTH) {
		throw InvalidInputException("GeoJSON geometry exceeds maximum recursion depth of %d",
		                            static_cast<int>(MAX_RECURSION_DEPTH));
	}
	if (!yyjson_is_obj(val)) {
		throw InvalidInputException("Invalid GeoJSON: geometry must be an object");
	}
	auto type_val = yyjson_obj_get(val, "type");
	if (!type_val || !yyjson_is_str(type_val)) {
		throw InvalidInputException("Invalid GeoJSON: geometry is missing a \"type\" string");
	}
	GeometryType type;
	if (!GeoJSONNameToGeometryType(yyjson_get_str(type_val), yyjson_get_len(type_val), type)) {
		throw InvalidInputException("Invalid GeoJSON: unknown geometry type \"%s\"", yyjson_get_str(type_val));
	}
	const auto context = GeometryTypeToGeoJSONName(type);
	WriteHeader(writer, type, has_z);

	if (type == GeometryType::GEOMETRYCOLLECTION) {
		auto geometries = yyjson_obj_get(val, "geometries");
		if (!geometries || !yyjson_is_arr(geometries)) {
			throw InvalidInputException("Invalid GeoJSON: GeometryCollection requires a \"geometries\" array");
		}
		writer.Write<uint32_t>(static_cast<uint32_t>(yyjson_arr_size(geometries)));
		size_t idx, max;
		yyjson_val *child;
		yyjson_arr_foreach(geometries, idx, max, child) {
			FromGeoJSONRecursive(child, writer, has_z, depth + 1);
		}
		return;
	}

	auto coords = yyjson_obj_get(val, "coordinates");
	if (!coords || !yyjson_is_arr(coords)) {
		throw InvalidInputException("Invalid GeoJSON: %s requires a \"coordinates\" array", context);
	}

	size_t idx, max;
	yyjson_val *child;
	switch (type) {
	case GeometryType::POINT:
		if (yyjson_arr_size(coords) == 0) {
			// An empty point is stored as all-NaN ordinates
			const auto nan = std::numeric_limits<double>::quiet_NaN();
			writer.Write(nan);
			writer.Write(nan);
			if (has_z) {
				writer.Write(nan);
			}
		} else {
			WritePosition(writer, coords, has_z, context);
		}
		break;
	case GeometryType::LINESTRING:
		WriteVertexArray(writer, coords, has_z, context);
		break;
	case GeometryType::POLYGON:
		writer.Write<uint32_t>(static_cast<uint32_t>(yyjson_arr_size(coords)));
		yyjson_arr_foreach(coords, idx, max, child) {
			WriteVertexArray(writer, child, has_z, context);
		}
		break;
	case GeometryType::MULTIPOINT:
		writer.Write<uint32_t>(static_cast<uint32_t>(yyjson_arr_size(coords)));
		yyjson_arr_foreach(coords, idx, max, child) {
			WriteHeader(writer, GeometryType::POINT, has_z);
			WritePosition(writer, child, has_z, context);
		}
		break;
	case GeometryType::MULTILINESTRING:
		writer.Write<uint32_t>(static_cast<uint32_t>(yyjson_arr_size(coords)));
		yyjson_arr_foreach(coords, idx, max, child) {
			WriteHeader(writer, GeometryType::LINESTRING, has_z);
			WriteVertexArray(writer, child, has_z, context);
		}
		break;
	case GeometryType::MULTIPOLYGON:
		writer.Write<uint32_t>(static_cast<uint32_t>(yyjson_arr_size(coords)));
		yyjson_arr_foreach(coords, idx, max, child) {
			WriteHeader(writer, GeometryType::POLYGON, has_z);
			if (!yyjson_is_arr(child)) {
				throw InvalidInputException("Invalid GeoJSON: %s coordinates are nested too shallowly", context);
			}
			writer.Write<uint32_t>(static_cast<uint32_t>(yyjson_arr_size(child)));
			size_t ring_idx, ring_max;
			yyjson_val *ring;
			yyjson_arr_foreach(child, ring_idx, ring_max, ring) {
				WriteVertexArray(writer, ring, has_z, context);
			}
		}
		break;
	default:
		throw InvalidInputException("Invalid GeoJSON: unsupported geometry type \"%s\"", context);
	}
}

} // namespace

//===--------------------------------------------------------------------===//
// Public interface
//===--------------------------------------------------------------------===//
yyjson_mut_val *WKBToGeoJSON(yyjson_mut_doc *doc, std::string_view wkb) {
	WKBReader reader(wkb);
	auto obj = yyjson_mut_obj(doc);
	ToGeoJSONRecursive(reader, doc, obj, 0);
	return obj;
}

bool GeoJSONToWKB(yyjson_val *val, string &wkb, string &error) {
	WKBWriter writer;
	try {
		FromGeoJSONRecursive(val, writer, GeometryHasZ(val, 0), 0);
		wkb = writer.GetBuffer();
		return true;
	} catch (cxx::Exception &ex) {
		error = ex.GetRawMessage().empty() ? string(ex.what()) : ex.GetRawMessage();
		return false;
	}
}

bool IsGeoJSONGeometry(yyjson_val *val) {
	if (!yyjson_is_obj(val)) {
		return false;
	}
	auto type_val = yyjson_obj_get(val, "type");
	if (!type_val || !yyjson_is_str(type_val)) {
		return false;
	}
	GeometryType type;
	if (!GeoJSONNameToGeometryType(yyjson_get_str(type_val), yyjson_get_len(type_val), type)) {
		return false;
	}
	const auto member = yyjson_obj_get(val, type == GeometryType::GEOMETRYCOLLECTION ? "geometries" : "coordinates");
	return member && yyjson_is_arr(member);
}

JSONGeometryExtent GetWKBExtent(std::string_view wkb) {
	JSONGeometryExtent extent;
	WKBReader reader(wkb);
	ExtentRecursive(reader, extent, 0);
	return extent;
}

//===--------------------------------------------------------------------===//
// Scalar functions
//===--------------------------------------------------------------------===//
static void AsGeoJSONFunction(ScalarFunction::ExecInput &input) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(input);
	auto alc = lstate.json_allocator.GetYYAlc();
	auto doc = JSONCommon::CreateDocument(alc);

	const auto count = input.GetRowCount();
	ValueView<blob_t> geometries(input.GetArg(0));
	auto result = input.GetResult();
	StringWriter writer(result, count);
	for (idx_t i = 0; i < count; i++) {
		if (!geometries.IsValid(i)) {
			writer.WriteNull();
			continue;
		}
		auto geojson = WKBToGeoJSON(doc, geometries.Get(i).view());
		writer.WriteValue(JSONCommon::WriteVal<yyjson_mut_val>(geojson, alc));
	}
}

static void GeomFromGeoJSONFunction(ScalarFunction::ExecInput &input) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(input);
	auto alc = lstate.json_allocator.GetYYAlc();

	const auto count = input.GetRowCount();
	ValueView<varchar_t> inputs(input.GetArg(0));
	auto result = input.GetResult();
	StringWriter writer(result, count);
	for (idx_t i = 0; i < count; i++) {
		if (!inputs.IsValid(i)) {
			writer.WriteNull();
			continue;
		}
		auto doc = JSONCommon::ReadDocument(inputs.Get(i), JSONCommon::READ_FLAG, alc);
		string wkb;
		string error;
		if (!GeoJSONToWKB(doc->root, wkb, error)) {
			throw InvalidInputException(error);
		}
		writer.WriteValue(wkb);
	}
}

static LogicalType GeometryLogicalType(Factory &factory) {
	try {
		return factory.CreateType(LogicalTypeId::GEOMETRY);
	} catch (cxx::Exception &) {
		return factory.ParseType("GEOMETRY");
	}
}

//===--------------------------------------------------------------------===//
// json_geometry_format
//===--------------------------------------------------------------------===//
JSONGeometryFormat ParseJSONGeometryFormat(const Value &value) {
	if (value.IsNull()) {
		return JSONGeometryFormat::WKT;
	}
	auto text = value.Get<varchar_t>();
	string format(text.data(), text.size());
	string lower = format;
	for (auto &c : lower) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c + ('a' - 'A'));
		}
	}
	if (lower == "wkt") {
		return JSONGeometryFormat::WKT;
	}
	if (lower == "geojson") {
		return JSONGeometryFormat::GEOJSON;
	}
	throw InvalidInputException("Invalid value \"%s\" for json_geometry_format, expected 'wkt' or 'geojson'", format);
}

static void ValidateGeometryFormat(cxx::CustomOption::UpdateInput &input) {
	ParseJSONGeometryFormat(input.GetValue());
}

static yyjson_mut_val *GeometryToGeoJSONHook(yyjson_mut_doc *doc, blob_t wkb) {
	return WKBToGeoJSON(doc, wkb.view());
}

static bool GeoJSONParserHook(yyjson_val *val, string &wkb, string &error) {
	return GeoJSONToWKB(val, wkb, error);
}

void RegisterGeoJSON(Extension &extension, Context &context) {
	auto &factory = context.GetFactory();

	auto format_option = cxx::CustomOption::Create(factory);
	format_option.SetName("json_geometry_format")
	    .SetDescription("How GEOMETRY values are written to JSON: 'wkt' for Well-Known Text, or 'geojson' for GeoJSON "
	                    "geometry objects. COPY ... TO ... (FORMAT GEOJSON) always writes GeoJSON regardless of this "
	                    "setting.")
	    .SetType(factory.CreateType(LogicalTypeId::VARCHAR))
	    .SetDefaultValue(Value::Create(factory, varchar_t(string_view("wkt"))))
	    .SetUpdateCallback(ValidateGeometryFormat);
	// a config that outlives its database (a restart) still holds the option of the previous load
	if (!context.GetConfig().TryGetOption("json_geometry_format")) {
		extension.Register(format_option);
	}

	SetGeometryToGeoJSON(GeometryToGeoJSONHook);
	JSONTransform::SetGeoJSONParser(GeoJSONParserHook);

	auto as_geojson = ScalarFunction::Create(factory);
	as_geojson.SetName("st_asgeojson")
	    .WithSignature([&](cxx::FunctionSignature &sig) {
		    sig.AddParameter("geom", GeometryLogicalType(factory));
		    sig.SetReturnType(JSONType(factory));
	    })
	    .SetInitCallback(JSONFunctionLocalState::Init)
	    .SetExecCallback(AsGeoJSONFunction)
	    .SetFallibility(FunctionFallibility::FALLIBLE);
	extension.Register(as_geojson);

	for (auto id : {LogicalTypeId::VARCHAR, LogicalTypeId::INVALID}) {
		auto from_geojson = ScalarFunction::Create(factory);
		auto input_type = id == LogicalTypeId::INVALID ? JSONType(factory) : factory.CreateType(id);
		from_geojson.SetName("st_geomfromgeojson")
		    .WithSignature([&](cxx::FunctionSignature &sig) {
			    sig.AddParameter("geojson", input_type);
			    sig.SetReturnType(GeometryLogicalType(factory));
		    })
		    .SetInitCallback(JSONFunctionLocalState::Init)
		    .SetExecCallback(GeomFromGeoJSONFunction)
		    .SetFallibility(FunctionFallibility::FALLIBLE);
		extension.Register(from_geojson);
	}
}

} // namespace json
} // namespace duckdb
