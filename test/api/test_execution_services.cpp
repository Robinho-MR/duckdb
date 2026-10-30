#include "catch.hpp"
#include "duckdb.hpp"
#include "duckdb/common/query_context.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/execution_services.hpp"
#include "duckdb/main/query_profiler.hpp"

using namespace duckdb;

TEST_CASE("Test ExecutionServices", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.Query("SET threads=2");

	SECTION("services from a ClientContext") {
		auto services = con.context->Services();
		REQUIRE(services.TryGetClientContext().get() == con.context.get());
		REQUIRE(&services.GetDatabase() == db.instance.get());
		REQUIRE(services.TryGetQueryProfiler());
		Value result;
		auto lookup = services.TryGetCurrentSetting(Identifier("threads"), result);
		bool found = lookup;
		REQUIRE(found);
		REQUIRE(result.GetValue<int64_t>() == 2);
	}
	SECTION("services from a DatabaseInstance") {
		auto services = db.instance->Services();
		REQUIRE(!services.TryGetClientContext());
		REQUIRE(!services.TryGetQueryProfiler());
		REQUIRE(&services.GetDatabase() == db.instance.get());
		Value result;
		auto lookup = services.TryGetCurrentSetting(Identifier("default_order"), result);
		bool found = lookup;
		REQUIRE(found);
		(void)services.GetLogger();
		(void)services.GetFileSystem();
		(void)services.GetMetricsManager();
	}
	SECTION("QueryContext holds services") {
		QueryContext invalid;
		REQUIRE(!invalid.GetServices());
		QueryContext valid(*con.context);
		REQUIRE(valid.GetServices());
		REQUIRE(valid.GetServices()->TryGetClientContext().get() == con.context.get());
		QueryContext from_ptr(optional_ptr<ClientContext>(con.context.get()));
		REQUIRE(from_ptr.GetServices());
	}
}
