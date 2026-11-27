// The macros that depend on nothing but built-in functions and json_extract (the other macros are registered by the
// area they belong to).

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

	// json_copy_strftime_if_date / json_copy_strftime_if_timestamp(x, format): x formatted with strftime when it is a
	// DATE / TIMESTAMP, x as it is otherwise
	const std::pair<const char *, vector<const char *>> strftime_macros[] = {
	    {"json_copy_strftime_if_date", {"DATE"}},
	    {"json_copy_strftime_if_timestamp", {"TIMESTAMP", "TIMESTAMP_NS", "TIMESTAMPTZ", "TIMESTAMPTZ_NS"}}};
	for (auto &entry : strftime_macros) {
		auto macro = cxx::MacroFunction::CreateScalar(factory);
		macro.SetName(entry.first).SetBody("x").WithSignature([&](cxx::FunctionSignature &sig) {
			sig.AddParameter("x", factory.CreateType(LogicalTypeId::ANY));
			sig.AddParameter("format", factory.CreateType(LogicalTypeId::ANY));
		});
		for (auto type : entry.second) {
			macro.AddOverload().SetBody("strftime(x, format)").WithSignature([&](cxx::FunctionSignature &sig) {
				sig.AddParameter("x", factory.ParseType(type));
				sig.AddParameter("format", factory.CreateType(LogicalTypeId::ANY));
			});
		}
		extension.Register(macro);
	}
}

} // namespace json
} // namespace duckdb
