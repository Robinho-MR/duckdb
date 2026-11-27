// json_contains

#include "json_executors.hpp"

namespace duckdb {
namespace json {

static inline bool JSONContains(yyjson_val *haystack, yyjson_val *needle);
static inline bool JSONFuzzyEquals(yyjson_val *haystack, yyjson_val *needle);

static inline bool JSONArrayFuzzyEquals(yyjson_val *haystack, yyjson_val *needle) {
	D_ASSERT(yyjson_get_tag(haystack) == (YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE) &&
	         yyjson_get_tag(needle) == (YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE));

	size_t needle_idx, needle_max, haystack_idx, haystack_max;
	yyjson_val *needle_child, *haystack_child;
	yyjson_arr_foreach(needle, needle_idx, needle_max, needle_child) {
		bool found = false;
		yyjson_arr_foreach(haystack, haystack_idx, haystack_max, haystack_child) {
			if (JSONFuzzyEquals(haystack_child, needle_child)) {
				found = true;
				break;
			}
		}
		if (!found) {
			return false;
		}
	}
	return true;
}

static inline bool JSONObjectFuzzyEquals(yyjson_val *haystack, yyjson_val *needle) {
	D_ASSERT(yyjson_get_tag(haystack) == (YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE) &&
	         yyjson_get_tag(needle) == (YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE));

	size_t idx, max;
	yyjson_val *key, *needle_child;
	yyjson_obj_foreach(needle, idx, max, key, needle_child) {
		auto haystack_child = yyjson_obj_getn(haystack, unsafe_yyjson_get_str(key), unsafe_yyjson_get_len(key));
		if (!haystack_child || !JSONFuzzyEquals(haystack_child, needle_child)) {
			return false;
		}
	}
	return true;
}

static inline bool JSONFuzzyEquals(yyjson_val *haystack, yyjson_val *needle) {
	D_ASSERT(haystack && needle);

	// Strict equality
	if (unsafe_yyjson_equals(haystack, needle)) {
		return true;
	}

	auto haystack_tag = yyjson_get_tag(needle);
	if (haystack_tag != yyjson_get_tag(haystack)) {
		return false;
	}

	// Fuzzy equality (contained in)
	switch (haystack_tag) {
	case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE:
		return JSONArrayFuzzyEquals(haystack, needle);
	case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE:
		return JSONObjectFuzzyEquals(haystack, needle);
	default:
		return false;
	}
}

static inline bool JSONArrayContains(yyjson_val *haystack_array, yyjson_val *needle) {
	D_ASSERT(yyjson_get_tag(haystack_array) == (YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE));

	size_t idx, max;
	yyjson_val *child_haystack;
	yyjson_arr_foreach(haystack_array, idx, max, child_haystack) {
		if (JSONContains(child_haystack, needle)) {
			return true;
		}
	}
	return false;
}

static inline bool JSONObjectContains(yyjson_val *haystack_object, yyjson_val *needle) {
	D_ASSERT(yyjson_get_tag(haystack_object) == (YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE));

	size_t idx, max;
	yyjson_val *key, *child_haystack;
	yyjson_obj_foreach(haystack_object, idx, max, key, child_haystack) {
		if (JSONContains(child_haystack, needle)) {
			return true;
		}
	}
	return false;
}

static inline bool JSONContains(yyjson_val *haystack, yyjson_val *needle) {
	if (JSONFuzzyEquals(haystack, needle)) {
		return true;
	}

	switch (yyjson_get_tag(haystack)) {
	case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE:
		return JSONArrayContains(haystack, needle);
	case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE:
		return JSONObjectContains(haystack, needle);
	default:
		return false;
	}
}

static void JSONContainsFunction(ScalarFunction::ExecInput &input) {
	D_ASSERT(input.GetArgCount() == 2);
	auto &lstate = JSONFunctionLocalState::ResetAndGet(input);
	auto alc = lstate.json_allocator.GetYYAlc();
	const auto count = input.GetRowCount();

	auto result = input.GetResult();
	auto needle_vector = input.GetArg(1);
	ValueView<varchar_t> haystacks(input.GetArg(0));
	ValueView<varchar_t> needles(needle_vector);
	ResultWriter<bool> writer(result, count);

	if (needle_vector.GetVectorType() == cxx::VectorType::CONSTANT) {
		if (!needles.IsValid(0)) {
			throw InternalException("JSON Contains called with constant NULL needles");
		}
		auto needle_doc = JSONCommon::ReadDocument(needles.Get(0), JSONCommon::READ_FLAG, alc);
		for (idx_t i = 0; i < count; i++) {
			if (!haystacks.IsValid(i)) {
				writer.Write(std::nullopt);
				continue;
			}
			auto haystack_doc = JSONCommon::ReadDocument(haystacks.Get(i), JSONCommon::READ_FLAG, alc);
			writer.Write(JSONContains(haystack_doc->root, needle_doc->root));
		}
	} else {
		for (idx_t i = 0; i < count; i++) {
			if (!haystacks.IsValid(i) || !needles.IsValid(i)) {
				writer.Write(std::nullopt);
				continue;
			}
			auto needle_doc = JSONCommon::ReadDocument(needles.Get(i), JSONCommon::READ_FLAG, alc);
			auto haystack_doc = JSONCommon::ReadDocument(haystacks.Get(i), JSONCommon::READ_FLAG, alc);
			writer.Write(JSONContains(haystack_doc->root, needle_doc->root));
		}
	}
	writer.Finish();
}

static void RegisterContainsOverload(Extension &extension, Context &context, const LogicalType &lhs,
                                     const LogicalType &rhs, FunctionFallibility fallibility) {
	auto &factory = context.GetFactory();
	auto function = ScalarFunction::Create(factory);
	function.SetName("json_contains");
	function.WithSignature([&](cxx::FunctionSignature &sig) {
		sig.AddParameter("haystack", lhs);
		sig.AddParameter("needle", rhs);
		sig.SetReturnType(factory.CreateType(LogicalTypeId::BOOLEAN));
	});
	function.SetExecCallback(JSONContainsFunction);
	function.SetInitCallback(JSONFunctionLocalState::Init);
	function.SetFallibility(fallibility);
	extension.Register(function);
}

void RegisterJSONContains(Extension &extension, Context &context) {
	auto &factory = context.GetFactory();
	auto varchar = factory.CreateType(LogicalTypeId::VARCHAR);
	auto json = JSONType(factory);
	RegisterContainsOverload(extension, context, varchar, varchar, FunctionFallibility::FALLIBLE);
	RegisterContainsOverload(extension, context, varchar, json, FunctionFallibility::FALLIBLE);
	RegisterContainsOverload(extension, context, json, varchar, FunctionFallibility::FALLIBLE);
	RegisterContainsOverload(extension, context, json, json, FunctionFallibility::INFALLIBLE);
}

} // namespace json
} // namespace duckdb
