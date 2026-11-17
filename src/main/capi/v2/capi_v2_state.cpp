#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/object_cache.hpp"

namespace duckdb::capiv2 {

//----------------------------------------------------------------------------------------------------------------------
// Connection state
//----------------------------------------------------------------------------------------------------------------------

//! What an init callback configures. Its user data is released when the builder is, i.e. right away if the init fails.
class CV2ContextStateBuilder {
public:
	shared_ptr<CV2UserData> user_data;
	duckdb_v2_context_state_query_begin_callback_fn query_begin_cb = nullptr;
	duckdb_v2_context_state_query_end_callback_fn query_end_cb = nullptr;
	duckdb_v2_context_state_transaction_begin_callback_fn transaction_begin_cb = nullptr;
	duckdb_v2_context_state_transaction_end_callback_fn transaction_commit_cb = nullptr;
	duckdb_v2_context_state_transaction_end_callback_fn transaction_rollback_cb = nullptr;
	duckdb_v2_context_state_profiling_callback_fn profiling_cb = nullptr;
};

static auto Convert(duckdb_v2_context_state_handle state) -> CV2ContextStateBuilder * {
	return reinterpret_cast<CV2ContextStateBuilder *>(state);
}
static auto Convert(CV2ContextStateBuilder *state) -> duckdb_v2_context_state_handle {
	return reinterpret_cast<duckdb_v2_context_state_handle>(state);
}

//! The profile being written: the stream the registered states append to.
class CV2ContextStateProfile {
public:
	explicit CV2ContextStateProfile(std::ostream &ss) : ss(ss) {
	}

	std::ostream &ss;
};

static auto Convert(duckdb_v2_context_state_profile_handle profile) -> CV2ContextStateProfile * {
	return reinterpret_cast<CV2ContextStateProfile *>(profile);
}
static auto Convert(CV2ContextStateProfile *profile) -> duckdb_v2_context_state_profile_handle {
	return reinterpret_cast<duckdb_v2_context_state_profile_handle>(profile);
}

//! Runs a callback taking an error slot, and throws what it reports.
template <class FUNC>
static void InvokeWithErrorSlot(FUNC &&fn) {
	CV2ErrorInfo info;
	auto handle = Convert(&info);
	fn(&handle);
	if (info.HasError()) {
		info.ThrowAsException();
	}
}

//! Connection state set up through the C API: forwards ClientContextState's hooks to the callbacks the init set.
class CV2ContextState final : public ClientContextState {
public:
	CV2ContextState(CV2Context &context, duckdb_v2_context_state_init_callback_fn init, void *init_user_data) {
		CV2ContextStateBuilder builder;
		InvokeWithErrorSlot(
		    [&](duckdb_v2_error_info_handle *err) { init(Convert(&context), init_user_data, Convert(&builder), err); });
		callbacks = std::move(builder);
	}

	void *GetUserData() const {
		return callbacks.user_data ? callbacks.user_data->GetData() : nullptr;
	}

	using ClientContextState::QueryEnd;
	using ClientContextState::TransactionRollback;

	void QueryBegin(ClientContext &context) override {
		if (!callbacks.query_begin_cb) {
			return;
		}
		CV2CallbackContext handle(context);
		InvokeWithErrorSlot(
		    [&](duckdb_v2_error_info_handle *err) { callbacks.query_begin_cb(Convert(&handle), GetUserData(), err); });
	}

	void QueryEnd(ClientContext &context, optional_ptr<ErrorData> error) override {
		if (!callbacks.query_end_cb) {
			return;
		}
		CV2CallbackContext handle(context);
		if (error && error->HasError()) {
			auto message = error->Message();
			duckdb_v2_str message_view {message.data(), message.size()};
			callbacks.query_end_cb(Convert(&handle), GetUserData(), &message_view);
		} else {
			callbacks.query_end_cb(Convert(&handle), GetUserData(), nullptr);
		}
	}

	void TransactionBegin(MetaTransaction &transaction, ClientContext &context) override {
		if (!callbacks.transaction_begin_cb) {
			return;
		}
		CV2CallbackContext handle(context);
		InvokeWithErrorSlot([&](duckdb_v2_error_info_handle *err) {
			callbacks.transaction_begin_cb(Convert(&handle), GetUserData(), err);
		});
	}

	void TransactionCommit(MetaTransaction &transaction, ClientContext &context) override {
		if (callbacks.transaction_commit_cb) {
			CV2CallbackContext handle(context);
			callbacks.transaction_commit_cb(Convert(&handle), GetUserData());
		}
	}

	void TransactionRollback(MetaTransaction &transaction, ClientContext &context,
	                         optional_ptr<ErrorData> error) override {
		if (callbacks.transaction_rollback_cb) {
			CV2CallbackContext handle(context);
			callbacks.transaction_rollback_cb(Convert(&handle), GetUserData());
		}
	}

	void WriteProfilingInformation(std::ostream &ss) override {
		if (!callbacks.profiling_cb) {
			return;
		}
		CV2ContextStateProfile profile(ss);
		CV2ErrorInfo info;
		auto handle = Convert(&info);
		// an error is dropped: the profile is written whatever the state adds to it
		callbacks.profiling_cb(Convert(&profile), GetUserData(), &handle);
	}

private:
	CV2ContextStateBuilder callbacks;
};

static auto ToKey(const duckdb_v2_str *key) -> string {
	return string(key->ptr, key->len);
}

//! The C API state under `key`, or null if the key has none. Throws if it is held by state of something else.
static auto FindState(ClientContext &client, const string &key) -> shared_ptr<CV2ContextState> {
	auto existing = client.registered_state->Get<ClientContextState>(key);
	if (!existing) {
		return nullptr;
	}
	if (!dynamic_cast<CV2ContextState *>(existing.get())) {
		throw InvalidInputException("The key \"%s\" is in use by state that is not registered through the C API", key);
	}
	return shared_ptr_cast<ClientContextState, CV2ContextState>(std::move(existing));
}

static void GetOrCreateState(ClientContext &client, CV2Context &context, const duckdb_v2_str *key_view,
                             duckdb_v2_context_state_init_callback_fn init, void *init_user_data, void **data) {
	auto key = ToKey(key_view);
	auto state = FindState(client, key);
	if (!state) {
		state = client.registered_state->GetOrCreate<CV2ContextState>(key, context, init, init_user_data);
	}
	if (data) {
		*data = state->GetUserData();
	}
}

static void GetState(ClientContext &client, const duckdb_v2_str *key_view, bool *found, void **data) {
	auto state = FindState(client, ToKey(key_view));
	*found = state != nullptr;
	if (state && data) {
		*data = state->GetUserData();
	}
}

static void RemoveState(ClientContext &client, const duckdb_v2_str *key_view) {
	auto key = ToKey(key_view);
	// refuse to remove what the C API did not register
	FindState(client, key);
	client.registered_state->Remove(key);
}

//----------------------------------------------------------------------------------------------------------------------
// Object cache
//----------------------------------------------------------------------------------------------------------------------

//! An entry of the object cache created through the C API: opaque data plus the memory it is accounted as.
class CV2ObjectCacheEntry final : public ObjectCacheEntry {
public:
	//! Wraps data that already exists.
	CV2ObjectCacheEntry(CV2UserData data_p, idx_t size_bytes) : data(std::move(data_p)), size_bytes(size_bytes) {
	}

	//! Creates the data through a callback.
	CV2ObjectCacheEntry(duckdb_v2_object_cache_create_callback_fn create, void *create_user_data) {
		duckdb_v2_opaque created = {nullptr, nullptr, nullptr};
		idx_t created_size = 0;
		try {
			InvokeWithErrorSlot(
			    [&](duckdb_v2_error_info_handle *err) { create(create_user_data, &created, &created_size, err); });
		} catch (...) {
			// whatever the callback created before failing goes with it
			CV2UserData release(created.ptr, created.destroy, created.equals);
			throw;
		}
		data = CV2UserData(created.ptr, created.destroy, created.equals);
		size_bytes = created_size;
	}

	static string ObjectType() {
		return "capi_v2";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		// no size pins the entry
		return size_bytes == 0 ? optional_idx() : optional_idx(size_bytes);
	}
	void *GetData() const {
		return data.GetData();
	}

private:
	CV2UserData data;
	idx_t size_bytes = 0;
};

//! The handle of an entry held by a caller: pins the data beyond the cache's own hold on it.
struct CV2ObjectCacheEntryHandle {
	shared_ptr<CV2ObjectCacheEntry> entry;
};

static auto Convert(duckdb_v2_object_cache_entry_handle entry) -> CV2ObjectCacheEntryHandle * {
	return reinterpret_cast<CV2ObjectCacheEntryHandle *>(entry);
}
static auto Convert(CV2ObjectCacheEntryHandle *entry) -> duckdb_v2_object_cache_entry_handle {
	return reinterpret_cast<duckdb_v2_object_cache_entry_handle>(entry);
}

static auto Convert(duckdb_v2_object_cache_handle cache) -> ObjectCache * {
	return reinterpret_cast<ObjectCache *>(cache);
}
static auto Convert(ObjectCache *cache) -> duckdb_v2_object_cache_handle {
	return reinterpret_cast<duckdb_v2_object_cache_handle>(cache);
}

//! Entries are looked up by type and key together; the length prefix keeps the two apart, and the C API prefix keeps
//! them apart from the cache's other users.
static auto CacheKey(const duckdb_v2_str *type, const duckdb_v2_str *key) -> string {
	return StringUtil::Format("capi_v2:%llu:%s:%s", static_cast<unsigned long long>(type->len),
	                          string(type->ptr, type->len), string(key->ptr, key->len));
}

static void GiveEntryToCaller(shared_ptr<CV2ObjectCacheEntry> entry, duckdb_v2_object_cache_entry_handle *out) {
	if (!entry) {
		return;
	}
	auto handle = make_uniq<CV2ObjectCacheEntryHandle>();
	handle->entry = std::move(entry);
	*out = Convert(handle.release());
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_context_get_or_create_state(duckdb_v2_context_handle context, const duckdb_v2_str *key,
                                                      duckdb_v2_context_state_init_callback_fn init,
                                                      void *init_user_data, void **data,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(context);
	DUCKDB_CHECK_ARG(key);
	DUCKDB_CHECK_ARG(init);
	if (data) {
		*data = nullptr;
	}
	return WithErrorHandler(err, [&]() {
		GetOrCreateState(Convert(context)->context, *Convert(context), key, init, init_user_data, data);
	});
}

DUCKDB_V2_ERROR duckdb_v2_connection_get_or_create_state(duckdb_v2_connection_handle conn, const duckdb_v2_str *key,
                                                         duckdb_v2_context_state_init_callback_fn init,
                                                         void *init_user_data, void **data,
                                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(key);
	DUCKDB_CHECK_ARG(init);
	if (data) {
		*data = nullptr;
	}
	return WithErrorHandler(err, [&]() {
		auto &connection = *Convert(conn);
		GetOrCreateState(*connection.context, connection.context_handle, key, init, init_user_data, data);
	});
}

DUCKDB_V2_ERROR duckdb_v2_context_get_state(duckdb_v2_context_handle context, const duckdb_v2_str *key, bool *found,
                                            void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(context);
	DUCKDB_CHECK_ARG(key);
	DUCKDB_CHECK_ARG(found);
	return WithErrorHandler(err, [&]() { GetState(Convert(context)->context, key, found, data); });
}

DUCKDB_V2_ERROR duckdb_v2_connection_get_state(duckdb_v2_connection_handle conn, const duckdb_v2_str *key, bool *found,
                                               void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(key);
	DUCKDB_CHECK_ARG(found);
	return WithErrorHandler(err, [&]() { GetState(*Convert(conn)->context, key, found, data); });
}

DUCKDB_V2_ERROR duckdb_v2_context_remove_state(duckdb_v2_context_handle context, const duckdb_v2_str *key,
                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(context);
	DUCKDB_CHECK_ARG(key);
	return WithErrorHandler(err, [&]() { RemoveState(Convert(context)->context, key); });
}

DUCKDB_V2_ERROR duckdb_v2_connection_remove_state(duckdb_v2_connection_handle conn, const duckdb_v2_str *key,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(key);
	return WithErrorHandler(err, [&]() { RemoveState(*Convert(conn)->context, key); });
}

DUCKDB_V2_ERROR duckdb_v2_context_state_set_user_data(duckdb_v2_context_state_handle state, duckdb_v2_opaque *data,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(state);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() {
		Convert(state)->user_data = duckdb::make_shared_ptr<CV2UserData>(data->ptr, data->destroy, data->equals);
	});
}

DUCKDB_V2_ERROR
duckdb_v2_context_state_set_query_begin_callback(duckdb_v2_context_state_handle state,
                                                 duckdb_v2_context_state_query_begin_callback_fn callback,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(state);
	return WithErrorHandler(err, [&]() { Convert(state)->query_begin_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_context_state_set_query_end_callback(duckdb_v2_context_state_handle state,
                                                               duckdb_v2_context_state_query_end_callback_fn callback,
                                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(state);
	return WithErrorHandler(err, [&]() { Convert(state)->query_end_cb = callback; });
}

DUCKDB_V2_ERROR
duckdb_v2_context_state_set_transaction_begin_callback(duckdb_v2_context_state_handle state,
                                                       duckdb_v2_context_state_transaction_begin_callback_fn callback,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(state);
	return WithErrorHandler(err, [&]() { Convert(state)->transaction_begin_cb = callback; });
}

DUCKDB_V2_ERROR
duckdb_v2_context_state_set_transaction_commit_callback(duckdb_v2_context_state_handle state,
                                                        duckdb_v2_context_state_transaction_end_callback_fn callback,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(state);
	return WithErrorHandler(err, [&]() { Convert(state)->transaction_commit_cb = callback; });
}

DUCKDB_V2_ERROR
duckdb_v2_context_state_set_transaction_rollback_callback(duckdb_v2_context_state_handle state,
                                                          duckdb_v2_context_state_transaction_end_callback_fn callback,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(state);
	return WithErrorHandler(err, [&]() { Convert(state)->transaction_rollback_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_context_state_set_profiling_callback(duckdb_v2_context_state_handle state,
                                                               duckdb_v2_context_state_profiling_callback_fn callback,
                                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(state);
	return WithErrorHandler(err, [&]() { Convert(state)->profiling_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_context_state_profile_write(duckdb_v2_context_state_profile_handle profile,
                                                      const duckdb_v2_str *text, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(profile);
	DUCKDB_CHECK_ARG(text);
	return WithErrorHandler(err,
	                        [&]() { Convert(profile)->ss.write(text->ptr, static_cast<std::streamsize>(text->len)); });
}

DUCKDB_V2_ERROR duckdb_v2_instance_get_object_cache(duckdb_v2_instance_handle instance,
                                                    duckdb_v2_object_cache_handle *cache,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(instance);
	DUCKDB_CHECK_ARG(cache);
	*cache = nullptr;
	return WithErrorHandler(err,
	                        [&]() { *cache = Convert(&Convert(instance)->GetDatabase().instance->GetObjectCache()); });
}

DUCKDB_V2_ERROR duckdb_v2_context_get_object_cache(duckdb_v2_context_handle context,
                                                   duckdb_v2_object_cache_handle *cache,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(context);
	DUCKDB_CHECK_ARG(cache);
	*cache = nullptr;
	return WithErrorHandler(err, [&]() {
		*cache = Convert(&duckdb::DatabaseInstance::GetDatabase(Convert(context)->context).GetObjectCache());
	});
}

DUCKDB_V2_ERROR duckdb_v2_object_cache_put(duckdb_v2_object_cache_handle cache, const duckdb_v2_str *type,
                                           const duckdb_v2_str *key, duckdb_v2_opaque *data, idx_t size_bytes,
                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(cache);
	DUCKDB_CHECK_ARG(type);
	DUCKDB_CHECK_ARG(key);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() {
		// the entry owns the data from here, so a failure below destroys it
		auto entry = duckdb::make_shared_ptr<CV2ObjectCacheEntry>(CV2UserData(data->ptr, data->destroy, data->equals),
		                                                          size_bytes);
		auto cache_key = CacheKey(type, key);
		Convert(cache)->Delete(cache_key);
		Convert(cache)->Put(std::move(cache_key), std::move(entry));
	});
}

DUCKDB_V2_ERROR duckdb_v2_object_cache_get(duckdb_v2_object_cache_handle cache, const duckdb_v2_str *type,
                                           const duckdb_v2_str *key, duckdb_v2_object_cache_entry_handle *entry,
                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(cache);
	DUCKDB_CHECK_ARG(type);
	DUCKDB_CHECK_ARG(key);
	DUCKDB_CHECK_ARG(entry);
	*entry = nullptr;
	return WithErrorHandler(err, [&]() {
		auto found = Convert(cache)->Get<CV2ObjectCacheEntry>(CacheKey(type, key));
		GiveEntryToCaller(std::move(found), entry);
	});
}

DUCKDB_V2_ERROR duckdb_v2_object_cache_get_or_create(duckdb_v2_object_cache_handle cache, const duckdb_v2_str *type,
                                                     const duckdb_v2_str *key,
                                                     duckdb_v2_object_cache_create_callback_fn create,
                                                     void *create_user_data, duckdb_v2_object_cache_entry_handle *entry,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(cache);
	DUCKDB_CHECK_ARG(type);
	DUCKDB_CHECK_ARG(key);
	DUCKDB_CHECK_ARG(create);
	DUCKDB_CHECK_ARG(entry);
	*entry = nullptr;
	return WithErrorHandler(err, [&]() {
		auto found = Convert(cache)->GetOrCreate<CV2ObjectCacheEntry>(CacheKey(type, key), create, create_user_data);
		GiveEntryToCaller(std::move(found), entry);
	});
}

DUCKDB_V2_ERROR duckdb_v2_object_cache_delete(duckdb_v2_object_cache_handle cache, const duckdb_v2_str *type,
                                              const duckdb_v2_str *key, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(cache);
	DUCKDB_CHECK_ARG(type);
	DUCKDB_CHECK_ARG(key);
	return WithErrorHandler(err, [&]() { Convert(cache)->Delete(CacheKey(type, key)); });
}

DUCKDB_V2_ERROR duckdb_v2_object_cache_entry_get_data(duckdb_v2_object_cache_entry_handle entry, void **data,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(entry);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(entry)->entry->GetData(); });
}

DUCKDB_V2_ERROR duckdb_v2_object_cache_entry_destroy(duckdb_v2_object_cache_entry_handle *entry) {
	if (!entry) {
		return DUCKDB_V2_ERROR_INPUT_INVALID;
	}
	return WithErrorHandler(nullptr, [&]() {
		duckdb::unique_ptr<CV2ObjectCacheEntryHandle> owned(Convert(*entry));
		*entry = nullptr;
	});
}
