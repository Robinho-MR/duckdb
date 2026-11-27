#include "json_functions.hpp"

namespace duckdb {
namespace json {

using JSONPathType = JSONCommon::JSONPathType;

//===--------------------------------------------------------------------===//
// Types
//===--------------------------------------------------------------------===//
LogicalType JSONType(Factory &factory) {
	return factory.CreateType(LogicalTypeId::VARCHAR).WithAlias(factory, "JSON");
}

LogicalType ListOf(Factory &factory, const LogicalType &child) {
	vector<cxx::TypeParam> params;
	params.emplace_back(factory.CreateValue(child));
	return factory.CreateType(LogicalTypeId::LIST, params);
}

bool IsJSONType(const LogicalType &type) {
	return type.GetTypeId() == LogicalTypeId::VARCHAR && type.GetName() == "JSON";
}

//===--------------------------------------------------------------------===//
// Per-thread state
//===--------------------------------------------------------------------===//
void JSONFunctionLocalState::Init(ScalarFunction::InitInput &input) {
	input.SetInitData<JSONFunctionLocalState>();
}

JSONFunctionLocalState &JSONFunctionLocalState::ResetAndGet(ScalarFunction::ExecInput &input) {
	auto &lstate = input.GetInitData<JSONFunctionLocalState>();
	lstate.json_allocator.Reset();
	return lstate;
}

JSONAllocator &ThreadLocalJSONAllocator() {
	static thread_local JSONAllocator allocator;
	return allocator;
}

//===--------------------------------------------------------------------===//
// Bind data
//===--------------------------------------------------------------------===//
static bool IsIntegralType(LogicalTypeId id) {
	switch (id) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
		return true;
	default:
		return false;
	}
}

JSONPathType JSONReadFunctionData::CheckPath(Factory &factory, const Value &path_val, string &path, idx_t &len) {
	if (path_val.IsNull()) {
		throw BinderException("JSON path cannot be NULL");
	}
	const auto integral = IsIntegralType(path_val.GetLogicalType().GetTypeId());
	string path_str;
	if (integral) {
		path_str = std::to_string(path_val.Get<int64_t>());
	} else if (path_val.GetLogicalType().GetTypeId() == LogicalTypeId::VARCHAR) {
		path_str = string(path_val.Get<varchar_t>().view());
	} else {
		auto casted = path_val.Cast(factory, factory.CreateType(LogicalTypeId::VARCHAR));
		path_str = string(casted.Get<varchar_t>().view());
	}
	len = path_str.size();
	const auto ptr = path_str.c_str();
	JSONPathType path_type = JSONPathType::REGULAR;
	// Copy over string to the bind data
	if (len != 0) {
		if (*ptr == '/' || *ptr == '$') {
			path = path_str;
		} else if (integral) {
			path = "$[" + path_str + "]";
		} else if (memchr(ptr, '"', len)) {
			path = "/" + path_str;
		} else {
			path = "$.\"" + path_str + "\"";
		}
		len = path.length();
		if (*path.c_str() == '$') {
			path_type = JSONCommon::ValidatePath(path.c_str(), len, true);
		}
	}
	return path_type;
}

JSONReadFunctionData::JSONReadFunctionData(bool constant, string path_p, idx_t len, JSONPathType path_type_p,
                                           bool integral)
    : constant(constant), path(std::move(path_p)), path_type(path_type_p), ptr(path.c_str()), len(len),
      integral(integral) {
	if (constant && len != 0 && path[0] == '$' && path_type == JSONPathType::REGULAR) {
		// The path was validated in CheckPath, parse it once so execution can skip tokenizing it per row
		elements = JSONCommon::ParsePathElements(ptr, len, false);
		use_elements = true;
	}
}

bool JSONReadFunctionData::operator==(const JSONReadFunctionData &other) const {
	return constant == other.constant && path == other.path && len == other.len && path_type == other.path_type &&
	       integral == other.integral;
}

void JSONReadFunctionData::ResolveTypes(ScalarFunction::ResolveTypesInput &input) {
	auto &factory = input.GetContext().GetFactory();
	auto &user_data = input.GetUserData<JSONReadUserData>();
	// An integral path is an array index, other paths are cast to VARCHAR
	const auto integral = IsIntegralType(input.GetArgType(1).GetTypeId());
	input.SetArgType(1, factory.CreateType(integral ? LogicalTypeId::BIGINT : LogicalTypeId::VARCHAR));
	auto path_val = input.TryGetConstantArgument(1);
	if (path_val && !path_val->IsNull()) {
		string path;
		idx_t len = 0;
		if (CheckPath(factory, *path_val, path, len) == JSONPathType::WILDCARD) {
			input.SetReturnType(ListOf(factory, user_data.result));
			return;
		}
	}
	input.SetReturnType(user_data.result.Copy());
}

void JSONReadFunctionData::Bind(ScalarFunction::BindInput &input) {
	auto &factory = input.GetContext().GetFactory();
	bool constant = false;
	string path;
	idx_t len = 0;
	JSONPathType path_type = JSONPathType::REGULAR;
	auto path_val = input.TryGetConstantArgument(1);
	if (path_val && !path_val->IsNull()) {
		constant = true;
		path_type = CheckPath(factory, *path_val, path, len);
	}
	input.SetBindData<JSONReadFunctionData>(constant, std::move(path), len, path_type,
	                                        IsIntegralType(input.GetArgType(1).GetTypeId()));
}

JSONReadManyFunctionData::JSONReadManyFunctionData(vector<string> paths_p, vector<idx_t> lens_p)
    : paths(std::move(paths_p)), lens(std::move(lens_p)) {
	for (const auto &path : paths) {
		ptrs.push_back(path.c_str());
	}
	for (idx_t i = 0; i < paths.size(); i++) {
		if (lens[i] != 0 && paths[i][0] == '$') {
			// The paths were validated in CheckPath, parse them once
			elements.push_back(JSONCommon::ParsePathElements(ptrs[i], lens[i], false));
			use_elements.push_back(true);
		} else {
			elements.emplace_back();
			use_elements.push_back(false);
		}
	}
}

bool JSONReadManyFunctionData::operator==(const JSONReadManyFunctionData &other) const {
	return paths == other.paths && lens == other.lens;
}

void JSONReadManyFunctionData::Bind(ScalarFunction::BindInput &input) {
	auto &factory = input.GetContext().GetFactory();
	vector<string> paths;
	vector<idx_t> lens;
	auto paths_val = input.TryGetConstantArgument(1);
	if (!paths_val) {
		throw BinderException("JSON paths must be a constant list");
	}
	if (paths_val->IsNull()) {
		throw BinderException("JSON path cannot be NULL");
	}

	for (idx_t i = 0; i < paths_val->GetChildCount(); i++) {
		auto path_val = paths_val->GetChild(i);
		paths.emplace_back("");
		lens.push_back(0);
		if (JSONReadFunctionData::CheckPath(factory, path_val, paths.back(), lens.back()) == JSONPathType::WILDCARD) {
			throw BinderException("Cannot have wildcards in JSON path when supplying multiple paths");
		}
	}

	input.SetBindData<JSONReadManyFunctionData>(std::move(paths), std::move(lens));
}

//===--------------------------------------------------------------------===//
// Registering read functions
//===--------------------------------------------------------------------===//
void RegisterJSONReadFunction(Extension &extension, Context &context, const vector<string> &names, JSONReadKind kind,
                              const LogicalType &input, const LogicalType &result, ScalarFunction::ExecCallback exec,
                              FunctionFallibility fallibility) {
	auto &factory = context.GetFactory();
	auto function = ScalarFunction::Create(factory);
	function.WithSignature([&](cxx::FunctionSignature &sig) {
		sig.AddParameter("json", input);
		switch (kind) {
		case JSONReadKind::UNARY:
			sig.SetReturnType(result);
			break;
		case JSONReadKind::PATH:
			sig.AddParameter("path", factory.CreateType(LogicalTypeId::VARCHAR));
			// A wildcard path returns a list of the result: ResolveTypes resolves it
			sig.SetReturnType(result);
			break;
		case JSONReadKind::INDEX:
			sig.AddParameter("index", factory.CreateType(LogicalTypeId::BIGINT));
			sig.SetReturnType(result);
			break;
		case JSONReadKind::MANY:
			sig.AddParameter("path", ListOf(factory, factory.CreateType(LogicalTypeId::VARCHAR)));
			sig.SetReturnType(ListOf(factory, result));
			break;
		}
	});
	function.SetExecCallback(exec);
	function.SetInitCallback(JSONFunctionLocalState::Init);
	function.SetFallibility(fallibility);
	switch (kind) {
	case JSONReadKind::UNARY:
		break;
	case JSONReadKind::PATH:
		function.SetResolveTypesCallback(JSONReadFunctionData::ResolveTypes);
		function.SetBindCallback(JSONReadFunctionData::Bind);
		break;
	case JSONReadKind::INDEX:
		function.SetBindCallback(JSONReadFunctionData::Bind);
		break;
	case JSONReadKind::MANY:
		function.SetBindCallback(JSONReadManyFunctionData::Bind);
		break;
	}
	for (auto &name : names) {
		function.SetName(name);
		if (kind == JSONReadKind::PATH) {
			// User data is consumed by registration
			function.SetUserData<JSONReadUserData>(result.Copy());
		}
		extension.Register(function);
	}
}

void RegisterJSONReadFunctions(Extension &extension, Context &context, const vector<string> &names,
                               const LogicalType &result, ScalarFunction::ExecCallback unary,
                               ScalarFunction::ExecCallback path, ScalarFunction::ExecCallback many, bool index,
                               bool infallible_on_json) {
	auto &factory = context.GetFactory();
	for (const bool json_input : {false, true}) {
		auto input = json_input ? JSONType(factory) : factory.CreateType(LogicalTypeId::VARCHAR);
		const auto cannot_fail = json_input && infallible_on_json;
		const auto no_fail = cannot_fail ? FunctionFallibility::INFALLIBLE : FunctionFallibility::FALLIBLE;
		if (unary) {
			RegisterJSONReadFunction(extension, context, names, JSONReadKind::UNARY, input, result, unary, no_fail);
		}
		if (index) {
			RegisterJSONReadFunction(extension, context, names, JSONReadKind::INDEX, input, result, path, no_fail);
		}
		if (path) {
			RegisterJSONReadFunction(extension, context, names, JSONReadKind::PATH, input, result, path,
			                         FunctionFallibility::FALLIBLE);
		}
		if (many) {
			RegisterJSONReadFunction(extension, context, names, JSONReadKind::MANY, input, result, many,
			                         FunctionFallibility::FALLIBLE);
		}
	}
}

} // namespace json
} // namespace duckdb
