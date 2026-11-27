// The JSON type and the simple casts around it.
// The casts that need the JSON creation / transformation machinery live with it (register_create.cpp,
// register_transform.cpp).

#include "json_functions.hpp"

namespace duckdb {
namespace json {

//===--------------------------------------------------------------------===//
// Casts
//===--------------------------------------------------------------------===//
//! JSON to VARCHAR: the same bytes. The C++ API cannot reinterpret a vector under another alias
//! (Vector::Reference needs identical types), so the strings are copied
static void CastJSONToVarchar(CastFunction::ExecInput &input) {
	const auto count = input.GetRowCount();
	auto source = input.GetInput();
	auto target = input.GetOutput();
	ValueView<varchar_t> values(source);
	StringWriter writer(target, count);
	for (idx_t i = 0; i < count; i++) {
		if (values.IsValid(i)) {
			writer.WriteValue(values.Get(i).view());
		} else {
			writer.WriteNull();
		}
	}
}

//! VARCHAR to JSON: validates the text
static void CastVarcharToJSON(CastFunction::ExecInput &input) {
	const auto count = input.GetRowCount();
	const auto try_cast = input.GetMode() == cxx::CastMode::TRY;
	auto &allocator = ThreadLocalJSONAllocator();
	allocator.Reset();
	auto alc = allocator.GetYYAlc();

	auto source = input.GetInput();
	auto target = input.GetOutput();
	ValueView<varchar_t> values(source);
	StringWriter writer(target, count);
	string first_error;
	for (idx_t i = 0; i < count; i++) {
		if (!values.IsValid(i)) {
			writer.WriteNull();
			continue;
		}
		auto value = values.Get(i);
		yyjson_read_err error;
		auto doc = JSONCommon::ReadDocumentUnsafe(value, JSONCommon::READ_FLAG, alc, &error);
		if (!doc) {
			auto message = JSONCommon::FormatParseError(value.data(), value.size(), error);
			if (!try_cast) {
				throw ConversionException(message);
			}
			if (first_error.empty()) {
				first_error = std::move(message);
			}
			writer.WriteNull();
			continue;
		}
		writer.WriteValue(value.view());
	}
	// In a try cast the failed rows are NULL, and the error tells the engine that not every row converted
	if (!first_error.empty()) {
		throw ConversionException(first_error);
	}
}

//! NULL to JSON
static void CastNullToJSON(CastFunction::ExecInput &input) {
	const auto count = input.GetRowCount();
	auto target = input.GetOutput();
	StringWriter writer(target, count);
	for (idx_t i = 0; i < count; i++) {
		writer.WriteNull();
	}
}

//! JSON[] to VARCHAR / JSON: needs a special case, otherwise the cast escapes quotes
static void CastJSONListToVarchar(CastFunction::ExecInput &input) {
	static constexpr char const *NULL_STRING = "NULL";

	const auto count = input.GetRowCount();
	auto source = input.GetInput();
	auto target = input.GetOutput();
	// The elements live in a child vector: read through the rows of the parent
	if (source.GetVectorType() != cxx::VectorType::FLAT) {
		source.Flatten();
	}
	ValueView<list_entry_t> lists(source);
	auto child = source.GetChild(0);
	ValueView<varchar_t> elements(child);
	StringWriter writer(target, count);
	string result;
	for (idx_t i = 0; i < count; i++) {
		if (!lists.IsValid(i)) {
			writer.WriteNull();
			continue;
		}
		auto entry = lists.Get(i);
		result = "[";
		for (idx_t j = 0; j < entry.length; j++) {
			if (j > 0) {
				result += ", ";
			}
			if (elements.IsValid(entry.offset + j)) {
				auto element = elements.Get(entry.offset + j);
				result.append(element.data(), element.size());
			} else {
				result += NULL_STRING;
			}
		}
		result += "]";
		writer.WriteValue(result);
	}
}

//! VARCHAR to JSON[]: needs a special case, otherwise we get a VARCHAR -> VARCHAR[] cast first
static void CastVarcharToJSONList(CastFunction::ExecInput &input) {
	const auto count = input.GetRowCount();
	const auto try_cast = input.GetMode() == cxx::CastMode::TRY;
	auto &allocator = ThreadLocalJSONAllocator();
	allocator.Reset();
	auto alc = allocator.GetYYAlc();

	auto source = input.GetInput();
	auto target = input.GetOutput();
	ValueView<varchar_t> values(source);
	ResultWriter<ListResult<string_view>> writer(target, count);
	string first_error;
	for (idx_t i = 0; i < count; i++) {
		if (!values.IsValid(i)) {
			writer.Write(std::optional<ListResult<string_view>>());
			continue;
		}
		auto value = values.Get(i);
		yyjson_read_err error;
		const auto doc = JSONCommon::ReadDocumentUnsafe(value, JSONCommon::READ_FLAG, alc, &error);
		if (!doc || !unsafe_yyjson_is_arr(doc->root)) {
			string message;
			if (!doc) {
				message = JSONCommon::FormatParseError(value.data(), value.size(), error);
			} else {
				auto truncated_input =
				    value.size() > 50 ? string(value.data(), 47) + "..." : string(value.data(), value.size());
				message = "Cannot cast to list of JSON. Input \"" + truncated_input + "\"";
			}
			if (!try_cast) {
				throw ConversionException(message);
			}
			if (first_error.empty()) {
				first_error = std::move(message);
			}
			writer.Write(std::optional<ListResult<string_view>>());
			continue;
		}
		ListResult<string_view> list;
		size_t arr_idx, max;
		yyjson_val *val;
		yyjson_arr_foreach(doc->root, arr_idx, max, val) {
			list.items.emplace_back(JSONCommon::WriteVal(val, alc));
		}
		writer.Write(std::move(list));
	}
	writer.Finish();
	// In a try cast the failed rows are NULL, and the error tells the engine that not every row converted
	if (!first_error.empty()) {
		throw ConversionException(first_error);
	}
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//
void RegisterJSONLogicalType(Extension &extension, Context &context) {
	auto &factory = context.GetFactory();
	auto type = cxx::CustomType::Create(factory);
	type.SetName("JSON").SetBaseType(factory.CreateType(LogicalTypeId::VARCHAR));
	extension.Register(type);
}

static void RegisterCast(Extension &extension, Factory &factory, const LogicalType &source, const LogicalType &target,
                         CastFunction::ExecCallback exec, int64_t implicit_cast_cost) {
	auto cast = CastFunction::Create(factory);
	cast.SetSourceType(source).SetTargetType(target).SetImplicitCastCost(implicit_cast_cost).SetExecCallback(exec);
	extension.Register(cast);
}

void RegisterSimpleCasts(Extension &extension, Context &context) {
	auto &factory = context.GetFactory();
	const auto json = JSONType(factory);
	const auto varchar = factory.CreateType(LogicalTypeId::VARCHAR);
	const auto json_list = ListOf(factory, json);

	// The costs below are relative to the implicit cast costs of the engine's own rules (cast_rules.cpp): casting to a
	// STRUCT costs 160 and to a VARCHAR 149; a cast that no rule allows implicitly has cost -1

	// JSON to VARCHAR is basically free
	RegisterCast(extension, factory, json, varchar, CastJSONToVarchar, 1);

	// VARCHAR to JSON requires a parse so it's not free. Let's make it 1 more than a cast to STRUCT
	RegisterCast(extension, factory, varchar, json, CastVarcharToJSON, 160 + 1);

	// Register NULL to JSON with a different cost than NULL to VARCHAR so the binder can disambiguate functions.
	// The C++ API cannot create the NULL type by id (CreateType(SQLNULL) is refused), so it is parsed
	RegisterCast(extension, factory, factory.ParseType("\"NULL\""), json, CastNullToJSON, 149 + 1);

	// JSON[] to VARCHAR (this needs a special case otherwise the cast will escape quotes)
	RegisterCast(extension, factory, json_list, varchar, CastJSONListToVarchar, -1 - 1);

	// JSON[] to JSON is allowed implicitly
	RegisterCast(extension, factory, json_list, json, CastJSONListToVarchar, 100);

	// VARCHAR to JSON[] (also needs a special case otherwise we get a VARCHAR -> VARCHAR[] cast first)
	RegisterCast(extension, factory, varchar, json_list, CastVarcharToJSONList, -1 - 1);
}

} // namespace json
} // namespace duckdb
