#include "test_capi_v2.hpp"

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// V2 extension state: connection state with query / transaction / profiling
// callbacks and a destructor, and the instance object cache.
//
// Callbacks avoid Catch assertions: they run inside DuckDB. They record what
// they see in their user data, and the test asserts on it afterwards.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

constexpr const char *STATE_KEY = "v2_test.state";

// Reports a failure through the live error slot a callback receives.
void Fail(duckdb_v2_error_info_handle *err, const char *message) {
	auto text = Convert(message);
	duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_error_info_set_text(*err, &text);
}

// What the state saw, carried in its user data.
struct Recorder {
	std::vector<std::string> events;
	std::string last_query_error;
	bool fail_query_begin = false;
	bool fail_init = false;
	idx_t inits = 0;
	idx_t profiles = 0;
	bool *destroyed = nullptr;
	// filled in from inside a callback, to test the context-based lookups
	bool context_found_self = false;
	void *context_data = nullptr;
	bool context_cache_ok = false;
};

void DestroyRecorder(void *ptr) {
	auto recorder = static_cast<Recorder *>(ptr);
	if (recorder->destroyed) {
		*recorder->destroyed = true;
	}
	delete recorder;
}

void OnQueryBegin(duckdb_v2_context_handle context, void *user_data, duckdb_v2_error_info_handle *err) {
	auto &recorder = *static_cast<Recorder *>(user_data);
	recorder.events.push_back("query_begin");
	auto key = Convert(STATE_KEY);
	duckdb_v2_context_get_state(context, &key, &recorder.context_found_self, &recorder.context_data, nullptr);
	duckdb_v2_object_cache_handle cache = nullptr;
	recorder.context_cache_ok =
	    duckdb_v2_context_get_object_cache(context, &cache, nullptr) == DUCKDB_V2_ERROR_NONE && cache != nullptr;
	if (recorder.fail_query_begin) {
		Fail(err, "query refused by the state");
	}
}

void OnQueryEnd(duckdb_v2_context_handle, void *user_data, const duckdb_v2_str *error) {
	auto &recorder = *static_cast<Recorder *>(user_data);
	recorder.events.push_back("query_end");
	recorder.last_query_error = error ? Convert(*error) : std::string();
}

void OnTransactionBegin(duckdb_v2_context_handle, void *user_data, duckdb_v2_error_info_handle *) {
	static_cast<Recorder *>(user_data)->events.push_back("tx_begin");
}

void OnTransactionCommit(duckdb_v2_context_handle, void *user_data) {
	static_cast<Recorder *>(user_data)->events.push_back("tx_commit");
}

void OnTransactionRollback(duckdb_v2_context_handle, void *user_data) {
	static_cast<Recorder *>(user_data)->events.push_back("tx_rollback");
}

void OnProfile(duckdb_v2_context_state_profile_handle profile, void *user_data, duckdb_v2_error_info_handle *err) {
	static_cast<Recorder *>(user_data)->profiles++;
	auto text = Convert("v2 state profile\n");
	duckdb_v2_context_state_profile_write(profile, &text, err);
}

// Configures a state to record into the Recorder handed in as the init's user data.
void InitRecording(duckdb_v2_context_handle, void *init_user_data, duckdb_v2_context_state_handle state,
                   duckdb_v2_error_info_handle *err) {
	auto recorder = static_cast<Recorder *>(init_user_data);
	recorder->inits++;
	duckdb_v2_opaque data = {recorder, DestroyRecorder, nullptr};
	duckdb_v2_context_state_set_user_data(state, &data, err);
	if (recorder->fail_init) {
		Fail(err, "init refused");
		return;
	}
	duckdb_v2_context_state_set_query_begin_callback(state, OnQueryBegin, err);
	duckdb_v2_context_state_set_query_end_callback(state, OnQueryEnd, err);
	duckdb_v2_context_state_set_transaction_begin_callback(state, OnTransactionBegin, err);
	duckdb_v2_context_state_set_transaction_commit_callback(state, OnTransactionCommit, err);
	duckdb_v2_context_state_set_transaction_rollback_callback(state, OnTransactionRollback, err);
	duckdb_v2_context_state_set_profiling_callback(state, OnProfile, err);
}

// Registers a recording state on the connection, which then owns the recorder.
DUCKDB_V2_ERROR RegisterRecorder(duckdb_v2_connection_handle conn, Recorder *recorder) {
	auto key = Convert(STATE_KEY);
	return duckdb_v2_connection_get_or_create_state(conn, &key, InitRecording, recorder, nullptr, nullptr);
}

idx_t Count(const Recorder &recorder, const std::string &event) {
	idx_t count = 0;
	for (auto &entry : recorder.events) {
		count += entry == event ? 1 : 0;
	}
	return count;
}

} // namespace

TEST_CASE("V2 connection state: query and transaction callbacks", "[capi_v2][state]") {
	EnvFixture fx;
	auto recorder = new Recorder();
	REQUIRE(RegisterRecorder(fx.conn, recorder) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(RunSql(fx.conn, "SELECT 42") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Count(*recorder, "query_begin") == 1);
	REQUIRE(Count(*recorder, "query_end") == 1);
	REQUIRE(recorder->last_query_error.empty());
	// from inside the callback, the context reaches the state and the object cache
	REQUIRE(recorder->context_found_self);
	REQUIRE(recorder->context_data == recorder);
	REQUIRE(recorder->context_cache_ok);

	recorder->events.clear();
	REQUIRE(RunSql(fx.conn, "BEGIN") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RunSql(fx.conn, "SELECT 1") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RunSql(fx.conn, "COMMIT") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Count(*recorder, "tx_begin") == 1);
	REQUIRE(Count(*recorder, "tx_commit") == 1);
	REQUIRE(Count(*recorder, "tx_rollback") == 0);

	recorder->events.clear();
	REQUIRE(RunSql(fx.conn, "BEGIN") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RunSql(fx.conn, "ROLLBACK") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(Count(*recorder, "tx_begin") == 1);
	REQUIRE(Count(*recorder, "tx_commit") == 0);
	REQUIRE(Count(*recorder, "tx_rollback") == 1);

	// a failing query still ends, and tells how
	recorder->events.clear();
	REQUIRE(RunSql(fx.conn, "SELECT * FROM v2_state_missing_table") != DUCKDB_V2_ERROR_NONE);
	REQUIRE(Count(*recorder, "query_end") == 1);
	REQUIRE(recorder->last_query_error.find("v2_state_missing_table") != std::string::npos);
}

TEST_CASE("V2 connection state: created once per key, destroyed with the connection", "[capi_v2][state]") {
	EnvFixture fx;
	bool destroyed = false;
	auto recorder = new Recorder();
	recorder->destroyed = &destroyed;
	auto key = Convert(STATE_KEY);

	void *data = nullptr;
	REQUIRE(duckdb_v2_connection_get_or_create_state(fx.conn, &key, InitRecording, recorder, &data, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(data == recorder);
	REQUIRE(recorder->inits == 1);

	// the second get finds the state: no init, same data
	auto other = new Recorder();
	data = nullptr;
	REQUIRE(duckdb_v2_connection_get_or_create_state(fx.conn, &key, InitRecording, other, &data, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(data == recorder);
	REQUIRE(other->inits == 0);
	delete other;

	bool found = false;
	data = nullptr;
	REQUIRE(duckdb_v2_connection_get_state(fx.conn, &key, &found, &data, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(found);
	REQUIRE(data == recorder);

	auto missing = Convert("v2_test.missing");
	REQUIRE(duckdb_v2_connection_get_state(fx.conn, &missing, &found, &data, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE_FALSE(found);

	// another connection has its own state
	duckdb_v2_connection_handle conn2 = nullptr;
	REQUIRE(duckdb_v2_connection_create(fx.instance, &conn2, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_get_state(conn2, &key, &found, nullptr, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE_FALSE(found);
	duckdb_v2_connection_destroy(&conn2);
	REQUIRE_FALSE(destroyed);

	duckdb_v2_connection_destroy(&fx.conn);
	REQUIRE(destroyed);
}

TEST_CASE("V2 connection state: remove", "[capi_v2][state]") {
	EnvFixture fx;
	bool destroyed = false;
	auto recorder = new Recorder();
	recorder->destroyed = &destroyed;
	REQUIRE(RegisterRecorder(fx.conn, recorder) == DUCKDB_V2_ERROR_NONE);

	auto key = Convert(STATE_KEY);
	REQUIRE(duckdb_v2_connection_remove_state(fx.conn, &key, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(destroyed);
	bool found = true;
	REQUIRE(duckdb_v2_connection_get_state(fx.conn, &key, &found, nullptr, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE_FALSE(found);
	// removing again is fine
	REQUIRE(duckdb_v2_connection_remove_state(fx.conn, &key, nullptr) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2 connection state: failures", "[capi_v2][state]") {
	EnvFixture fx;
	auto key = Convert(STATE_KEY);

	SECTION("a failing init registers nothing and releases the user data") {
		bool destroyed = false;
		auto recorder = new Recorder();
		recorder->destroyed = &destroyed;
		recorder->fail_init = true;
		REQUIRE(RegisterRecorder(fx.conn, recorder) != DUCKDB_V2_ERROR_NONE);
		REQUIRE(destroyed);
		bool found = true;
		REQUIRE(duckdb_v2_connection_get_state(fx.conn, &key, &found, nullptr, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE_FALSE(found);
	}

	SECTION("an error from the query begin callback fails the query") {
		auto recorder = new Recorder();
		recorder->fail_query_begin = true;
		REQUIRE(RegisterRecorder(fx.conn, recorder) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(RunSql(fx.conn, "SELECT 1") != DUCKDB_V2_ERROR_NONE);
		recorder->fail_query_begin = false;
		REQUIRE(RunSql(fx.conn, "SELECT 1") == DUCKDB_V2_ERROR_NONE);
	}

	SECTION("null arguments") {
		REQUIRE(duckdb_v2_connection_get_or_create_state(fx.conn, nullptr, InitRecording, nullptr, nullptr, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(duckdb_v2_connection_get_or_create_state(fx.conn, &key, nullptr, nullptr, nullptr, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(duckdb_v2_connection_get_state(fx.conn, &key, nullptr, nullptr, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
	}
}

TEST_CASE("V2 connection state: profiling", "[capi_v2][state]") {
	EnvFixture fx;
	auto recorder = new Recorder();
	REQUIRE(RegisterRecorder(fx.conn, recorder) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RunSql(fx.conn, "EXPLAIN ANALYZE SELECT 42") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(recorder->profiles >= 1);
}

namespace {

struct CacheData {
	std::string value;
	bool *destroyed = nullptr;
};

void DestroyCacheData(void *ptr) {
	auto data = static_cast<CacheData *>(ptr);
	if (data->destroyed) {
		*data->destroyed = true;
	}
	delete data;
}

struct Creator {
	idx_t creations = 0;
	idx_t size_bytes = 0;
	bool fail = false;
	bool *destroyed = nullptr;
};

void CreateCacheData(void *user_data, duckdb_v2_opaque *data, idx_t *size_bytes, duckdb_v2_error_info_handle *err) {
	auto &creator = *static_cast<Creator *>(user_data);
	creator.creations++;
	auto created = new CacheData {"created", creator.destroyed};
	*data = {created, DestroyCacheData, nullptr};
	*size_bytes = creator.size_bytes;
	if (creator.fail) {
		Fail(err, "creation refused");
	}
}

std::string ValueOf(duckdb_v2_object_cache_entry_handle entry) {
	void *data = nullptr;
	REQUIRE(duckdb_v2_object_cache_entry_get_data(entry, &data, nullptr) == DUCKDB_V2_ERROR_NONE);
	return static_cast<CacheData *>(data)->value;
}

} // namespace

TEST_CASE("V2 object cache: put, get, delete", "[capi_v2][state][object_cache]") {
	EnvFixture fx;
	duckdb_v2_object_cache_handle cache = nullptr;
	REQUIRE(duckdb_v2_instance_get_object_cache(fx.instance, &cache, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(cache != nullptr);

	auto type = Convert("v2_test.footer");
	auto key = Convert("file.parquet");

	duckdb_v2_object_cache_entry_handle entry = nullptr;
	REQUIRE(duckdb_v2_object_cache_get(cache, &type, &key, &entry, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(entry == nullptr);

	bool destroyed = false;
	duckdb_v2_opaque data = {new CacheData {"first", &destroyed}, DestroyCacheData, nullptr};
	// evictable
	REQUIRE(duckdb_v2_object_cache_put(cache, &type, &key, &data, 64, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_object_cache_get(cache, &type, &key, &entry, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(entry != nullptr);
	REQUIRE(ValueOf(entry) == "first");

	// the same key under another type is another entry
	auto other_type = Convert("v2_test.other");
	duckdb_v2_object_cache_entry_handle other = nullptr;
	REQUIRE(duckdb_v2_object_cache_get(cache, &other_type, &key, &other, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(other == nullptr);

	// a replaced entry lives on while it is held
	duckdb_v2_opaque replacement = {new CacheData {"second"}, DestroyCacheData, nullptr};
	REQUIRE(duckdb_v2_object_cache_put(cache, &type, &key, &replacement, 0, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE_FALSE(destroyed);
	REQUIRE(ValueOf(entry) == "first");
	REQUIRE(duckdb_v2_object_cache_entry_destroy(&entry) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(entry == nullptr);
	REQUIRE(destroyed);

	REQUIRE(duckdb_v2_object_cache_get(cache, &type, &key, &entry, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(ValueOf(entry) == "second");
	duckdb_v2_object_cache_entry_destroy(&entry);

	REQUIRE(duckdb_v2_object_cache_delete(cache, &type, &key, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_object_cache_get(cache, &type, &key, &entry, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(entry == nullptr);
	// deleting what is not there is fine
	REQUIRE(duckdb_v2_object_cache_delete(cache, &type, &key, nullptr) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2 object cache: get or create", "[capi_v2][state][object_cache]") {
	EnvFixture fx;
	duckdb_v2_object_cache_handle cache = nullptr;
	REQUIRE(duckdb_v2_instance_get_object_cache(fx.instance, &cache, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto type = Convert("v2_test.created");
	auto key = Convert("k");

	Creator creator;
	duckdb_v2_object_cache_entry_handle first = nullptr;
	duckdb_v2_object_cache_entry_handle second = nullptr;
	REQUIRE(duckdb_v2_object_cache_get_or_create(cache, &type, &key, CreateCacheData, &creator, &first, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_object_cache_get_or_create(cache, &type, &key, CreateCacheData, &creator, &second, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(creator.creations == 1);
	REQUIRE(ValueOf(first) == "created");
	void *first_data = nullptr;
	void *second_data = nullptr;
	duckdb_v2_object_cache_entry_get_data(first, &first_data, nullptr);
	duckdb_v2_object_cache_entry_get_data(second, &second_data, nullptr);
	REQUIRE(first_data == second_data);
	duckdb_v2_object_cache_entry_destroy(&first);
	duckdb_v2_object_cache_entry_destroy(&second);

	SECTION("a failing creation caches nothing") {
		auto failing_key = Convert("failing");
		bool destroyed = false;
		Creator failing;
		failing.fail = true;
		failing.destroyed = &destroyed;
		duckdb_v2_object_cache_entry_handle entry = nullptr;
		REQUIRE(duckdb_v2_object_cache_get_or_create(cache, &type, &failing_key, CreateCacheData, &failing, &entry,
		                                             nullptr) != DUCKDB_V2_ERROR_NONE);
		REQUIRE(entry == nullptr);
		REQUIRE(destroyed);
		REQUIRE(duckdb_v2_object_cache_get(cache, &type, &failing_key, &entry, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(entry == nullptr);
	}
}

TEST_CASE("V2 object cache: entries die with the instance", "[capi_v2][state][object_cache]") {
	duckdb_v2_environment_handle env = nullptr;
	duckdb_v2_instance_handle instance = nullptr;
	REQUIRE(duckdb_v2_environment_create(&env, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(OpenInstance(env, duckdb_v2_str {nullptr, 0}, &instance, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_object_cache_handle cache = nullptr;
	REQUIRE(duckdb_v2_instance_get_object_cache(instance, &cache, nullptr) == DUCKDB_V2_ERROR_NONE);

	auto type = Convert("v2_test.lifetime");
	auto pinned_key = Convert("pinned");
	auto evictable_key = Convert("evictable");
	bool pinned_destroyed = false;
	bool evictable_destroyed = false;
	duckdb_v2_opaque pinned = {new CacheData {"pinned", &pinned_destroyed}, DestroyCacheData, nullptr};
	duckdb_v2_opaque evictable = {new CacheData {"evictable", &evictable_destroyed}, DestroyCacheData, nullptr};
	REQUIRE(duckdb_v2_object_cache_put(cache, &type, &pinned_key, &pinned, 0, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_object_cache_put(cache, &type, &evictable_key, &evictable, 128, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE_FALSE(pinned_destroyed);
	REQUIRE_FALSE(evictable_destroyed);

	duckdb_v2_instance_destroy(&instance);
	REQUIRE(pinned_destroyed);
	REQUIRE(evictable_destroyed);
	duckdb_v2_environment_destroy(&env);
}

} // namespace test_capi_v2
