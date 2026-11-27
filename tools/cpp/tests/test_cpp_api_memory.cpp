#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: MemoryReservation (Context::ReserveMemory), and the table function metrics callback. Both are
// only reachable from inside a callback, so the tests drive them through small table functions.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

// What the reserve functions observed, latched for the test to assert on.
struct Observed {
	std::vector<idx_t> sizes;
	std::optional<int64_t> extension_usage_while_held;
};
Observed observed;
// A second connection to the database the scan runs in, to look at its memory while the scan holds a reservation.
Connection *probe_connection = nullptr;

int64_t ExtensionUsage(Connection &conn) {
	auto result = conn.Execute("SELECT coalesce(sum(memory_usage_bytes), 0)::BIGINT FROM duckdb_memory() "
	                           "WHERE tag = 'EXTENSION'");
	auto chunk = result.FetchChunk();
	REQUIRE(chunk);
	auto view = chunk.GetVector(0).GetView();
	return view.Data<int64_t>()[view.SelAt(0)];
}

// cpp_reserve(n): reserves n bytes in its global init, grows the reservation by n in its exec, and produces no rows.
struct ReserveBind {
	int64_t bytes = 0;
	bool operator==(const ReserveBind &other) const {
		return bytes == other.bytes;
	}
};
struct ReserveGlobal {
	explicit ReserveGlobal(MemoryReservation reservation) : reservation(std::move(reservation)) {
	}
	MemoryReservation reservation;
	bool done = false;
};

void ReserveBind_(TableFunction::BindInput &input) {
	input.AddResultColumn("i", input.GetContext().GetFactory().ParseType("BIGINT"));
	input.SetBindData<ReserveBind>(ReserveBind {input.GetConstantArgument(0).Get<int64_t>()});
}

void ReserveInitGlobal(TableFunction::InitGlobalInput &input) {
	const auto bytes = input.GetBindData<ReserveBind>().bytes;
	input.SetGlobalState<ReserveGlobal>(input.GetContext().ReserveMemory(static_cast<idx_t>(bytes)));
}

void ReserveExec(TableFunction::ExecInput &input) {
	auto &global = input.GetGlobalState<ReserveGlobal>();
	const auto bytes = static_cast<idx_t>(input.GetBindData<ReserveBind>().bytes);
	if (!global.done) {
		global.done = true;
		observed.sizes.push_back(global.reservation.GetSize());
		if (probe_connection) {
			observed.extension_usage_while_held = ExtensionUsage(*probe_connection);
		}
		global.reservation.Resize(bytes * 2);
		observed.sizes.push_back(global.reservation.GetSize());
		global.reservation.Resize(bytes / 2);
		observed.sizes.push_back(global.reservation.GetSize());
	}
	input.GetOutputChunk().GetVector(0).SetSize(0);
}

void RegisterReserve(Connection &conn) {
	auto function = TableFunction::Create(conn.GetFactory());
	function.SetName("cpp_reserve");
	function.GetSignature().AddParameter("n", conn.GetFactory().ParseType("BIGINT"));
	function.SetBindCallback(ReserveBind_).SetInitGlobalCallback(ReserveInitGlobal).SetExecCallback(ReserveExec);
	conn.Register(function);
}

// cpp_metrics(): produces no rows and reports 123 bytes scanned. A bad metric name is refused.
bool bad_metric_refused = false;

void MetricsBind(TableFunction::BindInput &input) {
	input.AddResultColumn("i", input.GetContext().GetFactory().ParseType("BIGINT"));
}

void MetricsExec(TableFunction::ExecInput &input) {
	input.GetOutputChunk().GetVector(0).SetSize(0);
}

void MetricsCb(TableFunction::MetricsInput &input) {
	try {
		input.SetMetric("operator.no_such_metric", 1);
	} catch (InvalidInputException &) {
		bad_metric_refused = true;
	}
	input.SetMetric("operator.bytes_scanned", 123);
}

} // namespace

TEST_CASE("Stable C++API: reserve memory", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto probe = db.Connect();
	probe_connection = &probe;
	observed = Observed {};
	conn.Execute("SET threads = 1").Drain();
	RegisterReserve(conn);

	REQUIRE(ExtensionUsage(conn) == 0);
	conn.Execute("SELECT * FROM cpp_reserve(10000000)").Drain();
	REQUIRE(observed.sizes == std::vector<idx_t> {10000000, 20000000, 5000000});
	// the reservation counts as EXTENSION memory while it is held, and not any more once the query is gone
	REQUIRE(observed.extension_usage_while_held.has_value());
	REQUIRE(*observed.extension_usage_while_held >= 10000000);
	REQUIRE(ExtensionUsage(conn) == 0);

	// a reservation that does not fit fails the query with an out of memory error, and holds nothing back
	conn.Execute("SET memory_limit = '50MB'").Drain();
	REQUIRE_THROWS_MATCHES(conn.Execute("SELECT * FROM cpp_reserve(1000000000)").Drain(), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_RESOURCE_OUT_OF_MEMORY));
	REQUIRE(ExtensionUsage(conn) == 0);

	// growing past the limit fails and leaves the reservation as it was
	observed = Observed {};
	REQUIRE_THROWS_MATCHES(conn.Execute("SELECT * FROM cpp_reserve(40000000)").Drain(), Exception,
	                       HasErrorCode(DUCKDB_V2_ERROR_RESOURCE_OUT_OF_MEMORY));
	REQUIRE(observed.sizes == std::vector<idx_t> {40000000});
	REQUIRE(ExtensionUsage(conn) == 0);

	// a zero reservation is fine
	conn.Execute("SELECT * FROM cpp_reserve(0)").Drain();
	probe_connection = nullptr;
}

TEST_CASE("Stable C++API: table function metrics callback", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto function = TableFunction::Create(conn.GetFactory());
	function.SetName("cpp_metrics");
	function.SetBindCallback(MetricsBind).SetExecCallback(MetricsExec).SetMetricsCallback(MetricsCb);
	conn.Register(function);

	bad_metric_refused = false;
	conn.EnableProfiling();
	conn.Execute("SELECT * FROM cpp_metrics()").Drain();
	REQUIRE(bad_metric_refused);

	auto metrics = conn.GetQueryMetrics();
	auto root = metrics.GetRoot();
	auto total = root.GetMetric("query.total_bytes_scanned");
	REQUIRE(total.has_value());
	REQUIRE(total->Get<uint64_t>() == 123);
}
