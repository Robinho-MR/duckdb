// json_transform / from_json (and the strict versions) and the casts from JSON to every other type. Ports the
// registration half of legacy/json_functions/json_transform.cpp; the transformation itself is json_transform.cpp.

#include "json_transform.hpp"

namespace duckdb {
namespace json {

//===--------------------------------------------------------------------===//
// Structure string to type
//===--------------------------------------------------------------------===//
static LogicalType StructureStringToType(yyjson_val *val, Factory &factory);

static LogicalType StructureStringToTypeArray(yyjson_val *arr, Factory &factory) {
	if (yyjson_arr_size(arr) != 1) {
		throw BinderException("Too many values in array of JSON structure");
	}
	return ListOf(factory, StructureStringToType(yyjson_arr_get_first(arr), factory));
}

static string LowerCase(const string &input) {
	string result = input;
	for (auto &c : result) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c + ('a' - 'A'));
		}
	}
	return result;
}

static LogicalType StructureToTypeObject(yyjson_val *obj, Factory &factory) {
	// STRUCT field names are case-insensitive
	unordered_set<string> names;
	vector<cxx::TypeParam> fields;
	size_t idx, max;
	yyjson_val *key, *val;
	yyjson_obj_foreach(obj, idx, max, key, val) {
		val = yyjson_obj_iter_get_val(key);
		string key_str(unsafe_yyjson_get_str(key), unsafe_yyjson_get_len(key));
		if (key_str.empty()) {
			throw BinderException("Empty key in object in JSON structure");
		}
		if (!names.insert(LowerCase(key_str)).second) {
			JSONCommon::ThrowValFormatError("Duplicate keys in object in JSON structure: %s", val);
		}
		fields.emplace_back(key_str, Value::Create(factory, StructureStringToType(val, factory)));
	}
	D_ASSERT(yyjson_obj_size(obj) == names.size());
	if (fields.empty()) {
		throw BinderException("Empty object in JSON structure");
	}
	return factory.CreateType(LogicalTypeId::STRUCT, fields);
}

static LogicalType StructureStringToType(yyjson_val *val, Factory &factory) {
	switch (yyjson_get_tag(val)) {
	case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE:
		return StructureStringToTypeArray(val, factory);
	case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE:
		return StructureToTypeObject(val, factory);
	case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NOESC:
	case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NONE:
		return factory.ParseType(unsafe_yyjson_get_str(val));
	default:
		throw BinderException("invalid JSON structure");
	}
}

//===--------------------------------------------------------------------===//
// json_transform
//===--------------------------------------------------------------------===//
static void JSONTransformResolveTypes(ScalarFunction::ResolveTypesInput &input) {
	auto &factory = input.GetContext().GetFactory();
	auto structure_val = input.TryGetConstantArgument(1);
	if (!structure_val) {
		throw BinderException("The \"structure\" argument in function \"%s\" must be a constant expression",
		                      input.GetUserData<string>());
	}
	if (structure_val->IsNull() || input.GetArgType(1).GetTypeId() == LogicalTypeId::SQLNULL) {
		input.SetReturnType(factory.ParseType("\"NULL\""));
		return;
	}
	std::optional<Value> json_structure_val;
	try {
		json_structure_val.emplace(structure_val->Cast(factory, JSONType(factory)));
	} catch (cxx::Exception &) {
		throw BinderException("Cannot cast JSON structure to string");
	}
	if (json_structure_val->IsNull()) {
		throw BinderException("Cannot cast JSON structure to string");
	}
	auto structure_string = json_structure_val->Get<varchar_t>();
	JSONAllocator json_allocator;
	auto doc = JSONCommon::ReadDocument(structure_string, JSONCommon::READ_FLAG, json_allocator.GetYYAlc());
	input.SetReturnType(StructureStringToType(doc->root, factory));
}

static bool TransformFunctionInternal(const Vector &input, const idx_t count, Vector &result, yyjson_alc *alc,
                                      JSONTransformOptions &options) {
	ValueView<varchar_t> inputs(input);

	// Read documents
	auto vals = JSONCommon::AllocateArray<yyjson_val *>(alc, count);
	options.numbers_as_raw = JSONCommon::NumbersAsRaw(result.GetLogicalType());
	const auto read_flags = JSONCommon::GetReadFlag(JSONCommon::READ_FLAG, options.numbers_as_raw);
	for (idx_t i = 0; i < count; i++) {
		if (!inputs.IsValid(i)) {
			vals[i] = nullptr;
		} else {
			vals[i] = JSONCommon::ReadDocument(inputs.Get(i), read_flags, alc)->root;
		}
	}
	return JSONTransform::Transform(vals, alc, result, count, options, nullptr);
}

template <bool strict>
static void TransformFunction(ScalarFunction::ExecInput &input) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(input);
	auto alc = lstate.json_allocator.GetYYAlc();

	JSONTransformOptions options(input.GetContext().GetFactory(), strict, strict, strict, false);
	auto args = input.GetArg(0);
	auto result = input.GetResult();
	if (!TransformFunctionInternal(args, input.GetRowCount(), result, alc, options)) {
		throw InvalidInputException(options.error_message);
	}
}

static void RegisterTransformFunction(Extension &extension, Factory &factory, const vector<string> &names,
                                      ScalarFunction::ExecCallback exec) {
	for (const bool json_input : {false, true}) {
		auto input_type = json_input ? JSONType(factory) : factory.CreateType(LogicalTypeId::VARCHAR);
		auto function = ScalarFunction::Create(factory);
		function.WithSignature([&](cxx::FunctionSignature &sig) {
			sig.AddParameter("json", input_type);
			sig.AddParameter("structure", factory.CreateType(LogicalTypeId::VARCHAR));
			sig.SetReturnType(factory.CreateType(LogicalTypeId::ANY));
		});
		function.SetResolveTypesCallback(JSONTransformResolveTypes);
		function.SetExecCallback(exec);
		function.SetInitCallback(JSONFunctionLocalState::Init);
		function.SetFallibility(FunctionFallibility::FALLIBLE);
		for (auto &name : names) {
			function.SetName(name);
			function.SetUserData<string>(name);
			extension.Register(function);
		}
	}
}

//===--------------------------------------------------------------------===//
// Casts from JSON
//===--------------------------------------------------------------------===//
static void CastJSONToAny(CastFunction::ExecInput &input) {
	auto &allocator = ThreadLocalJSONAllocator();
	allocator.Reset();
	auto alc = allocator.GetYYAlc();

	JSONTransformOptions options(input.GetContext().GetFactory(), true, true, true, true);
	options.delay_error = true;

	auto source = input.GetInput();
	auto target = input.GetOutput();
	auto success = TransformFunctionInternal(source, input.GetRowCount(), target, alc, options);
	if (!success && input.GetMode() == cxx::CastMode::NORMAL) {
		throw ConversionException(options.error_message);
	}
}

// There is no cast to VARIANT: the legacy registration for it never matched a VARIANT, which is cast by the engine
static void RegisterTransformCasts(Extension &extension, Factory &factory) {
	static const LogicalTypeId TARGETS[] = {LogicalTypeId::BOOLEAN,      LogicalTypeId::TINYINT,
	                                        LogicalTypeId::SMALLINT,     LogicalTypeId::INTEGER,
	                                        LogicalTypeId::BIGINT,       LogicalTypeId::DATE,
	                                        LogicalTypeId::TIME,         LogicalTypeId::TIMESTAMP_SEC,
	                                        LogicalTypeId::TIMESTAMP_MS, LogicalTypeId::TIMESTAMP,
	                                        LogicalTypeId::TIMESTAMP_NS, LogicalTypeId::DECIMAL,
	                                        LogicalTypeId::FLOAT,        LogicalTypeId::DOUBLE,
	                                        LogicalTypeId::BLOB,         LogicalTypeId::INTERVAL,
	                                        LogicalTypeId::UTINYINT,     LogicalTypeId::USMALLINT,
	                                        LogicalTypeId::UINTEGER,     LogicalTypeId::UBIGINT,
	                                        LogicalTypeId::TIMESTAMP_TZ, LogicalTypeId::TIMESTAMP_TZ_NS,
	                                        LogicalTypeId::TIME_TZ,      LogicalTypeId::TIME_NS,
	                                        LogicalTypeId::BIT,          LogicalTypeId::BIGNUM,
	                                        LogicalTypeId::UHUGEINT,     LogicalTypeId::HUGEINT,
	                                        LogicalTypeId::UUID,         LogicalTypeId::GEOMETRY,
	                                        LogicalTypeId::STRUCT,       LogicalTypeId::TUPLE,
	                                        LogicalTypeId::LIST,         LogicalTypeId::MAP,
	                                        LogicalTypeId::ENUM,         LogicalTypeId::UNION,
	                                        LogicalTypeId::ARRAY};
	const auto json = JSONType(factory);
	for (auto id : TARGETS) {
		// Going from JSON to another type has the same cost as going from VARCHAR to that type. The implicit cast rules
		// (cast_rules.cpp) allow VARCHAR to nothing, except to a UNION that has a member VARCHAR is castable to: the
		// cost of the ANY member, 5
		const int64_t cost = id == LogicalTypeId::UNION ? 5 : -1;
		auto cast = CastFunction::Create(factory);
		cast.SetSourceType(json).SetImplicitCastCost(cost).SetExecCallback(CastJSONToAny);
		// The nested types and the types with parameters are registered once per kind, as a wildcard target
		switch (id) {
		case LogicalTypeId::STRUCT:
			cast.SetTargetKind(cxx::CastWildcardKind::STRUCT);
			break;
		case LogicalTypeId::TUPLE:
			cast.SetTargetKind(cxx::CastWildcardKind::TUPLE);
			break;
		case LogicalTypeId::LIST:
			cast.SetTargetKind(cxx::CastWildcardKind::LIST);
			break;
		case LogicalTypeId::MAP:
			cast.SetTargetKind(cxx::CastWildcardKind::MAP);
			break;
		case LogicalTypeId::UNION:
			cast.SetTargetKind(cxx::CastWildcardKind::UNION);
			break;
		case LogicalTypeId::ARRAY:
			cast.SetTargetKind(cxx::CastWildcardKind::ARRAY);
			break;
		case LogicalTypeId::DECIMAL:
			cast.SetTargetKind(cxx::CastWildcardKind::DECIMAL);
			break;
		case LogicalTypeId::ENUM:
			cast.SetTargetKind(cxx::CastWildcardKind::ENUM);
			break;
		case LogicalTypeId::GEOMETRY:
			cast.SetTargetType(factory.ParseType("GEOMETRY"));
			break;
		default:
			cast.SetTargetType(factory.CreateType(id));
			break;
		}
		extension.Register(cast);
	}
}

void RegisterTransform(Extension &extension, Context &context) {
	auto &factory = context.GetFactory();
	RegisterTransformFunction(extension, factory, {"json_transform", "from_json"}, TransformFunction<false>);
	RegisterTransformFunction(extension, factory, {"json_transform_strict", "from_json_strict"},
	                          TransformFunction<true>);
	RegisterTransformCasts(extension, factory);
}

} // namespace json
} // namespace duckdb
