#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"

namespace duckdb::capiv2 {

//! The callbacks of a cast, shared by its registration and every cast bound from it
class CV2CastCallbacks {
public:
	duckdb_v2_cast_function_bind_callback_fn bind_cb = nullptr;
	duckdb_v2_cast_function_init_callback_fn init_cb = nullptr;
	duckdb_v2_cast_function_exec_callback_fn exec_cb = nullptr;
	shared_ptr<CV2UserData> user_data = nullptr;
	FunctionStability stability = FunctionStability::CONSISTENT;

	void *GetUserData() const {
		return user_data ? user_data->GetData() : nullptr;
	}
};

class CV2CastBoundData final : public BoundCastData {
public:
	CV2CastBoundData(shared_ptr<const CV2CastCallbacks> callbacks, shared_ptr<CV2UserData> bind_data)
	    : callbacks(std::move(callbacks)), bind_data(std::move(bind_data)) {
	}

	auto Copy() const -> unique_ptr<BoundCastData> override {
		return make_uniq<CV2CastBoundData>(callbacks, bind_data);
	}

	void *GetBindData() const {
		return bind_data ? bind_data->GetData() : nullptr;
	}

public:
	shared_ptr<const CV2CastCallbacks> callbacks;
	//! The bind data the bind callback set, or nullptr
	shared_ptr<CV2UserData> bind_data;
};

//! The callbacks of a cast with a bind callback, the bind function of its registration is called with
class CV2CastBindFunctionInfo final : public BindCastInfo {
public:
	explicit CV2CastBindFunctionInfo(shared_ptr<const CV2CastCallbacks> callbacks) : callbacks(std::move(callbacks)) {
	}

	shared_ptr<const CV2CastCallbacks> callbacks;
};

// CastParameters carries no context, so the only place to pick one up is the local state init, which does receive
// one. The state forwards it to the exec callback, next to the init data of the thread.
class CV2CastLocalState final : public FunctionLocalState {
public:
	unique_ptr<CV2CallbackContext> context;
	CV2UserData init_data;
};

class CV2CastBindArgs {
public:
	void *in_user_data = nullptr;
	const LogicalType *source = nullptr;
	const LogicalType *target = nullptr;

	shared_ptr<CV2UserData> out_bind_data;
};

static auto Convert(duckdb_v2_cast_function_bind_info_handle info) -> CV2CastBindArgs * {
	return reinterpret_cast<CV2CastBindArgs *>(info);
}
static auto Convert(CV2CastBindArgs *info) -> duckdb_v2_cast_function_bind_info_handle {
	return reinterpret_cast<duckdb_v2_cast_function_bind_info_handle>(info);
}

class CV2CastInitArgs {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;

	duckdb_v2_opaque out_init_data = {};
};

static auto Convert(duckdb_v2_cast_function_init_info_handle info) -> CV2CastInitArgs * {
	return reinterpret_cast<CV2CastInitArgs *>(info);
}
static auto Convert(CV2CastInitArgs *info) -> duckdb_v2_cast_function_init_info_handle {
	return reinterpret_cast<duckdb_v2_cast_function_init_info_handle>(info);
}

class CV2CastExecInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;
	void *in_init_data = nullptr;

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

	const auto &bound_data = parameters.cast_data->Cast<CV2CastBoundData>();
	auto &callbacks = *bound_data.callbacks;
	if (callbacks.init_cb) {
		CV2CastInitArgs args;
		args.in_user_data = callbacks.GetUserData();
		args.in_bind_data = bound_data.GetBindData();

		CV2ErrorInfo err = {};
		auto err_ptr = Convert(&err);
		callbacks.init_cb(Convert(&args), Convert(state->context.get()), &err_ptr);
		// Keep the init data before throwing, so that it is destroyed with the state
		if (args.out_init_data.ptr) {
			state->init_data = CV2UserData(args.out_init_data.ptr, args.out_init_data.destroy);
		}
		if (err.HasError()) {
			err.ThrowAsException();
		}
	}
	return std::move(state);
}

static auto CV2CastExec(Vector &input, Vector &output, idx_t count, CastParameters &parameters) -> bool {
	const auto &bound_data = parameters.cast_data->Cast<CV2CastBoundData>();
	auto &callbacks = *bound_data.callbacks;

	// A try cast is exactly the case where the engine handed us a slot to write a message into instead of aborting.
	const auto is_try_cast = parameters.error_message != nullptr;

	CV2CastExecInfo args = {};
	args.in_user_data = callbacks.GetUserData();
	args.in_bind_data = bound_data.GetBindData();
	args.input = &input;
	args.output = &output;
	args.count = count;
	args.mode = is_try_cast ? DUCKDB_V2_CAST_MODE_TRY : DUCKDB_V2_CAST_MODE_NORMAL;

	optional_ptr<CV2Context> context = nullptr;
	if (parameters.local_state) {
		auto &local_state = parameters.local_state->Cast<CV2CastLocalState>();
		context = local_state.context.get();
		args.in_init_data = local_state.init_data.GetData();
	}

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	callbacks.exec_cb(Convert(&args), Convert(context.get()), &err_ptr);

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
	auto &callbacks = input.info->Cast<CV2CastBindFunctionInfo>().callbacks;
	shared_ptr<CV2UserData> bind_data;
	if (callbacks->bind_cb) {
		CV2CastBindArgs args;
		args.in_user_data = callbacks->GetUserData();
		args.source = &source;
		args.target = &target;

		unique_ptr<CV2CallbackContext> context;
		if (input.context) {
			context = make_uniq<CV2CallbackContext>(*input.context);
		}
		CV2ErrorInfo err = {};
		auto err_ptr = Convert(&err);
		callbacks->bind_cb(Convert(&args), Convert(context.get()), &err_ptr);
		bind_data = std::move(args.out_bind_data);
		if (err.HasError()) {
			err.ThrowAsException();
		}
	}
	if (callbacks->stability != FunctionStability::CONSISTENT) {
		input.stability = callbacks->stability;
	}
	return BoundCastInfo(CV2CastExec, make_uniq<CV2CastBoundData>(callbacks, std::move(bind_data)),
	                     CV2CastInitLocalState);
}

class CV2CastFunction {
public:
	explicit CV2CastFunction(DatabaseInstance &db) : db(db) {
	}

	//! Validates the configuration and checks that it can be registered on `target`.
	void Verify(DatabaseInstance &target) const {
		CheckRegistrationTarget(db, target, "cast function");
		if (!source_type.IsComplete()) {
			throw InvalidInputException("Source type must be set to a fully defined concrete type");
		}
		if (!target_type.IsComplete()) {
			throw InvalidInputException("Target type must be set to a fully defined concrete type");
		}
		if (!callbacks.exec_cb) {
			throw InvalidInputException("Exec callback must be set for the function.");
		}
	}

	//! Registers the cast on `casts`: bound when a query uses it if it has to be, and once now otherwise.
	template <class REGISTER>
	void Register(DatabaseInstance &target, REGISTER &&register_fn) const {
		Verify(target);
		auto shared_callbacks = make_shared_ptr<const CV2CastCallbacks>(callbacks);
		if (callbacks.bind_cb || callbacks.stability != FunctionStability::CONSISTENT) {
			register_fn(BindCastFunction(CV2CastBind, make_uniq<CV2CastBindFunctionInfo>(shared_callbacks)));
			return;
		}
		register_fn(BoundCastInfo(CV2CastExec, make_uniq<CV2CastBoundData>(shared_callbacks, nullptr),
		                          CV2CastInitLocalState));
	}

public:
	//! The database it was created for: the only one it can be registered on.
	DatabaseInstance &db;
	LogicalType source_type;
	LogicalType target_type;
	int64_t implicit_cast_cost = -1;
	CV2CastCallbacks callbacks;
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
		Convert(function)->callbacks.user_data =
		    duckdb::make_shared_ptr<CV2UserData>(user_data->ptr, user_data->destroy, user_data->equals);
	});
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_set_exec_callback(duckdb_v2_cast_function_handle function,
                                                          duckdb_v2_cast_function_exec_callback_fn callback,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->callbacks.exec_cb = callback; });
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

DUCKDB_V2_ERROR duckdb_v2_cast_function_set_property(duckdb_v2_cast_function_handle function,
                                                     DUCKDB_V2_FUNCTION_PROPERTY_KEY key,
                                                     DUCKDB_V2_FUNCTION_PROPERTY_VALUE value,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		if (key != DUCKDB_V2_FUNCTION_PROPERTY_STABILITY) {
			throw duckdb::InvalidInputException("Only the stability of a cast function can be set");
		}
		duckdb::FunctionProperties props;
		SetScalarFunctionProperty(props, key, value);
		Convert(function)->callbacks.stability = props.GetStability();
	});
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_set_bind_callback(duckdb_v2_cast_function_handle function,
                                                          duckdb_v2_cast_function_bind_callback_fn callback,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->callbacks.bind_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_set_init_callback(duckdb_v2_cast_function_handle function,
                                                          duckdb_v2_cast_function_init_callback_fn callback,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->callbacks.init_cb = callback; });
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
	return WithErrorHandler(err, [&]() { *type = Convert(duckdb::LogicalType(*Convert(info)->source)); });
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_bind_get_target_type(duckdb_v2_cast_function_bind_info_handle info,
                                                             duckdb_v2_logical_type_handle *type,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(type);
	*type = nullptr;
	return WithErrorHandler(err, [&]() { *type = Convert(duckdb::LogicalType(*Convert(info)->target)); });
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_bind_set_bind_data(duckdb_v2_cast_function_bind_info_handle info,
                                                           duckdb_v2_opaque *data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() {
		Convert(info)->out_bind_data = duckdb::make_shared_ptr<CV2UserData>(data->ptr, data->destroy, data->equals);
	});
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_init_get_user_data(duckdb_v2_cast_function_init_info_handle info, void **data,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_init_get_bind_data(duckdb_v2_cast_function_init_info_handle info, void **data,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_init_set_init_data(duckdb_v2_cast_function_init_info_handle info,
                                                           duckdb_v2_opaque *data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() {
		auto &args = *Convert(info);
		if (args.out_init_data.ptr && args.out_init_data.destroy) {
			args.out_init_data.destroy(args.out_init_data.ptr);
		}
		args.out_init_data = *data;
	});
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_exec_get_bind_data(duckdb_v2_cast_function_exec_info_handle info, void **data,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_cast_function_exec_get_init_data(duckdb_v2_cast_function_exec_info_handle info, void **data,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_init_data; });
}

DUCKDB_V2_ERROR duckdb_v2_connection_register_cast_function(duckdb_v2_connection_handle conn,
                                                            duckdb_v2_cast_function_handle function,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		auto &self = *Convert(function);
		auto &db = *Convert(conn)->context->db;
		auto &casts = duckdb::DBConfig::GetConfig(db).GetCastFunctions();
		self.Register(db, [&](auto cast) {
			casts.RegisterCastFunction(self.source_type, self.target_type, std::move(cast), self.implicit_cast_cost);
		});
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
		self.Register(loader.GetDatabaseInstance(), [&](auto cast) {
			loader.RegisterCastFunction(self.source_type, self.target_type, std::move(cast), self.implicit_cast_cost);
		});
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
