#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"

namespace duckdb::capiv2 {

class CV2CastBoundData final : public BoundCastData {
public:
	CV2CastBoundData(duckdb_v2_cast_function_exec_callback_fn exec_cb, shared_ptr<CV2UserData> user_data,
	                 shared_ptr<CV2UserData> bind_data = nullptr)
	    : exec_cb(exec_cb), user_data(std::move(user_data)), bind_data(std::move(bind_data)) {
	}

	auto Copy() const -> unique_ptr<BoundCastData> override {
		return make_uniq<CV2CastBoundData>(exec_cb, user_data, bind_data);
	}

public:
	duckdb_v2_cast_function_exec_callback_fn exec_cb;
	shared_ptr<CV2UserData> user_data;
	shared_ptr<CV2UserData> bind_data;
};

//! The per-cast info the engine hands to the bind function
class CV2CastBindFunctionInfo final : public BindCastInfo {
public:
	duckdb_v2_cast_function_exec_callback_fn exec_cb = nullptr;
	duckdb_v2_cast_function_bind_callback_fn bind_cb = nullptr;
	shared_ptr<CV2UserData> user_data;
	FunctionStability stability = FunctionStability::CONSISTENT;
};

//! What the bind callback reads and writes, behind the bind info handle
class CV2CastBindInfo {
public:
	void *in_user_data = nullptr;
	LogicalType source_type;
	LogicalType target_type;
	shared_ptr<CV2UserData> out_bind_data;
};

static auto Convert(duckdb_v2_cast_function_bind_info_handle info) -> CV2CastBindInfo * {
	return reinterpret_cast<CV2CastBindInfo *>(info);
}
static auto Convert(CV2CastBindInfo *info) -> duckdb_v2_cast_function_bind_info_handle {
	return reinterpret_cast<duckdb_v2_cast_function_bind_info_handle>(info);
}

// CastParameters carries no context, so the only place to pick one up is the local state init, which does receive
// one. The state exists purely to forward it to the exec callback.
class CV2CastLocalState final : public FunctionLocalState {
public:
	unique_ptr<CV2CallbackContext> context;
};

class CV2CastExecInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;

	Vector *input = nullptr;
	Vector *output = nullptr;
	idx_t count = 0;
	DUCKDB_V2_CAST_MODE mode = DUCKDB_V2_CAST_MODE_NORMAL;
};

static auto Convert(duckdb_v2_cast_function_exec_info_handle info) -> CV2CastExecInfo * {
	return reinterpret_cast<CV2CastExecInfo *>(info);
}
static auto Convert(CV2CastExecInfo *info) -> duckdb_v2_cast_function_exec_info_handle {
	return reinterpret_cast<duckdb_v2_cast_function_exec_info_handle>(info);
}

static auto CV2CastInitLocalState(CastLocalStateParameters &parameters) -> unique_ptr<FunctionLocalState> {
	auto state = make_uniq<CV2CastLocalState>();

	if (!parameters.context) {
		throw InvalidInputException("Cannot initialize local state for extension cast function without a context");
	}

	state->context = make_uniq<CV2CallbackContext>(*parameters.context);
	return std::move(state);
}

static auto CV2CastExec(Vector &input, Vector &output, idx_t count, CastParameters &parameters) -> bool {
	const auto &bound_data = parameters.cast_data->Cast<CV2CastBoundData>();

	// A try cast is exactly the case where the engine handed us a slot to write a message into instead of aborting.
	const auto is_try_cast = parameters.error_message != nullptr;

	CV2CastExecInfo args = {};
	args.in_user_data = bound_data.user_data ? bound_data.user_data->GetData() : nullptr;
	args.in_bind_data = bound_data.bind_data ? bound_data.bind_data->GetData() : nullptr;
	args.input = &input;
	args.output = &output;
	args.count = count;
	args.mode = is_try_cast ? DUCKDB_V2_CAST_MODE_TRY : DUCKDB_V2_CAST_MODE_NORMAL;

	optional_ptr<CV2Context> context = nullptr;
	if (parameters.local_state) {
		context = parameters.local_state->Cast<CV2CastLocalState>().context.get();
	}

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	bound_data.exec_cb(Convert(&args), Convert(context.get()), &err_ptr);

	if (!err.HasError()) {
		return true;
	}
	if (is_try_cast) {
		// The engine discards the message and keeps whatever the callback left in the output vector.
		HandleCastError::AssignError(err.message, parameters);
		return false;
	}
	err.ThrowAsException();
}

static auto CV2CastBind(BindCastInput &input, const LogicalType &source, const LogicalType &target) -> BoundCastInfo {
	D_ASSERT(input.info);
	const auto &info = input.info->Cast<CV2CastBindFunctionInfo>();

	CV2CastBindInfo bind_info;
	bind_info.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	bind_info.source_type = source;
	bind_info.target_type = target;

	// The context is absent when the engine binds a cast without a client context.
	unique_ptr<CV2CallbackContext> callback_context;
	if (input.context) {
		callback_context = make_uniq<CV2CallbackContext>(*input.context);
	}

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	info.bind_cb(Convert(&bind_info), Convert(callback_context.get()), &err_ptr);
	if (err.HasError()) {
		err.ThrowAsException();
	}

	BoundCastInfo cast_info(
	    CV2CastExec, make_uniq<CV2CastBoundData>(info.exec_cb, info.user_data, std::move(bind_info.out_bind_data)),
	    CV2CastInitLocalState);
	cast_info.SetStability(info.stability);
	return cast_info;
}

class CV2CastFunction {
public:
	explicit CV2CastFunction(DatabaseInstance &db) : db(db) {
	}

	//! Validates the configuration of the cast to register on `target`.
	void Validate(DatabaseInstance &target) const {
		CheckRegistrationTarget(db, target, "cast function");
		if (!source_type.IsComplete()) {
			throw InvalidInputException("Source type must be set to a fully defined concrete type");
		}
		if (!target_type.IsComplete()) {
			throw InvalidInputException("Target type must be set to a fully defined concrete type");
		}
		if (!exec_cb) {
			throw InvalidInputException("Exec callback must be set for the function.");
		}
	}

	//! Builds the cast without a bind callback.
	BoundCastInfo Build() const {
		BoundCastInfo cast_info(CV2CastExec, make_uniq<CV2CastBoundData>(exec_cb, user_data), CV2CastInitLocalState);
		cast_info.SetStability(properties.GetStability());
		return cast_info;
	}

	//! The info the bind function of a cast with a bind callback runs with.
	shared_ptr<BindCastInfo> BuildBindInfo() const {
		auto info = make_shared_ptr<CV2CastBindFunctionInfo>();
		info->exec_cb = exec_cb;
		info->bind_cb = bind_cb;
		info->user_data = user_data;
		info->stability = properties.GetStability();
		return info;
	}

public:
	//! The database it was created for: the only one it can be registered on.
	DatabaseInstance &db;
	LogicalType source_type;
	LogicalType target_type;
	int64_t implicit_cast_cost = -1;
	duckdb_v2_cast_function_exec_callback_fn exec_cb = nullptr;
	duckdb_v2_cast_function_bind_callback_fn bind_cb = nullptr;
	FunctionProperties properties;
	shared_ptr<CV2UserData> user_data = nullptr;
};

static auto Convert(duckdb_v2_cast_function_handle func) -> CV2CastFunction * {
	return reinterpret_cast<CV2CastFunction *>(func);
}
static auto Convert(CV2CastFunction *func) -> duckdb_v2_cast_function_handle {
	return reinterpret_cast<duckdb_v2_cast_function_handle>(func);
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_cast_function_create(duckdb_v2_factory_handle factory,
                                               duckdb_v2_cast_function_handle *function,
                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(function);
	*function = nullptr;
	return WithErrorHandler(err, [&]() {
		auto result = duckdb::make_uniq<CV2CastFunction>(Convert(factory)->GetDatabase());
		*function = Convert(result.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_set_source_type(duckdb_v2_cast_function_handle function,
                                                        duckdb_v2_logical_type_handle source_type,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(source_type);
	return WithErrorHandler(err, [&]() {
		auto type = Convert(source_type);
		Convert(function)->source_type = *type;
	});
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_set_target_type(duckdb_v2_cast_function_handle function,
                                                        duckdb_v2_logical_type_handle target_type,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(target_type);
	return WithErrorHandler(err, [&]() {
		auto type = Convert(target_type);
		Convert(function)->target_type = *type;
	});
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_set_implicit_cast_cost(duckdb_v2_cast_function_handle function, int64_t cost,
                                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->implicit_cast_cost = cost; });
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_set_user_data(duckdb_v2_cast_function_handle function,
                                                      duckdb_v2_opaque *user_data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(user_data);
	return WithErrorHandler(err, [&]() {
		Convert(function)->user_data =
		    duckdb::make_shared_ptr<CV2UserData>(user_data->ptr, user_data->destroy, user_data->equals);
	});
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_set_exec_callback(duckdb_v2_cast_function_handle function,
                                                          duckdb_v2_cast_function_exec_callback_fn callback,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->exec_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_set_property(duckdb_v2_cast_function_handle function,
                                                     DUCKDB_V2_FUNCTION_PROPERTY_KEY key,
                                                     DUCKDB_V2_FUNCTION_PROPERTY_VALUE value,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		if (key != DUCKDB_V2_FUNCTION_PROPERTY_STABILITY) {
			throw duckdb::InvalidInputException("Only the stability property is valid for cast functions");
		}
		SetScalarFunctionProperty(Convert(function)->properties, key, value);
	});
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_set_bind_callback(duckdb_v2_cast_function_handle function,
                                                          duckdb_v2_cast_function_bind_callback_fn callback,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->bind_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_bind_get_user_data(duckdb_v2_cast_function_bind_info_handle info, void **data,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_bind_get_source_type(duckdb_v2_cast_function_bind_info_handle info,
                                                             duckdb_v2_logical_type_handle *type,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(type);
	*type = nullptr;
	return WithErrorHandler(err, [&]() { *type = Convert(Convert(info)->source_type); });
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_bind_get_target_type(duckdb_v2_cast_function_bind_info_handle info,
                                                             duckdb_v2_logical_type_handle *type,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(type);
	*type = nullptr;
	return WithErrorHandler(err, [&]() { *type = Convert(Convert(info)->target_type); });
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_bind_set_bind_data(duckdb_v2_cast_function_bind_info_handle info,
                                                           duckdb_v2_opaque *data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() {
		Convert(info)->out_bind_data =
		    data->ptr ? duckdb::make_shared_ptr<CV2UserData>(data->ptr, data->destroy, data->equals) : nullptr;
	});
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_exec_get_bind_data(duckdb_v2_cast_function_exec_info_handle info, void **data,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_exec_get_user_data(duckdb_v2_cast_function_exec_info_handle info, void **data,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_exec_get_row_count(duckdb_v2_cast_function_exec_info_handle info, idx_t *count,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(count);
	return WithErrorHandler(err, [&]() { *count = Convert(info)->count; });
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_exec_get_input(duckdb_v2_cast_function_exec_info_handle info,
                                                       duckdb_v2_vector_handle *vector,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(vector);
	return WithErrorHandler(err, [&]() { *vector = Convert(Convert(info)->input); });
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_exec_get_output(duckdb_v2_cast_function_exec_info_handle info,
                                                        duckdb_v2_vector_handle *vector,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(vector);
	return WithErrorHandler(err, [&]() { *vector = Convert(Convert(info)->output); });
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_exec_get_mode(duckdb_v2_cast_function_exec_info_handle info,
                                                      DUCKDB_V2_CAST_MODE *mode, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(mode);
	return WithErrorHandler(err, [&]() { *mode = Convert(info)->mode; });
}

DUCKDB_V2_ERROR duckdb_v2_connection_register_cast_function(duckdb_v2_connection_handle conn,
                                                            duckdb_v2_cast_function_handle function,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		auto &self = *Convert(function);
		auto &db = *Convert(conn)->context->db;
		self.Validate(db);
		auto &casts = duckdb::DBConfig::GetConfig(db).GetCastFunctions();
		if (self.bind_cb) {
			casts.RegisterCastFunction(self.source_type, self.target_type, CV2CastBind, self.BuildBindInfo(),
			                           self.implicit_cast_cost);
			return;
		}
		casts.RegisterCastFunction(self.source_type, self.target_type, self.Build(), self.implicit_cast_cost);
	});
}

DUCKDB_V2_ERROR duckdb_v2_extension_register_cast_function(duckdb_v2_extension_handle extension,
                                                           duckdb_v2_cast_function_handle function,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(extension);
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		auto &self = *Convert(function);
		auto &loader = GetExtensionLoader(extension);
		self.Validate(loader.GetDatabaseInstance());
		if (self.bind_cb) {
			loader.RegisterCastFunction(self.source_type, self.target_type, CV2CastBind, self.BuildBindInfo(),
			                            self.implicit_cast_cost);
			return;
		}
		loader.RegisterCastFunction(self.source_type, self.target_type, self.Build(), self.implicit_cast_cost);
	});
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_destroy(duckdb_v2_cast_function_handle *function) {
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
