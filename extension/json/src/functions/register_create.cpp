// Creating JSON from SQL values: json_object, json_array, to_json/json_quote, array_to_json, row_to_json, the casts to
// JSON (json_create.cpp), and the json_group_array / json_group_object macros of legacy/json_extension.cpp.

#include "json_create.hpp"

namespace duckdb {
namespace json {

void RegisterCreate(Extension &extension, Context &context) {
	auto &factory = context.GetFactory();
	RegisterCreateFunctions(extension, context);

	auto any = factory.CreateType(LogicalTypeId::ANY);

	// json_group_array(x)
	auto group_array = cxx::MacroFunction::CreateScalar(factory);
	group_array.SetName("json_group_array")
	    .SetBody(
	        "CAST('[' || string_agg(CASE WHEN x IS NULL THEN 'null'::JSON ELSE to_json(x) END, ',') || ']' AS JSON)")
	    .WithSignature([&](cxx::FunctionSignature &sig) { sig.AddParameter("x", any); });
	extension.Register(group_array);

	// json_group_object(n, v)
	auto group_object = cxx::MacroFunction::CreateScalar(factory);
	group_object.SetName("json_group_object")
	    .SetBody("CAST('{' || string_agg(CASE WHEN n IS NULL THEN error('json_group_object key cannot be NULL') ELSE "
	             "to_json(n::VARCHAR) END || ':' || CASE WHEN v IS NULL THEN 'null'::JSON ELSE to_json(v) END, ',') || "
	             "'}' AS JSON)")
	    .WithSignature([&](cxx::FunctionSignature &sig) {
		    sig.AddParameter("n", any);
		    sig.AddParameter("v", any);
	    });
	extension.Register(group_object);
}

} // namespace json
} // namespace duckdb
