// json_valid

#include "json_executors.hpp"

namespace duckdb {
namespace json {

static void ValidFunction(ScalarFunction::ExecInput &input) {
	auto &lstate = JSONFunctionLocalState::ResetAndGet(input);
	auto alc = lstate.json_allocator.GetYYAlc();
	const auto count = input.GetRowCount();

	auto result = input.GetResult();
	ValueView<varchar_t> inputs(input.GetArg(0));
	FlatWriter<bool> writer(result, count);
	for (idx_t i = 0; i < count; i++) {
		if (!inputs.IsValid(i)) {
			writer.WriteNull();
			continue;
		}
		writer.WriteValue(JSONCommon::ReadDocumentUnsafe(inputs.Get(i), JSONCommon::READ_FLAG, alc) != nullptr);
	}
}

void RegisterJSONValid(Extension &extension, Context &context) {
	auto &factory = context.GetFactory();
	for (const bool json_input : {false, true}) {
		auto input = json_input ? JSONType(factory) : factory.CreateType(LogicalTypeId::VARCHAR);
		RegisterJSONReadFunction(extension, context, {"json_valid"}, JSONReadKind::UNARY, input,
		                         factory.CreateType(LogicalTypeId::BOOLEAN), ValidFunction,
		                         FunctionFallibility::INFALLIBLE);
	}
}

} // namespace json
} // namespace duckdb
