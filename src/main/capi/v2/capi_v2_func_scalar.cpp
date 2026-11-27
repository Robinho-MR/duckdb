#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/main/capi_v2/capi_v2_function_internal.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb::capiv2 {

class CV2FunctionData final : public FunctionData {
public:
	// The bind data set by the user
	shared_ptr<CV2UserData> handle;

	auto Copy() const -> unique_ptr<FunctionData> override {
		auto copy = make_uniq<CV2FunctionData>();
		copy->handle = handle;
		return std::move(copy);
	}

	auto Equals(const FunctionData &other) const -> bool override {
		const auto &other_data = other.Cast<CV2FunctionData>();
		return handle && other_data.handle && handle->Equals(*other_data.handle);
	}
};

class CV2FunctionLocalState final : public FunctionLocalState {
public:
	CV2UserData handle;
};

static constexpr const char *SCALAR_BIND_DATA_ERROR =
    "Scalar functions set the bind data through duckdb_v2_scalar_function_bind_set_bind_data";

//! The arguments of a scalar call before they are cast to the parameter types - the bind data cannot be set
class CV2ResolveTypesBindInfo final : public CV2FunctionBindInfo {
public:
	CV2ResolveTypesBindInfo(const BoundScalarFunction &function, void *user_data,
	                        const ResolveScalarFunctionTypesInput &input)
	    : CV2FunctionBindInfo(function, user_data), input(input) {
		D_ASSERT(GetArgCount() == input.GetArgumentCount());
		bind_data_error = SCALAR_BIND_DATA_ERROR;
	}

	LogicalType GetArgType(idx_t index) const override {
		return input.GetArgumentType(index);
	}
	Value GetArgValue(idx_t index) const override {
		return input.GetConstant(index);
	}

private:
	const ResolveScalarFunctionTypesInput &input;
};

class CV2BindInfo {
public:
	//! The bind data the callback set, or nullptr
	shared_ptr<CV2UserData> out_bind_data;
	//! The stability the callback set for this call, if any
	optional<FunctionStability> stability;
	//! The monotonicity the callback set for this call, per argument
	vector<pair<idx_t, Monotonicity>> monotonicity;
	//! The function the callback replaced the call with, if any
	optional<string> replacement_name;
	vector<idx_t> replacement_args;
	//! Whether the callback replaced the call with the overload the function overrode
	bool call_overridden = false;
};

static auto Convert(duckdb_v2_scalar_function_bind_info_handle info) -> CV2BindInfo * {
	return reinterpret_cast<CV2BindInfo *>(info);
}
static auto Convert(CV2BindInfo *info) -> duckdb_v2_scalar_function_bind_info_handle {
	return reinterpret_cast<duckdb_v2_scalar_function_bind_info_handle>(info);
}

class CV2ResolveTypesInfo {
public:
	explicit CV2ResolveTypesInfo(BoundScalarFunction &bound_function) : bound_function(bound_function) {
	}

	BoundScalarFunction &bound_function;
};

static auto Convert(duckdb_v2_scalar_function_resolve_types_info_handle info) -> CV2ResolveTypesInfo * {
	return reinterpret_cast<CV2ResolveTypesInfo *>(info);
}
static auto Convert(CV2ResolveTypesInfo *info) -> duckdb_v2_scalar_function_resolve_types_info_handle {
	return reinterpret_cast<duckdb_v2_scalar_function_resolve_types_info_handle>(info);
}

class CV2InitInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;

	duckdb_v2_opaque out_init_data = {};
};

static auto Convert(duckdb_v2_scalar_function_init_info_handle info) -> CV2InitInfo * {
	return reinterpret_cast<CV2InitInfo *>(info);
}
static auto Convert(CV2InitInfo *info) -> duckdb_v2_scalar_function_init_info_handle {
	return reinterpret_cast<duckdb_v2_scalar_function_init_info_handle>(info);
}

class CV2ExecInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;
	void *in_init_data = nullptr;

	DataChunk *input;
	//! Tells the four parts of the argument list apart
	const BoundScalarFunction *function = nullptr;
	Vector *result;
};

static auto Convert(duckdb_v2_scalar_function_exec_info_handle info) -> CV2ExecInfo * {
	return reinterpret_cast<CV2ExecInfo *>(info);
}
static auto Convert(CV2ExecInfo *info) -> duckdb_v2_scalar_function_exec_info_handle {
	return reinterpret_cast<duckdb_v2_scalar_function_exec_info_handle>(info);
}

class CV2ScalarStatsInfo {
public:
	CV2ScalarStatsInfo(const BoundScalarFunction &function, BaseStatistics &result)
	    : function(function), result_stats(CV2Stats::Writable(result)) {
	}

	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;

	//! Tells the four parts of the argument list apart
	const BoundScalarFunction &function;
	vector<CV2Stats> arg_stats;
	CV2Stats result_stats;
};

static auto Convert(duckdb_v2_scalar_function_stats_info_handle info) -> CV2ScalarStatsInfo * {
	return reinterpret_cast<CV2ScalarStatsInfo *>(info);
}
static auto Convert(CV2ScalarStatsInfo *info) -> duckdb_v2_scalar_function_stats_info_handle {
	return reinterpret_cast<duckdb_v2_scalar_function_stats_info_handle>(info);
}

class CV2ScalarFunctionInfo : public ScalarFunctionInfo {
public:
	duckdb_v2_scalar_function_resolve_types_callback_fn resolve_types_cb = nullptr;
	duckdb_v2_scalar_function_bind_callback_fn bind_cb = nullptr;
	duckdb_v2_scalar_function_init_callback_fn init_cb = nullptr;
	duckdb_v2_scalar_function_exec_callback_fn exec_cb = nullptr;
	duckdb_v2_scalar_function_stats_callback_fn stats_cb = nullptr;
	shared_ptr<CV2UserData> user_data = nullptr;
	//! The schema the function is registered in, where a replacement call is looked up
	Identifier schema = Identifier::DefaultSchema();
	//! The overload the function replaced when it was registered, if any
	shared_ptr<const ScalarFunction> overridden;
};

static auto CV2ScalarResolveTypes(ResolveScalarFunctionTypesInput &input) -> void {
	auto &bound_function = input.GetBoundFunction();
	const auto &info = bound_function.GetExtraFunctionInfo().Cast<CV2ScalarFunctionInfo>();

	CV2ResolveTypesBindInfo bind_info(bound_function, info.user_data ? info.user_data->GetData() : nullptr, input);
	CV2ResolveTypesInfo result_info(bound_function);

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(input.GetClientContext());
	info.resolve_types_cb(Convert(&bind_info), Convert(&result_info), Convert(&callback_context), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}
}

//! Moves a bound replacement call into the call being bound
static auto CV2ScalarTakeReplacement(BindScalarFunctionInput &input, unique_ptr<Expression> expression)
    -> unique_ptr<FunctionData> {
	if (expression->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		throw InternalException("The replacement call of a scalar function did not bind to a function call");
	}
	auto &replacement = expression->Cast<BoundFunctionExpression>();
	input.GetBoundFunction() = std::move(replacement.FunctionMutable());
	input.GetArguments() = std::move(replacement.GetChildrenMutable());
	return replacement.BindInfo() ? replacement.BindInfo()->Copy() : nullptr;
}

//! Binds the overload the function overrode to the arguments of the call
static auto CV2ScalarCallOverridden(BindScalarFunctionInput &input, const shared_ptr<const ScalarFunction> &overridden)
    -> unique_ptr<FunctionData> {
	if (!overridden) {
		throw InvalidInputException("The scalar function did not override an overload when it was registered");
	}
	vector<unique_ptr<Expression>> children;
	for (auto &argument : input.GetArguments()) {
		children.push_back(argument->Copy());
	}
	auto binder = input.NestedFunctionBinder();
	return CV2ScalarTakeReplacement(input, binder.BindScalarFunction(overridden, std::move(children)));
}

//! Binds the replacement call and moves it into the call being bound
static auto CV2ScalarReplaceCall(BindScalarFunctionInput &input, const Identifier &schema, const string &name,
                                 const vector<idx_t> &arg_indices) -> unique_ptr<FunctionData> {
	auto &context = input.GetClientContext();
	auto &arguments = input.GetArguments();
	vector<unique_ptr<Expression>> children;
	for (auto index : arg_indices) {
		if (index >= arguments.size()) {
			throw InvalidInputException("Argument index %llu of the replacement call is out of range", index);
		}
		children.push_back(arguments[index]->Copy());
	}
	auto &catalog = Catalog::GetSystemCatalog(context);
	QualifiedName qualified_name(catalog.GetName(), schema, Identifier(name));
	auto &entry = catalog.GetEntry<ScalarFunctionCatalogEntry>(context, qualified_name);
	auto binder = input.NestedFunctionBinder();
	ErrorData error;
	auto expression = binder.BindScalarFunction(entry, std::move(children), error);
	if (!expression) {
		error.Throw();
	}
	return CV2ScalarTakeReplacement(input, std::move(expression));
}

static auto CV2ScalarBind(BindScalarFunctionInput &input) -> unique_ptr<FunctionData> {
	const auto &info = input.GetBoundFunction().GetExtraFunctionInfo().Cast<CV2ScalarFunctionInfo>();

	auto &bound_function = input.GetBoundFunction();
	CV2ExpressionBindInfo bind_info(bound_function, info.user_data ? info.user_data->GetData() : nullptr, input);
	bind_info.bind_data_error = SCALAR_BIND_DATA_ERROR;
	CV2BindInfo result_info;

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(input.GetClientContext());
	info.bind_cb(Convert(&bind_info), Convert(&result_info), Convert(&callback_context), &err_ptr);

	unique_ptr<FunctionData> result = nullptr;
	if (result_info.out_bind_data) {
		auto set_result = make_uniq<CV2FunctionData>();
		set_result->handle = std::move(result_info.out_bind_data);
		result = std::move(set_result);
	}

	if (err.HasError()) {
		err.ThrowAsException();
	}

	if (result_info.stability) {
		bound_function.SetStability(*result_info.stability);
	}
	for (auto &entry : result_info.monotonicity) {
		auto props = bound_function.GetArgProperties(entry.first);
		props.monotonicity = entry.second;
		bound_function.SetArgProperties(entry.first, props);
	}
	if (result_info.call_overridden) {
		return CV2ScalarCallOverridden(input, info.overridden);
	}
	if (result_info.replacement_name) {
		return CV2ScalarReplaceCall(input, info.schema, *result_info.replacement_name, result_info.replacement_args);
	}

	return result;
}

static auto CV2ScalarInit(ExpressionState &state, const BoundFunctionExpression &expr,
                          optional_ptr<const FunctionData> bind_data) -> unique_ptr<FunctionLocalState> {
	const auto &info = expr.Function().GetExtraFunctionInfo().Cast<CV2ScalarFunctionInfo>();

	auto user_bind_data = bind_data ? bind_data->Cast<CV2FunctionData>().handle : nullptr;

	CV2InitInfo args = {};
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	args.in_bind_data = user_bind_data ? user_bind_data->GetData() : nullptr;

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(state.GetClientContext());
	info.init_cb(Convert(&args), Convert(&callback_context), &err_ptr);

	unique_ptr<FunctionLocalState> result = nullptr;
	if (args.out_init_data.ptr) {
		auto set_result = make_uniq<CV2FunctionLocalState>();
		set_result->handle = CV2UserData(args.out_init_data.ptr, args.out_init_data.destroy, args.out_init_data.equals);
		result = std::move(set_result);
	}

	// Throw after setting the state, so that it is destroyed even if we error.

	if (err.HasError()) {
		err.ThrowAsException();
	}

	return result;
}

static auto CV2ScalarExec(DataChunk &input, ExpressionState &state, Vector &result) -> void {
	auto &expr = state.expr.Cast<BoundFunctionExpression>();
	const auto &info = expr.Function().GetExtraFunctionInfo().Cast<CV2ScalarFunctionInfo>();

	CV2ExecInfo args;
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	args.input = &input;
	args.function = &expr.Function();
	args.result = &result;

	// Setup bind data (if provided)
	if (auto bind_ptr = expr.BindInfo().get()) {
		const auto &bind_data = bind_ptr->Cast<CV2FunctionData>();
		args.in_bind_data = bind_data.handle ? bind_data.handle->GetData() : nullptr;
	}

	// Setup local state (if provided)
	if (auto state_ptr = ExecuteFunctionState::GetFunctionState(state)) {
		const auto &state_data = state_ptr->Cast<CV2FunctionLocalState>();
		args.in_init_data = state_data.handle.GetData();
	}

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);

	CV2CallbackContext callback_context(state.GetClientContext());
	info.exec_cb(Convert(&args), Convert(&callback_context), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}
}

static auto CV2ScalarStats(ClientContext &context, FunctionStatisticsInput &input) -> unique_ptr<BaseStatistics> {
	const auto &function = input.expr.Function();
	const auto &info = function.GetExtraFunctionInfo().Cast<CV2ScalarFunctionInfo>();

	auto result = BaseStatistics::CreateUnknown(input.expr.GetReturnType());
	CV2ScalarStatsInfo args(function, result);
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	if (input.bind_data) {
		const auto &bind_data = input.bind_data->Cast<CV2FunctionData>();
		args.in_bind_data = bind_data.handle ? bind_data.handle->GetData() : nullptr;
	}
	args.arg_stats.reserve(input.child_stats.size());
	for (const auto &child_stats : input.child_stats) {
		args.arg_stats.emplace_back(child_stats);
	}

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(context);
	info.stats_cb(Convert(&args), Convert(&callback_context), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}

	return result.ToUnique();
}

class CV2ScalarFunction {
public:
	explicit CV2ScalarFunction(DatabaseInstance &db) : db(db) {
		// The exec callback's error slot makes failure part of the API contract, so the function defaults to fallible:
		// a non-fallible function turns any execution error reported through the slot into an internal error.
		properties.SetFallible();
	}

	//! Validates the configuration and builds the scalar function to register on `target`, which replaces the
	//! overload `overridden` of the function of its name, if any.
	ScalarFunction Build(DatabaseInstance &target, shared_ptr<const ScalarFunction> overridden = nullptr) {
		CheckRegistrationTarget(db, target, "scalar function");
		if (name.empty()) {
			throw InvalidInputException("Function name cannot be empty.");
		}
		if (!info.exec_cb) {
			throw InvalidInputException("Exec callback must be set for the function.");
		}

		const auto &return_type = signature.GetReturnType();

		// ANY is allowed as a placeholder return type only when a resolve types callback is present to resolve the
		// actual type.
		if (return_type.id() == LogicalTypeId::ANY) {
			if (info.resolve_types_cb == nullptr) {
				throw InvalidInputException(
				    "An ANY return type requires a resolve types callback to set the concrete return type.");
			}
		} else {
			if (return_type.id() == LogicalTypeId::INVALID) {
				throw InvalidInputException("Return type must be set for the function.");
			}
			if (!return_type.IsComplete()) {
				throw InvalidInputException("Return type must be a fully defined concrete type");
			}
		}

		signature.Verify();

		ScalarFunction function(name, {}, return_type, CV2ScalarExec);
		function.SetProperties(properties);
		function.GetSignature() = signature;
		if (!arg_properties.empty()) {
			function.SetArgProperties(arg_properties);
		}

		if (info.resolve_types_cb) {
			function.SetResolveTypesCallback(CV2ScalarResolveTypes);
		}
		if (info.bind_cb) {
			function.SetBindCallback(CV2ScalarBind);
		}
		if (info.init_cb) {
			function.SetInitStateCallback(CV2ScalarInit);
		}
		if (info.stats_cb) {
			function.SetStatisticsCallback(CV2ScalarStats);
		}
		info.schema = schema;
		info.overridden = std::move(overridden);
		function.SetExtraFunctionInfo<CV2ScalarFunctionInfo>(std::move(info));

		return function;
	}

public:
	//! The database it was created for: the only one it can be registered on.
	DatabaseInstance &db;
	FunctionSignature signature;
	CV2FunctionDocs docs;
	CV2ScalarFunctionInfo info;
	Identifier name;
	//! The schema of the system catalog the function is registered in
	Identifier schema = Identifier::DefaultSchema();
	FunctionProperties properties;
	vector<ArgProperties> arg_properties;
};

//! The overload of an existing function that a function with `signature` replaces, if any
static auto FindOverridden(optional_ptr<CatalogEntry> entry, const FunctionSignature &signature)
    -> shared_ptr<const ScalarFunction> {
	if (!entry || entry->type != CatalogType::SCALAR_FUNCTION_ENTRY) {
		return nullptr;
	}
	for (auto &function : entry->Cast<ScalarFunctionCatalogEntry>().functions.functions) {
		if (function->GetSignature().IsSameOverload(signature)) {
			return function;
		}
	}
	return nullptr;
}

static auto ConvertMonotonicity(DUCKDB_V2_FUNCTION_MONOTONICITY monotonicity) -> Monotonicity {
	switch (monotonicity) {
	case DUCKDB_V2_FUNCTION_MONOTONICITY_UNKNOWN:
		return Monotonicity::UNKNOWN;
	case DUCKDB_V2_FUNCTION_MONOTONICITY_CONSTANT:
		return Monotonicity::CONSTANT;
	case DUCKDB_V2_FUNCTION_MONOTONICITY_NON_DECREASING:
		return Monotonicity::NON_DECREASING;
	case DUCKDB_V2_FUNCTION_MONOTONICITY_STRICTLY_INCREASING:
		return Monotonicity::STRICTLY_INCREASING;
	case DUCKDB_V2_FUNCTION_MONOTONICITY_NON_INCREASING:
		return Monotonicity::NON_INCREASING;
	case DUCKDB_V2_FUNCTION_MONOTONICITY_STRICTLY_DECREASING:
		return Monotonicity::STRICTLY_DECREASING;
	default:
		throw InvalidInputException("Unknown function monotonicity %d", static_cast<int>(monotonicity));
	}
}

static auto Convert(duckdb_v2_scalar_function_handle func) -> CV2ScalarFunction * {
	return reinterpret_cast<CV2ScalarFunction *>(func);
}
static auto Convert(CV2ScalarFunction *func) -> duckdb_v2_scalar_function_handle {
	return reinterpret_cast<duckdb_v2_scalar_function_handle>(func);
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_scalar_function_create(duckdb_v2_factory_handle factory,
                                                 duckdb_v2_scalar_function_handle *function,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(function);
	*function = nullptr;
	return WithErrorHandler(err, [&]() {
		auto result = duckdb::make_uniq<CV2ScalarFunction>(Convert(factory)->GetDatabase());
		*function = Convert(result.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_set_schema(duckdb_v2_scalar_function_handle function, const char *schema,
                                                     idx_t schema_length, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_STR_ARG(schema, schema_length);
	return WithErrorHandler(err, [&]() {
		auto name = ConvertIdentifierName(schema, schema_length);
		if (name.empty()) {
			throw duckdb::InvalidInputException("Schema name cannot be empty.");
		}
		Convert(function)->schema = duckdb::Identifier(name);
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_set_name(duckdb_v2_scalar_function_handle function, const char *name,
                                                   idx_t name_length, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_STR_ARG(name, name_length);
	return WithErrorHandler(
	    err, [&]() { Convert(function)->name = duckdb::Identifier(ConvertIdentifierName(name, name_length)); });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_get_docs(duckdb_v2_scalar_function_handle function,
                                                   duckdb_v2_function_docs_handle *docs,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(docs);
	return WithErrorHandler(err, [&]() { *docs = Convert(&Convert(function)->docs); });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_get_signature(duckdb_v2_scalar_function_handle function,
                                                        duckdb_v2_function_signature_handle *sig,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(sig);
	return WithErrorHandler(err, [&]() { *sig = Convert(&Convert(function)->signature); });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_set_user_data(duckdb_v2_scalar_function_handle function,
                                                        duckdb_v2_opaque *user_data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(user_data);
	return WithErrorHandler(err, [&]() {
		Convert(function)->info.user_data =
		    duckdb::make_shared_ptr<CV2UserData>(user_data->ptr, user_data->destroy, user_data->equals);
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_set_property(duckdb_v2_scalar_function_handle function,
                                                       DUCKDB_V2_FUNCTION_PROPERTY_KEY key,
                                                       DUCKDB_V2_FUNCTION_PROPERTY_VALUE value,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { SetScalarFunctionProperty(Convert(function)->properties, key, value); });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_set_generic_property(duckdb_v2_scalar_function_handle function,
                                                               const char *name, idx_t name_length,
                                                               duckdb_v2_value_handle value,
                                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_STR_ARG(name, name_length);
	DUCKDB_CHECK_ARG(value);
	return WithErrorHandler(
	    err, [&]() { SetGenericFunctionProperty(Convert(function)->properties, name, name_length, *Convert(value)); });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_get_generic_property(duckdb_v2_scalar_function_handle function,
                                                               const char *name, idx_t name_length,
                                                               duckdb_v2_value_handle *out_value,
                                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_STR_ARG(name, name_length);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(
	    err, [&]() { *out_value = GetGenericFunctionProperty(Convert(function)->properties, name, name_length); });
}

DUCKDB_V2_ERROR
duckdb_v2_scalar_function_set_resolve_types_callback(duckdb_v2_scalar_function_handle function,
                                                     duckdb_v2_scalar_function_resolve_types_callback_fn callback,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.resolve_types_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_set_bind_callback(duckdb_v2_scalar_function_handle function,
                                                            duckdb_v2_scalar_function_bind_callback_fn callback,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.bind_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_set_init_callback(duckdb_v2_scalar_function_handle function,
                                                            duckdb_v2_scalar_function_init_callback_fn callback,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.init_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_set_exec_callback(duckdb_v2_scalar_function_handle function,
                                                            duckdb_v2_scalar_function_exec_callback_fn callback,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.exec_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_set_stats_callback(duckdb_v2_scalar_function_handle function,
                                                             duckdb_v2_scalar_function_stats_callback_fn callback,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.stats_cb = callback; });
}

DUCKDB_V2_ERROR
duckdb_v2_scalar_function_resolve_types_set_return_type(duckdb_v2_scalar_function_resolve_types_info_handle info,
                                                        duckdb_v2_logical_type_handle return_type,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(return_type);
	return WithErrorHandler(err, [&]() {
		auto type = Convert(return_type);
		Convert(info)->bound_function.SetReturnType(*type);
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_bind_set_bind_data(duckdb_v2_scalar_function_bind_info_handle info,
                                                             duckdb_v2_opaque *data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() {
		Convert(info)->out_bind_data =
		    data->ptr ? duckdb::make_shared_ptr<CV2UserData>(data->ptr, data->destroy, data->equals) : nullptr;
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_set_argument_monotonicity(duckdb_v2_scalar_function_handle function,
                                                                    idx_t index,
                                                                    DUCKDB_V2_FUNCTION_MONOTONICITY monotonicity,
                                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		auto value = ConvertMonotonicity(monotonicity);
		auto &props = Convert(function)->arg_properties;
		if (props.size() <= index) {
			props.resize(index + 1);
		}
		props[index].monotonicity = value;
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_bind_set_property(duckdb_v2_scalar_function_bind_info_handle info,
                                                            DUCKDB_V2_FUNCTION_PROPERTY_KEY key,
                                                            DUCKDB_V2_FUNCTION_PROPERTY_VALUE value,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() {
		if (key != DUCKDB_V2_FUNCTION_PROPERTY_STABILITY) {
			throw duckdb::InvalidInputException("Only the stability of a scalar function call can be set at bind time");
		}
		duckdb::FunctionProperties props;
		SetScalarFunctionProperty(props, key, value);
		Convert(info)->stability = props.GetStability();
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_bind_set_argument_monotonicity(
    duckdb_v2_scalar_function_bind_info_handle info, idx_t index, DUCKDB_V2_FUNCTION_MONOTONICITY monotonicity,
    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(
	    err, [&]() { Convert(info)->monotonicity.emplace_back(index, ConvertMonotonicity(monotonicity)); });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_bind_call_overridden(duckdb_v2_scalar_function_bind_info_handle info,
                                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() { Convert(info)->call_overridden = true; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_bind_replace_call(duckdb_v2_scalar_function_bind_info_handle info,
                                                            const char *name, idx_t name_length,
                                                            const idx_t *arg_indices, idx_t arg_count,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_STR_ARG(name, name_length);
	if (arg_count > 0) {
		DUCKDB_CHECK_ARG(arg_indices);
	}
	return WithErrorHandler(err, [&]() {
		auto function_name = ConvertIdentifierName(name, name_length);
		if (function_name.empty()) {
			throw duckdb::InvalidInputException("The name of the replacement function cannot be empty");
		}
		auto &bind_info = *Convert(info);
		bind_info.replacement_name = duckdb::string(function_name);
		bind_info.replacement_args.assign(arg_indices, arg_indices + arg_count);
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_init_get_user_data(duckdb_v2_scalar_function_init_info_handle info,
                                                             void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_init_get_bind_data(duckdb_v2_scalar_function_init_info_handle info,
                                                             void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_init_set_init_data(duckdb_v2_scalar_function_init_info_handle info,
                                                             duckdb_v2_opaque *data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);

	return WithErrorHandler(err, [&]() { Convert(info)->out_init_data = *data; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_exec_get_user_data(duckdb_v2_scalar_function_exec_info_handle info,
                                                             void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);

	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_exec_get_bind_data(duckdb_v2_scalar_function_exec_info_handle info,
                                                             void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);

	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_exec_get_init_data(duckdb_v2_scalar_function_exec_info_handle info,
                                                             void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);

	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_init_data; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_exec_get_row_count(duckdb_v2_scalar_function_exec_info_handle info,
                                                             idx_t *count, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(count);

	return WithErrorHandler(err, [&]() { *count = Convert(info)->input->size(); });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_exec_get_arg_count(duckdb_v2_scalar_function_exec_info_handle info,
                                                             idx_t *positional_fixed, idx_t *positional_variadic,
                                                             idx_t *named_fixed, idx_t *named_variadic,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() {
		auto &function = *Convert(info)->function;
		if (positional_fixed) {
			*positional_fixed = function.GetStandardArgumentCount();
		}
		if (positional_variadic) {
			*positional_variadic = function.GetVarArgsCount();
		}
		if (named_fixed) {
			*named_fixed = function.GetKeywordOnlyArgumentCount();
		}
		if (named_variadic) {
			*named_variadic = function.GetKwargsCount();
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_exec_get_arg(duckdb_v2_scalar_function_exec_info_handle info, idx_t index,
                                                       duckdb_v2_vector_handle *vector,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(vector);
	return WithErrorHandler(err, [&]() {
		auto &exec_info = *Convert(info);
		if (index >= exec_info.input->ColumnCount()) {
			throw duckdb::InvalidInputException("Index out of bounds in duckdb_v2_scalar_function_exec_get_arg");
		}
		*vector = Convert(&exec_info.input->data[index]);
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_exec_get_result(duckdb_v2_scalar_function_exec_info_handle info,
                                                          duckdb_v2_vector_handle *vector,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(vector);

	return WithErrorHandler(err, [&]() {
		auto &exec_info = *Convert(info);
		*vector = Convert(exec_info.result);
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_stats_get_user_data(duckdb_v2_scalar_function_stats_info_handle info,
                                                              void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_stats_get_bind_data(duckdb_v2_scalar_function_stats_info_handle info,
                                                              void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_stats_get_arg_count(duckdb_v2_scalar_function_stats_info_handle info,
                                                              idx_t *positional_fixed, idx_t *positional_variadic,
                                                              idx_t *named_fixed, idx_t *named_variadic,
                                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() {
		auto &function = Convert(info)->function;
		if (positional_fixed) {
			*positional_fixed = function.GetStandardArgumentCount();
		}
		if (positional_variadic) {
			*positional_variadic = function.GetVarArgsCount();
		}
		if (named_fixed) {
			*named_fixed = function.GetKeywordOnlyArgumentCount();
		}
		if (named_variadic) {
			*named_variadic = function.GetKwargsCount();
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_stats_get_arg_stats(duckdb_v2_scalar_function_stats_info_handle info,
                                                              idx_t index, duckdb_v2_stats_handle *stats,
                                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(stats);
	return WithErrorHandler(err, [&]() {
		auto &stats_info = *Convert(info);
		if (index >= stats_info.arg_stats.size()) {
			throw duckdb::InvalidInputException("Index out of bounds in duckdb_v2_scalar_function_stats_get_arg_stats");
		}
		*stats = Convert(&stats_info.arg_stats[index]);
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_stats_get_result_stats(duckdb_v2_scalar_function_stats_info_handle info,
                                                                 duckdb_v2_stats_handle *stats,
                                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(stats);
	return WithErrorHandler(err, [&]() { *stats = Convert(&Convert(info)->result_stats); });
}

DUCKDB_V2_ERROR duckdb_v2_connection_register_scalar_function(duckdb_v2_connection_handle conn,
                                                              duckdb_v2_scalar_function_handle function,
                                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		auto &context = *Convert(conn)->context;
		context.RunFunctionInTransaction([&]() {
			auto &self = *Convert(function);
			auto &catalog = duckdb::Catalog::GetSystemCatalog(context);
			auto existing = catalog.GetEntry(context, duckdb::CatalogType::SCALAR_FUNCTION_ENTRY, self.schema, self.name,
			                                 duckdb::OnEntryNotFound::RETURN_NULL);
			auto info = duckdb::CreateScalarFunctionInfo(self.Build(*context.db, FindOverridden(existing, self.signature)));
			info.SetQualifiedName(
			    duckdb::QualifiedName(info.GetQualifiedName().Catalog(), self.schema, info.GetFunctionName()));
			self.docs.AddTo(self.signature, info);
			info.on_conflict = duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
			catalog.CreateFunction(context, info);
		});
	});
}

DUCKDB_V2_ERROR duckdb_v2_extension_register_scalar_function(duckdb_v2_extension_handle extension,
                                                             duckdb_v2_scalar_function_handle function,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(extension);
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		auto &self = *Convert(function);
		auto &loader = GetExtensionLoader(extension);
		duckdb::optional_ptr<duckdb::CatalogEntry> existing;
		if (self.schema == duckdb::Identifier::DefaultSchema()) {
			existing = loader.TryGetFunction(self.name);
		}
		duckdb::CreateScalarFunctionInfo info(
		    self.Build(loader.GetDatabaseInstance(), FindOverridden(existing, self.signature)));
		info.SetQualifiedName(
		    duckdb::QualifiedName(info.GetQualifiedName().Catalog(), self.schema, info.GetFunctionName()));
		self.docs.AddTo(self.signature, info);
		info.on_conflict = duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
		loader.RegisterFunction(std::move(info));
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_destroy(duckdb_v2_scalar_function_handle *function) {
	return WithErrorHandler(nullptr, [&]() {
		if (!function) {
			return;
		}
		if (*function) {
			delete Convert(*function);
			*function = nullptr;
		}
	});
}
