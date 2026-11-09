// The macros that depend on nothing but json_extract. Ports the JSON_MACROS table of legacy/json_extension.cpp (the
// other macros are registered by the area they belong to).

#include "json_functions.hpp"

namespace duckdb {
namespace json {

void RegisterCore(Extension &extension, Context &context) {
	auto &factory = context.GetFactory();

	// json(x) AS json_extract(x, '$')
	auto json = cxx::MacroFunction::CreateScalar(factory);
	json.SetName("json").SetBody("json_extract(x, '$')").WithSignature([&](cxx::FunctionSignature &sig) {
		sig.AddParameter("x", factory.CreateType(LogicalTypeId::ANY));
	});
	extension.Register(json);
}

} // namespace json
} // namespace duckdb
