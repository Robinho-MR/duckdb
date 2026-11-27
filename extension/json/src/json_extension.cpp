// The entrypoint of the json extension, built both statically into DuckDB and as a loadable extension.
// Everything is registered by the files in functions/, one registration function per area of the extension.

#include "duckdb_cpp_extension.hpp"

#include "json_functions.hpp"

DUCKDB_CPP_EXTENSION_ENTRYPOINT(duckdb::cxx::Extension &extension, duckdb::cxx::Context &context) {
	using namespace duckdb::json;

	// The type and the casts first, the functions refer to them
	RegisterJSONLogicalType(extension, context);
	RegisterSimpleCasts(extension, context);

	RegisterCore(extension, context);
	RegisterScalarRead(extension, context);
	RegisterScalarModify(extension, context);
	RegisterCreate(extension, context);
	RegisterTransform(extension, context);
	RegisterStructure(extension, context);
	RegisterReader(extension, context);
	RegisterTableInOut(extension, context);
	RegisterCopy(extension, context);
}
