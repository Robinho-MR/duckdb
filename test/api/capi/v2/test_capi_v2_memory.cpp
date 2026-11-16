#include "test_capi_v2.hpp"

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// V2 memory reservations (context_reserve_memory) and the table function metrics callback. A context only exists
// inside a callback, so both are driven through small table functions. Callbacks avoid Catch assertions: they latch
// what they observe into file-scope statics, which the test asserts after the query.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

struct ReserveBind {
	int64_t bytes = 0;
};
struct ReserveGlobal {
	duckdb_v2_memory_reservation_handle reservation = nullptr;
	bool done = false;
	~ReserveGlobal() {
		duckdb_v2_memory_reservation_destroy(&reservation);
	}
};

struct Observed {
	std::vector<idx_t> sizes;
	DUCKDB_V2_ERROR grow_rc = DUCKDB_V2_ERROR_NONE;
	std::string grow_error;
};
Observed observed;
bool bad_metric_refused = false;
// Metric names the metrics callback reports, one value each
std::vector<std::pair<std::string, uint64_t>> metrics_to_report;

duckdb_v2_table_function_handle MakeFunction(duckdb_v2_connection_handle conn, std::string_view name) {
	duckdb_v2_table_function_handle function = nullptr;
	REQUIRE(duckdb_v2_table_function_create(FactoryOf(conn), &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_name(function, name.data(), name.size(), nullptr) == DUCKDB_V2_ERROR_NONE);
	return function;
}

void AddBigintColumn(duckdb_v2_table_function_bind_info_handle result, duckdb_v2_context_handle context,
                     duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	duckdb_v2_logical_type_handle bigint = nullptr;
	if (duckdb_v2_factory_create_type_from_id(factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, nullptr, nullptr, nullptr, 0,
	                                          &bigint, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_table_function_bind_add_result_column(result, "i", 1, bigint, err);
	duckdb_v2_logical_type_destroy(&bigint);
}

void ReserveBindCb(duckdb_v2_function_bind_info_handle info, duckdb_v2_table_function_bind_info_handle result,
                   duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	duckdb_v2_value_handle value = nullptr;
	if (duckdb_v2_function_bind_get_arg_value(info, 0, &value, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	int64_t bytes = 0;
	auto rc = duckdb_v2_value_get_bigint(value, &bytes, err);
	duckdb_v2_value_destroy(&value);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	AddBigintColumn(result, context, err);
	duckdb_v2_opaque bind_data = {new ReserveBind {bytes}, [](void *ptr) { delete static_cast<ReserveBind *>(ptr); },
	                              nullptr};
	duckdb_v2_function_bind_set_bind_data(info, &bind_data, err);
}

void ReserveInitGlobalCb(duckdb_v2_table_function_init_global_info_handle info, duckdb_v2_context_handle context,
                         duckdb_v2_error_info_handle *err) {
	void *bind_ptr = nullptr;
	if (duckdb_v2_table_function_init_global_get_bind_data(info, &bind_ptr, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto global = new ReserveGlobal();
	duckdb_v2_opaque state = {global, [](void *ptr) { delete static_cast<ReserveGlobal *>(ptr); }, nullptr};
	// the state owns whatever was reserved, so a failure below still releases it
	if (duckdb_v2_table_function_init_global_set_global_state(info, &state, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_context_reserve_memory(context, static_cast<idx_t>(static_cast<ReserveBind *>(bind_ptr)->bytes),
	                                 &global->reservation, err);
}

void ReserveExecCb(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle,
                   duckdb_v2_error_info_handle *err) {
	void *bind_ptr = nullptr;
	void *global_ptr = nullptr;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	if (duckdb_v2_table_function_exec_get_bind_data(info, &bind_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_global_state(info, &global_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_exec_get_output_chunk(info, &chunk, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &global = *static_cast<ReserveGlobal *>(global_ptr);
	auto bytes = static_cast<idx_t>(static_cast<ReserveBind *>(bind_ptr)->bytes);
	if (!global.done) {
		global.done = true;
		idx_t size = 0;
		duckdb_v2_memory_reservation_get_size(global.reservation, &size, nullptr);
		observed.sizes.push_back(size);
		// grow, then shrink
		duckdb_v2_error_info_handle grow_err = nullptr;
		observed.grow_rc = duckdb_v2_memory_reservation_resize(global.reservation, bytes * 2, &grow_err);
		if (grow_err) {
			const char *text = nullptr;
			idx_t length = 0;
			duckdb_v2_error_info_get_text(grow_err, &text, &length);
			observed.grow_error = std::string(text, length);
			duckdb_v2_error_info_destroy(&grow_err);
		}
		duckdb_v2_memory_reservation_get_size(global.reservation, &size, nullptr);
		observed.sizes.push_back(size);
		duckdb_v2_memory_reservation_resize(global.reservation, bytes / 2, nullptr);
		duckdb_v2_memory_reservation_get_size(global.reservation, &size, nullptr);
		observed.sizes.push_back(size);
	}
	duckdb_v2_vector_handle vec = nullptr;
	if (duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_vector_set_size(vec, 0, err);
}

void EmptyBindCb(duckdb_v2_function_bind_info_handle, duckdb_v2_table_function_bind_info_handle result,
                 duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	AddBigintColumn(result, context, err);
}

void EmptyExecCb(duckdb_v2_table_function_exec_info_handle info, duckdb_v2_context_handle,
                 duckdb_v2_error_info_handle *err) {
	duckdb_v2_data_chunk_handle chunk = nullptr;
	duckdb_v2_vector_handle vec = nullptr;
	if (duckdb_v2_table_function_exec_get_output_chunk(info, &chunk, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_vector_set_size(vec, 0, err);
}

void MetricsCb(duckdb_v2_table_function_metrics_info_handle info, duckdb_v2_context_handle,
               duckdb_v2_error_info_handle *err) {
	// an unknown name is refused, and does not fail the callback
	duckdb_v2_error_info_handle refused = nullptr;
	const char *bad = "operator.no_such_metric";
	if (duckdb_v2_table_function_metrics_set(info, bad, std::string_view(bad).size(), 1, &refused) ==
	    DUCKDB_V2_ERROR_INPUT_INVALID) {
		bad_metric_refused = true;
	}
	duckdb_v2_error_info_destroy(&refused);
	// the bind data was not set, and the global state does not exist for a scan without one
	void *data = reinterpret_cast<void *>(1);
	duckdb_v2_table_function_metrics_get_global_state(info, &data, nullptr);
	if (data != nullptr) {
		bad_metric_refused = false;
	}
	for (auto &metric : metrics_to_report) {
		if (duckdb_v2_table_function_metrics_set(info, metric.first.data(), metric.first.size(), metric.second, err) !=
		    DUCKDB_V2_ERROR_NONE) {
			return;
		}
	}
}

} // namespace

TEST_CASE("V2 memory: reserve, resize and release", "[capi_v2][memory]") {
	EnvFixture fx;
	ExecSQL(fx.conn, "SET threads=1");

	auto bigint = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	auto function = MakeFunction(fx.conn, "capi_reserve");
	duckdb_v2_function_signature_handle sig = nullptr;
	REQUIRE(duckdb_v2_table_function_get_signature(function, &sig, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, "n", 1, bigint, nullptr,
	                                                   DUCKDB_V2_FUNCTION_PARAMETER_KIND_STANDARD,
	                                                   nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_destroy(&bigint);
	REQUIRE(duckdb_v2_table_function_set_bind_callback(function, ReserveBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_init_global_callback(function, ReserveInitGlobalCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(function, ReserveExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&function);

	observed = Observed {};
	ExecSQL(fx.conn, "SELECT * FROM capi_reserve(10000000)");
	REQUIRE(observed.sizes == std::vector<idx_t> {10000000, 20000000, 5000000});
	REQUIRE(observed.grow_rc == DUCKDB_V2_ERROR_NONE);

	// the reservation counts against the limit and is released with the query
	ExecSQL(fx.conn, "SET memory_limit='50MB'");
	REQUIRE(RunSql(fx.conn, "SELECT * FROM capi_reserve(1000000000)") == DUCKDB_V2_ERROR_RESOURCE_OUT_OF_MEMORY);
	ExecSQL(fx.conn, "SELECT * FROM capi_reserve(20000000)");

	// growing past the limit fails with out of memory and leaves the reservation as it was
	observed = Observed {};
	ExecSQL(fx.conn, "SELECT * FROM capi_reserve(40000000)");
	REQUIRE(observed.grow_rc == DUCKDB_V2_ERROR_RESOURCE_OUT_OF_MEMORY);
	REQUIRE(observed.grow_error.find("Out of Memory") != std::string::npos);
	REQUIRE(observed.sizes == std::vector<idx_t> {40000000, 40000000, 20000000});

	// everything was released
	duckdb_v2_result_handle r = nullptr;
	REQUIRE(Query(fx.conn,
	              "SELECT coalesce(sum(memory_usage_bytes), 0)::BIGINT FROM duckdb_memory() WHERE tag = 'EXTENSION'",
	              &r) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(r);
	REQUIRE(chunk != nullptr);
	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
	duckdb_v2_vector_view view {};
	duckdb_v2_vector_get_view(vec, &view, nullptr);
	REQUIRE(static_cast<const int64_t *>(view.data)[SelAt(view.sel, 0)] == 0);
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&r);
}

TEST_CASE("V2 memory: null arguments", "[capi_v2][memory]") {
	EnvFixture fx;
	duckdb_v2_memory_reservation_handle reservation = nullptr;
	idx_t size = 0;
	REQUIRE(duckdb_v2_context_reserve_memory(nullptr, 1, &reservation, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_context_reserve_memory(nullptr, 1, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_memory_reservation_resize(nullptr, 1, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_memory_reservation_get_size(nullptr, &size, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_memory_reservation_get_size(nullptr, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	// destroying nothing is fine
	REQUIRE(duckdb_v2_memory_reservation_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_memory_reservation_destroy(&reservation) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2 table: metrics callback reports bytes scanned", "[capi_v2][table_function]") {
	EnvFixture fx;
	auto function = MakeFunction(fx.conn, "capi_metrics");
	REQUIRE(duckdb_v2_table_function_set_bind_callback(function, EmptyBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_exec_callback(function, EmptyExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_metrics_callback(function, MetricsCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&function);

	// the profiler shows the bytes the function reported
	metrics_to_report = {{"operator.bytes_scanned", 123}};
	bad_metric_refused = false;
	REQUIRE(RunSql(fx.conn, "SET enable_profiling = 'no_output'") == DUCKDB_V2_ERROR_NONE);
	ExecSQL(fx.conn, "SELECT * FROM capi_metrics()");
	REQUIRE(bad_metric_refused);

	duckdb_v2_query_metrics_handle metrics = nullptr;
	REQUIRE(duckdb_v2_connection_get_query_metrics(fx.conn, &metrics, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_query_metrics_node_handle root = nullptr;
	REQUIRE(duckdb_v2_query_metrics_get_root(metrics, &root, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_handle value = nullptr;
	bool present = false;
	std::string_view name = "query.total_bytes_scanned";
	REQUIRE(duckdb_v2_query_metrics_node_get_metric(root, name.data(), name.size(), &value, &present, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(present);
	uint64_t bytes = 0;
	REQUIRE(duckdb_v2_value_get_ubigint(value, &bytes, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_destroy(&value);
	duckdb_v2_query_metrics_destroy(&metrics);
	REQUIRE(bytes == 123);

	// a name that is not an operator metric fails the query
	metrics_to_report = {{"operator.bogus", 1}};
	REQUIRE(RunSql(fx.conn, "SELECT * FROM capi_metrics()") != DUCKDB_V2_ERROR_NONE);
	metrics_to_report.clear();
}

TEST_CASE("V2 table: metrics callback null arguments", "[capi_v2][table_function]") {
	void *data = nullptr;
	REQUIRE(duckdb_v2_table_function_set_metrics_callback(nullptr, MetricsCb, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_metrics_get_user_data(nullptr, &data, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_metrics_get_bind_data(nullptr, &data, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_metrics_get_global_state(nullptr, &data, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_table_function_metrics_set(nullptr, "operator.bytes_scanned", 22, 1, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
}

} // namespace test_capi_v2
