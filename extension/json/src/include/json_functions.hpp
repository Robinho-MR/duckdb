//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_functions.hpp
//
// The state the JSON scalar functions share (bind data of a constant path, the per-thread arena) and the helpers that
// register the common shapes of a JSON read function. The registration entry points of every area are declared here.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "json_allocator.hpp"
#include "json_common.hpp"
#include "json_vector.hpp"

namespace duckdb {
namespace json {

using cxx::AggregateFunction;
using cxx::CastFunction;
using cxx::FunctionFallibility;
using cxx::ScalarFunction;

//===--------------------------------------------------------------------===//
// Types
//===--------------------------------------------------------------------===//
//! The JSON type: VARCHAR carrying the alias JSON, the same type as the engine's LogicalType::JSON()
LogicalType JSONType(Factory &factory);
//! LIST(child)
LogicalType ListOf(Factory &factory, const LogicalType &child);
//! Whether the type is the JSON type
bool IsJSONType(const LogicalType &type);

//===--------------------------------------------------------------------===//
// Per-thread state
//===--------------------------------------------------------------------===//
//! The state of a scalar function on one execution thread, created by the init callback (set it with
//! `function.SetInitCallback(JSONFunctionLocalState::Init)`). Holds the arena yyjson allocates from.
struct JSONFunctionLocalState {
public:
	static void Init(ScalarFunction::InitInput &input);
	//! Gets the state and frees what the previous call allocated: call it once at the start of every exec callback
	static JSONFunctionLocalState &ResetAndGet(ScalarFunction::ExecInput &input);

public:
	JSONAllocator json_allocator;
};

//! The arena of the current thread, for code that has no local state (the cast callbacks). Call Reset() on it at the
//! start of every callback.
JSONAllocator &ThreadLocalJSONAllocator();

//===--------------------------------------------------------------------===//
// Bind data
//===--------------------------------------------------------------------===//
//! The path argument of a function like json_extract(json, path), as far as it is known at bind time
struct JSONReadFunctionData {
public:
	using JSONPathType = JSONCommon::JSONPathType;

	JSONReadFunctionData(bool constant, string path, idx_t len, JSONPathType path_type, bool integral);
	JSONReadFunctionData(const JSONReadFunctionData &) = delete;
	JSONReadFunctionData &operator=(const JSONReadFunctionData &) = delete;

	bool operator==(const JSONReadFunctionData &other) const;

	//! Turns a constant path argument (a string, or an integer: an array index) into the path to evaluate. Throws a
	//! BinderException on malformed paths.
	static JSONPathType CheckPath(Factory &factory, const Value &path_val, string &path, idx_t &len);
	//! Resolve types callback of the (json, path) overloads, which return LIST(T) if the path has a wildcard
	static void ResolveTypes(ScalarFunction::ResolveTypesInput &input);
	//! Bind callback of the (json, path) and (json, index) overloads
	static void Bind(ScalarFunction::BindInput &input);

public:
	//! Whether the path is a constant
	const bool constant;
	const string path;
	const JSONPathType path_type;
	const char *ptr;
	const idx_t len;
	//! Whether the path argument is an integer (an array index)
	const bool integral;
	//! Path elements parsed once at bind time (constant regular '$' paths only)
	vector<JSONPathElement> elements;
	bool use_elements = false;
};

//! The user data of the (json, path) overloads: what they return when the path has no wildcard
struct JSONReadUserData {
	explicit JSONReadUserData(LogicalType result_p) : result(std::move(result_p)) {
	}
	LogicalType result;
};

//! The path list argument of a function like json_extract(json, [path, ...])
struct JSONReadManyFunctionData {
public:
	JSONReadManyFunctionData(vector<string> paths, vector<idx_t> lens);
	JSONReadManyFunctionData(const JSONReadManyFunctionData &) = delete;
	JSONReadManyFunctionData &operator=(const JSONReadManyFunctionData &) = delete;

	bool operator==(const JSONReadManyFunctionData &other) const;

	static void Bind(ScalarFunction::BindInput &input);

public:
	const vector<string> paths;
	vector<const char *> ptrs;
	const vector<idx_t> lens;
	//! Per-path elements parsed once at bind time (regular '$' paths only)
	vector<vector<JSONPathElement>> elements;
	vector<bool> use_elements;
};

//===--------------------------------------------------------------------===//
// Registering read functions
//===--------------------------------------------------------------------===//
//! The shapes of a JSON read function
enum class JSONReadKind : uint8_t {
	//! f(json)
	UNARY,
	//! f(json, path VARCHAR): returns LIST(result) if the (constant) path has a wildcard
	PATH,
	//! f(json, index BIGINT): the array element
	INDEX,
	//! f(json, paths VARCHAR[]): returns LIST(result), one element per path
	MANY
};

//! Registers one overload of a read function under every name in `names`. `result` is what the function returns for
//! one value (for MANY and for wildcard paths the list of it is derived). The callback is an exec callback that uses
//! JSONExecutors (json_executors.hpp).
void RegisterJSONReadFunction(Extension &extension, Context &context, const vector<string> &names, JSONReadKind kind,
                              const LogicalType &input, const LogicalType &result, ScalarFunction::ExecCallback exec,
                              FunctionFallibility fallibility);

//! Registers the overloads of a read function for both VARCHAR and JSON input. `index` also adds the (json, index)
//! overload.
void RegisterJSONReadFunctions(Extension &extension, Context &context, const vector<string> &names,
                               const LogicalType &result, ScalarFunction::ExecCallback unary,
                               ScalarFunction::ExecCallback path, ScalarFunction::ExecCallback many, bool index,
                               bool infallible_on_json);

//===--------------------------------------------------------------------===//
// Registration of the areas of the extension. Each is defined by (and only by) the file named in the comment.
//===--------------------------------------------------------------------===//
//! json_types.cpp: the JSON type, and the casts between VARCHAR, JSON and lists of JSON
void RegisterJSONLogicalType(Extension &extension, Context &context);
void RegisterSimpleCasts(Extension &extension, Context &context);
//! functions/register_core.cpp: the macros that need nothing else
void RegisterCore(Extension &extension, Context &context);
//! functions/register_scalar_read.cpp
void RegisterScalarRead(Extension &extension, Context &context);
//! functions/register_scalar_modify.cpp
void RegisterScalarModify(Extension &extension, Context &context);
//! functions/register_create.cpp
void RegisterCreate(Extension &extension, Context &context);
//! functions/register_transform.cpp
void RegisterTransform(Extension &extension, Context &context);
//! functions/register_structure.cpp
void RegisterStructure(Extension &extension, Context &context);
//! functions/register_reader.cpp
void RegisterReader(Extension &extension, Context &context);
//! functions/register_table_inout.cpp
void RegisterTableInOut(Extension &extension, Context &context);
//! functions/register_copy.cpp
void RegisterCopy(Extension &extension, Context &context);

//! Individual read functions (functions/<name>.cpp), registered by RegisterScalarRead
void RegisterJSONTypeFunction(Extension &extension, Context &context);
void RegisterJSONValid(Extension &extension, Context &context);
void RegisterJSONExists(Extension &extension, Context &context);
void RegisterJSONExtract(Extension &extension, Context &context);
void RegisterJSONValue(Extension &extension, Context &context);
void RegisterJSONArrayLength(Extension &extension, Context &context);

} // namespace json
} // namespace duckdb
