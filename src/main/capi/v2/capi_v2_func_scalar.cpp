#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/main/capi_v2/capi_v2_function_internal.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
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
	//! Whether the callback replaced the call, and by what
	//! The monotonicity overrides of the call, by argument index
	vector<pair<idx_t, Monotonicity>> monotonicity;
	bool has_stability = false;
	FunctionStability stability = FunctionStability::CONSISTENT;
	bool replace_call = false;
	Identifier replace_name;
	vector<idx_t> replace_arguments;
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

class CV2SerializeInfo {
public:
	CV2SerializeInfo(void *user_data, void *bind_data) : user_data(user_data), bind_data(bind_data) {
	}

	void *user_data;
	void *bind_data;
	//! The bytes written by the callback
	string bytes;
};

static auto Convert(duckdb_v2_scalar_function_serialize_info_handle info) -> CV2SerializeInfo * {
	return reinterpret_cast<CV2SerializeInfo *>(info);
}
static auto Convert(CV2SerializeInfo *info) -> duckdb_v2_scalar_function_serialize_info_handle {
	return reinterpret_cast<duckdb_v2_scalar_function_serialize_info_handle>(info);
}

class CV2DeserializeInfo {
public:
	void *user_data = nullptr;
	string bytes;
	shared_ptr<CV2UserData> out_bind_data;
};

static auto Convert(duckdb_v2_scalar_function_deserialize_info_handle info) -> CV2DeserializeInfo * {
	return reinterpret_cast<CV2DeserializeInfo *>(info);
}
static auto Convert(CV2DeserializeInfo *info) -> duckdb_v2_scalar_function_deserialize_info_handle {
	return reinterpret_cast<duckdb_v2_scalar_function_deserialize_info_handle>(info);
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
	DUCKDB_V2_CAST_MODE cast_mode = DUCKDB_V2_CAST_MODE_NORMAL;
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
	duckdb_v2_scalar_function_serialize_callback_fn serialize_cb = nullptr;
	duckdb_v2_scalar_function_deserialize_callback_fn deserialize_cb = nullptr;
	shared_ptr<CV2UserData> user_data = nullptr;
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

//! Rebinds the call as a call to the function the callback named, taking the arguments it listed
static auto ReplaceCall(BindScalarFunctionInput &input, const CV2BindInfo &result_info) -> unique_ptr<FunctionData> {
	auto &bound_function = input.GetBoundFunction();
	auto &context = input.GetClientContext();
	auto &arguments = input.GetArguments();

	vector<unique_ptr<Expression>> children;
	vector<LogicalType> argument_types;
	for (auto index : result_info.replace_arguments) {
		if (index >= arguments.size()) {
			throw InvalidInputException("Argument index %llu of the replacement of function %s is out of bounds: the "
			                            "call has %llu arguments",
			                            index, bound_function.GetName(), arguments.size());
		}
		children.push_back(arguments[index]->Copy());
		argument_types.push_back(arguments[index]->GetReturnType());
	}

	auto definition_name = bound_function.GetDefinition()->GetQualifiedName();
	if (result_info.replace_name == definition_name.Name()) {
		throw InvalidInputException("Function %s cannot replace its own call by a call to itself",
		                            bound_function.GetName());
	}
	auto replacement_name = definition_name.WithName(result_info.replace_name);
	auto &entry = Catalog::GetEntry<ScalarFunctionCatalogEntry>(context, replacement_name);
	auto replacement = entry.functions.GetFunctionByArguments(context, argument_types);

	FunctionBinder binder(context);
	auto expression = binder.BindScalarFunction(std::move(replacement), std::move(children), false,
	                                            input.HasBinder() ? &input.GetBinder() : nullptr);
	if (expression->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		throw InvalidInputException("The replacement %s of function %s was simplified away: it must remain a "
		                            "function call",
		                            result_info.replace_name.GetIdentifierName(), bound_function.GetName());
	}
	auto &replaced = expression->Cast<BoundFunctionExpression>();
	bound_function = std::move(replaced.FunctionMutable());
	arguments = std::move(replaced.GetChildrenMutable());
	return std::move(replaced.BindInfoMutable());
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

	// Throw after reading the bind data, so that it is destroyed even if we error.
	unique_ptr<FunctionData> result = nullptr;
	if (result_info.out_bind_data) {
		auto set_result = make_uniq<CV2FunctionData>();
		set_result->handle = std::move(result_info.out_bind_data);
		result = std::move(set_result);
	}

	if (err.HasError()) {
		err.ThrowAsException();
	}
	if (result_info.replace_call) {
		return ReplaceCall(input, result_info);
	}
	if (result_info.has_stability) {
		bound_function.SetStability(result_info.stability);
	}
	for (auto &entry : result_info.monotonicity) {
		if (entry.first >= input.GetArguments().size()) {
			throw InvalidInputException("Argument index %llu of the monotonicity of function %s is out of bounds: the "
			                            "call has %llu arguments",
			                            entry.first, bound_function.GetName(), input.GetArguments().size());
		}
		ArgProperties props;
		props.monotonicity = entry.second;
		bound_function.SetArgProperties(entry.first, props);
	}

	return result;
}

static auto CV2ScalarSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
                               const BoundScalarFunction &function) -> void {
	const auto &info = function.GetExtraFunctionInfo().Cast<CV2ScalarFunctionInfo>();

	void *user_bind_data = nullptr;
	if (bind_data) {
		const auto &data = bind_data->Cast<CV2FunctionData>();
		user_bind_data = data.handle ? data.handle->GetData() : nullptr;
	}
	CV2SerializeInfo serialize_info(info.user_data ? info.user_data->GetData() : nullptr, user_bind_data);

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	info.serialize_cb(Convert(&serialize_info), &err_ptr);
	if (err.HasError()) {
		err.ThrowAsException();
	}

	serializer.WriteProperty<idx_t>(100, "size", serialize_info.bytes.size());
	serializer.WriteProperty(101, "data", const_data_ptr_cast(serialize_info.bytes.data()),
	                         serialize_info.bytes.size());
}

static auto CV2ScalarDeserialize(Deserializer &deserializer, BoundScalarFunction &function)
    -> unique_ptr<FunctionData> {
	const auto &info = function.GetExtraFunctionInfo().Cast<CV2ScalarFunctionInfo>();

	CV2DeserializeInfo deserialize_info;
	deserialize_info.user_data = info.user_data ? info.user_data->GetData() : nullptr;
	auto size = deserializer.ReadProperty<idx_t>(100, "size");
	deserialize_info.bytes.resize(size);
	deserializer.ReadProperty(101, "data", data_ptr_cast(&deserialize_info.bytes[0]), size);

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(deserializer.Get<ClientContext &>());
	info.deserialize_cb(Convert(&deserialize_info), Convert(&callback_context), &err_ptr);

	unique_ptr<FunctionData> result = nullptr;
	if (deserialize_info.out_bind_data) {
		auto set_result = make_uniq<CV2FunctionData>();
		set_result->handle = std::move(deserialize_info.out_bind_data);
		result = std::move(set_result);
	}
	if (err.HasError()) {
		err.ThrowAsException();
	}
	return result;
}

static auto CV2ScalarInit(ExpressionState &state, const BoundFunctionExpression &expr, FunctionData *bind_data)
    -> unique_ptr<FunctionLocalState> {
	const auto &info = expr.Function().GetExtraFunctionInfo().Cast<CV2ScalarFunctionInfo>();

	auto user_bind_data = bind_data ? bind_data->Cast<CV2FunctionData>().handle : nullptr;

	CV2InitInfo args = {};
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	args.in_bind_data = user_bind_data ? user_bind_data->GetData() : nullptr;

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	// Without a client context (e.g. while an index is built) the callback receives a NULL context
	unique_ptr<CV2CallbackContext> callback_context;
	if (state.HasContext()) {
		callback_context = make_uniq<CV2CallbackContext>(state.GetContext());
	}
	info.init_cb(Convert(&args), Convert(callback_context.get()), &err_ptr);

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

	// A function registered as a cast runs under a cast executor, which tells a try cast apart
	optional_ptr<CV2CastExecutor> cast_executor;
	if (state.root.executor && state.root.executor == ActiveCastExecutor()) {
		cast_executor = ActiveCastExecutor();
	}

	CV2ExecInfo args;
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	if (cast_executor && cast_executor->is_try) {
		args.cast_mode = DUCKDB_V2_CAST_MODE_TRY;
	}
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

	unique_ptr<CV2CallbackContext> callback_context;
	if (state.HasContext()) {
		callback_context = make_uniq<CV2CallbackContext>(state.GetContext());
	}
	info.exec_cb(Convert(&args), Convert(callback_context.get()), &err_ptr);

	if (err.HasError()) {
		if (args.cast_mode == DUCKDB_V2_CAST_MODE_TRY) {
			// The rows the callback wrote stay: the cast reports that not every row converted
			cast_executor->error = err.message;
			return;
		}
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

	//! Validates the configuration and builds the scalar function to register on `target`.
	ScalarFunction Build(DatabaseInstance &target) {
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

		if (info.resolve_types_cb) {
			function.SetResolveTypesCallback(CV2ScalarResolveTypes);
		}
		if (info.bind_cb) {
			function.SetBindCallback(CV2ScalarBind);
		}
		if (info.init_cb) {
			function.SetInitStateCallback(CV2ScalarInit);
		}
		if (info.stats_cb && finite_monotone_bounds) {
			throw InvalidInputException("The finite monotone bounds statistics propagation cannot be combined with a "
			                            "stats callback.");
		}
		if (info.stats_cb) {
			function.SetStatisticsCallback(CV2ScalarStats);
		} else if (finite_monotone_bounds) {
			function.SetStatisticsCallback(StatisticsPropagator::PropagateFiniteMonotoneBounds);
		}
		for (auto &entry : monotonicity) {
			if (entry.first >= signature.GetParameterCount()) {
				throw InvalidInputException("Argument index %llu of the monotonicity is out of bounds: the signature "
				                            "has %llu parameters.",
				                            entry.first, signature.GetParameterCount());
			}
			ArgProperties props;
			props.monotonicity = entry.second;
			function.SetArgProperties(entry.first, props);
		}
		if (info.serialize_cb) {
			function.SetSerializeCallback(CV2ScalarSerialize);
			function.SetDeserializeCallback(CV2ScalarDeserialize);
		}
		function.SetExtraFunctionInfo<CV2ScalarFunctionInfo>(std::move(info));

		return function;
	}

	//! Builds the function a cast is registered from: one concrete parameter, converting to the concrete return type
	ScalarFunction BuildForCast(DatabaseInstance &target) {
		if (signature.GetParameterCount() != 1 || signature.IsVariadic() ||
		    !signature.GetParameter(0).AcceptsPosition()) {
			throw InvalidInputException("A cast function must take exactly one standard parameter: the value to cast.");
		}
		if (!signature.GetParameter(0).GetType().IsComplete()) {
			throw InvalidInputException("The parameter of a cast function must be a fully defined concrete type");
		}
		if (info.resolve_types_cb) {
			throw InvalidInputException("A cast function cannot have a resolve types callback: its types are fixed.");
		}
		return Build(target);
	}

public:
	//! The database it was created for: the only one it can be registered on.
	DatabaseInstance &db;
	FunctionSignature signature;
	CV2FunctionDocs docs;
	CV2ScalarFunctionInfo info;
	Identifier name;
	FunctionProperties properties;
	bool finite_monotone_bounds = false;
	//! The declared monotonicity per argument index
	vector<pair<idx_t, Monotonicity>> monotonicity;
};

static auto Convert(duckdb_v2_scalar_function_handle func) -> CV2ScalarFunction * {
	return reinterpret_cast<CV2ScalarFunction *>(func);
}
static auto Convert(CV2ScalarFunction *func) -> duckdb_v2_scalar_function_handle {
	return reinterpret_cast<duckdb_v2_scalar_function_handle>(func);
}

ScalarFunction BuildScalarFunctionForCast(duckdb_v2_scalar_function_handle function, DatabaseInstance &target) {
	return Convert(function)->BuildForCast(target);
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

DUCKDB_V2_ERROR duckdb_v2_scalar_function_set_name(duckdb_v2_scalar_function_handle function, const duckdb_v2_str *name,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(name);
	return WithErrorHandler(err, [&]() { Convert(function)->name = duckdb::Identifier(ConvertIdentifierName(name)); });
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
	return WithErrorHandler(err, [&]() {
		auto &self = *Convert(function);
		if (!TrySetScalarStatisticsPropagation(key, value, self.finite_monotone_bounds)) {
			SetScalarFunctionProperty(self.properties, key, value);
		}
	});
}

static auto ConvertMonotonicity(DUCKDB_V2_MONOTONICITY value) -> duckdb::Monotonicity {
	switch (value) {
	case DUCKDB_V2_MONOTONICITY_UNKNOWN:
		return duckdb::Monotonicity::UNKNOWN;
	case DUCKDB_V2_MONOTONICITY_CONSTANT:
		return duckdb::Monotonicity::CONSTANT;
	case DUCKDB_V2_MONOTONICITY_NON_DECREASING:
		return duckdb::Monotonicity::NON_DECREASING;
	case DUCKDB_V2_MONOTONICITY_STRICTLY_INCREASING:
		return duckdb::Monotonicity::STRICTLY_INCREASING;
	case DUCKDB_V2_MONOTONICITY_NON_INCREASING:
		return duckdb::Monotonicity::NON_INCREASING;
	case DUCKDB_V2_MONOTONICITY_STRICTLY_DECREASING:
		return duckdb::Monotonicity::STRICTLY_DECREASING;
	default:
		throw duckdb::InvalidInputException("Unknown monotonicity %d", static_cast<int>(value));
	}
}

//! Records the monotonicity of an argument, replacing an earlier one of the same argument
static auto RecordMonotonicity(duckdb::vector<duckdb::pair<duckdb::idx_t, duckdb::Monotonicity>> &list,
                               duckdb::idx_t index, duckdb::Monotonicity value) -> void {
	for (auto &entry : list) {
		if (entry.first == index) {
			entry.second = value;
			return;
		}
	}
	list.emplace_back(index, value);
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_set_argument_monotonicity(duckdb_v2_scalar_function_handle function,
                                                                    idx_t argument_index,
                                                                    DUCKDB_V2_MONOTONICITY monotonicity,
                                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		RecordMonotonicity(Convert(function)->monotonicity, argument_index, ConvertMonotonicity(monotonicity));
	});
}

DUCKDB_V2_ERROR
duckdb_v2_scalar_function_bind_set_argument_monotonicity(duckdb_v2_scalar_function_bind_info_handle info,
                                                         idx_t argument_index, DUCKDB_V2_MONOTONICITY monotonicity,
                                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() {
		auto &bind_info = *Convert(info);
		if (bind_info.replace_call) {
			throw duckdb::InvalidInputException("The argument monotonicity cannot be set after the call was replaced: "
			                                    "the replacement has its own properties");
		}
		RecordMonotonicity(bind_info.monotonicity, argument_index, ConvertMonotonicity(monotonicity));
	});
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
		if (Convert(info)->replace_call) {
			throw duckdb::InvalidInputException(
			    "The bind data cannot be set after the call was replaced: replacing it must be the only thing the "
			    "bind callback does");
		}
		Convert(info)->out_bind_data =
		    data->ptr ? duckdb::make_shared_ptr<CV2UserData>(data->ptr, data->destroy, data->equals) : nullptr;
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_bind_replace_call(duckdb_v2_scalar_function_bind_info_handle info,
                                                            const duckdb_v2_identifier_t *function_name,
                                                            const idx_t *argument_indices, idx_t argument_count,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(function_name);
	if (argument_count > 0) {
		DUCKDB_CHECK_ARG(argument_indices);
	}
	return WithErrorHandler(err, [&]() {
		auto &bind_info = *Convert(info);
		if (bind_info.out_bind_data) {
			throw duckdb::InvalidInputException(
			    "The call cannot be replaced after its bind data was set: replacing it must be the only thing the "
			    "bind callback does");
		}
		if (bind_info.replace_call) {
			throw duckdb::InvalidInputException("The call can only be replaced once");
		}
		if (bind_info.has_stability) {
			throw duckdb::InvalidInputException(
			    "The call cannot be replaced after its stability was set: the replacement has its own stability");
		}
		if (!bind_info.monotonicity.empty()) {
			throw duckdb::InvalidInputException("The call cannot be replaced after its argument monotonicity was set: "
			                                    "the replacement has its own properties");
		}
		bind_info.replace_call = true;
		bind_info.replace_name = duckdb::Identifier(ConvertIdentifierName(function_name));
		bind_info.replace_arguments.assign(argument_indices, argument_indices + argument_count);
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_bind_set_stability(duckdb_v2_scalar_function_bind_info_handle info,
                                                             DUCKDB_V2_FUNCTION_PROPERTY_VALUE value,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() {
		auto &bind_info = *Convert(info);
		if (bind_info.replace_call) {
			throw duckdb::InvalidInputException(
			    "The stability cannot be set after the call was replaced: the replacement has its own stability");
		}
		duckdb::FunctionProperties properties;
		SetScalarFunctionProperty(properties, DUCKDB_V2_FUNCTION_PROPERTY_STABILITY, value);
		bind_info.has_stability = true;
		bind_info.stability = properties.stability;
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_set_serialize_callbacks(
    duckdb_v2_scalar_function_handle function, duckdb_v2_scalar_function_serialize_callback_fn serialize_callback,
    duckdb_v2_scalar_function_deserialize_callback_fn deserialize_callback, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		if ((serialize_callback == nullptr) != (deserialize_callback == nullptr)) {
			throw duckdb::InvalidInputException("The serialize and deserialize callbacks must be set together");
		}
		auto &info = Convert(function)->info;
		info.serialize_cb = serialize_callback;
		info.deserialize_cb = deserialize_callback;
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_serialize_get_user_data(duckdb_v2_scalar_function_serialize_info_handle info,
                                                                  void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_serialize_get_bind_data(duckdb_v2_scalar_function_serialize_info_handle info,
                                                                  void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_serialize_write(duckdb_v2_scalar_function_serialize_info_handle info,
                                                          const void *data, idx_t size,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	if (size > 0) {
		DUCKDB_CHECK_ARG(data);
	}
	return WithErrorHandler(err, [&]() {
		if (size > 0) {
			Convert(info)->bytes.append(static_cast<const char *>(data), size);
		}
	});
}

DUCKDB_V2_ERROR
duckdb_v2_scalar_function_deserialize_get_user_data(duckdb_v2_scalar_function_deserialize_info_handle info, void **data,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_deserialize_get_data(duckdb_v2_scalar_function_deserialize_info_handle info,
                                                               const void **data, idx_t *size,
                                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	DUCKDB_CHECK_ARG(size);
	return WithErrorHandler(err, [&]() {
		*data = Convert(info)->bytes.data();
		*size = Convert(info)->bytes.size();
	});
}

DUCKDB_V2_ERROR
duckdb_v2_scalar_function_deserialize_set_bind_data(duckdb_v2_scalar_function_deserialize_info_handle info,
                                                    duckdb_v2_opaque *data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() {
		Convert(info)->out_bind_data =
		    data->ptr ? duckdb::make_shared_ptr<CV2UserData>(data->ptr, data->destroy, data->equals) : nullptr;
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

DUCKDB_V2_ERROR duckdb_v2_scalar_function_exec_get_cast_mode(duckdb_v2_scalar_function_exec_info_handle info,
                                                             DUCKDB_V2_CAST_MODE *mode,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(mode);

	return WithErrorHandler(err, [&]() { *mode = Convert(info)->cast_mode; });
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
			auto info = duckdb::CreateScalarFunctionInfo(self.Build(*context.db));
			self.docs.AddTo(self.signature, info);
			info.on_conflict = duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
			auto &catalog = duckdb::Catalog::GetSystemCatalog(context);
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
		duckdb::CreateScalarFunctionInfo info(self.Build(loader.GetDatabaseInstance()));
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
