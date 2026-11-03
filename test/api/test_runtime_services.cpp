#include "catch.hpp"
#include "duckdb.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/query_context.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/runtime_services.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/parallel/task_executor.hpp"

using namespace duckdb;

TEST_CASE("Test RuntimeServices", "[api]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.Query("SET threads=2");

	SECTION("services owned by a ClientContext") {
		auto &services = con.context->Services();
		REQUIRE(services.IsClient());
		REQUIRE(services.TryGetClientContext().get() == con.context.get());
		REQUIRE(&services.GetDatabase() == db.instance.get());
		REQUIRE(services.TryGetQueryProfiler());
		REQUIRE(services.TryGetFileOpener());
		REQUIRE(&services.GetClientConfig() == &ClientConfig::GetConfig(*con.context));
		REQUIRE(&services.GetLogger() == &con.context->GetLogger());
		(void)services.GetTaskScheduler();
		Value result;
		auto lookup = services.TryGetCurrentSetting(Identifier("threads"), result);
		bool found = lookup;
		REQUIRE(found);
		REQUIRE(result.GetValue<int64_t>() == 2);
	}
	SECTION("services created from a DatabaseInstance") {
		auto services = db.instance->Services();
		REQUIRE(!services->IsClient());
		REQUIRE(!services->TryGetClientContext());
		REQUIRE(!services->TryGetQueryProfiler());
		REQUIRE(!services->TryGetClientConfig());
		REQUIRE(&services->GetDatabase() == db.instance.get());
		Value result;
		auto lookup = services->TryGetCurrentSetting(Identifier("default_order"), result);
		bool found = lookup;
		REQUIRE(found);
		(void)services->GetLogger();
		(void)services->GetFileSystem();
		(void)services->GetMetricsManager();
		(void)services->GetTaskScheduler();
	}
	SECTION("TaskExecutor from services") {
		auto services = db.instance->Services();
		TaskExecutor executor(*services);
		executor.WorkOnTasks();
	}
	SECTION("QueryContext holds services") {
		QueryContext invalid;
		REQUIRE(!invalid.Valid());
		REQUIRE(!invalid.GetServices());
		QueryContext valid(*con.context);
		REQUIRE(valid.Valid());
		REQUIRE(valid.GetServices().get() == con.context->services.get());
		QueryContext from_ptr(optional_ptr<ClientContext>(con.context.get()));
		REQUIRE(from_ptr.GetServices().get() == con.context->services.get());
	}
}

namespace {
class SharedTestState : public ClientContextState {};
} // namespace

TEST_CASE("Test RuntimeServices shared with child connections", "[api]") {
	DuckDB db(nullptr);
	Connection parent(db);

	SECTION("a child shares the services of its parent") {
		auto child = Connection::CreateChild(parent);
		auto &parent_context = *parent.context;
		auto &child_context = *child.context;
		REQUIRE(&child_context.Services() == &parent_context.Services());
		REQUIRE(child_context.UsesInheritedServices());
		REQUIRE(!parent_context.UsesInheritedServices());
		REQUIRE(child_context.Services().TryGetClientContext().get() == parent.context.get());
		REQUIRE(&ClientConfig::GetConfig(child_context) == &ClientConfig::GetConfig(parent_context));
		REQUIRE(&child_context.GetLogger() == &parent_context.GetLogger());
		REQUIRE(&child_context.Services().GetRegisteredState() == &parent_context.Services().GetRegisteredState());

		// the services of a grandchild are the services of the original context
		auto grandchild = Connection::CreateChild(child);
		REQUIRE(&grandchild.context->Services() == &parent_context.Services());
		REQUIRE(grandchild.context->Services().TryGetClientContext().get() == parent.context.get());
	}
	SECTION("a plain connection does not share services") {
		Connection other(db);
		REQUIRE(&other.context->Services() != &parent.context->Services());
		REQUIRE(!other.context->UsesInheritedServices());
	}
	SECTION("client settings are shared with the child but not with other connections") {
		auto child = Connection::CreateChild(parent);
		Connection other(db);
		REQUIRE_NO_FAIL(parent.Query("SET max_execution_time=12345"));
		auto result = child.Query("SELECT current_setting('max_execution_time')::VARCHAR");
		REQUIRE(CHECK_COLUMN(result, 0, {"12345"}));
		result = other.Query("SELECT current_setting('max_execution_time')::VARCHAR");
		REQUIRE(CHECK_COLUMN(result, 0, {"0"}));
		// a setting changed in the child is visible to the parent
		REQUIRE_NO_FAIL(child.Query("RESET max_execution_time"));
		result = parent.Query("SELECT current_setting('max_execution_time')::VARCHAR");
		REQUIRE(CHECK_COLUMN(result, 0, {"0"}));
	}
	SECTION("registered states are shared") {
		auto child = Connection::CreateChild(parent);
		Connection other(db);
		parent.context->Services().GetRegisteredState().GetOrCreate<SharedTestState>("shared_test_state");
		REQUIRE(child.context->Services().GetRegisteredState().Get<SharedTestState>("shared_test_state"));
		REQUIRE(!other.context->Services().GetRegisteredState().Get<SharedTestState>("shared_test_state"));
	}
	SECTION("transactions and temporary objects are not shared") {
		auto child = Connection::CreateChild(parent);
		REQUIRE_NO_FAIL(parent.Query("BEGIN"));
		REQUIRE_NO_FAIL(parent.Query("CREATE TEMPORARY TABLE shared_test_temp(i INTEGER)"));
		REQUIRE(parent.context->HasActiveTransaction());
		REQUIRE(!child.context->HasActiveTransaction());
		auto result = child.Query("SELECT * FROM shared_test_temp");
		REQUIRE(result->HasError());
		REQUIRE_NO_FAIL(parent.Query("ROLLBACK"));
		REQUIRE_NO_FAIL(child.Query("BEGIN"));
		REQUIRE_NO_FAIL(child.Query("CREATE TABLE shared_test_table(i INTEGER)"));
		REQUIRE_NO_FAIL(child.Query("ROLLBACK"));
		result = parent.Query("SELECT * FROM shared_test_table");
		REQUIRE(result->HasError());
	}
	SECTION("each context has its own query profiler") {
		auto child = Connection::CreateChild(parent);
		parent.EnableProfiling();
		ClientConfig::GetConfig(*parent.context).profiler_save_location =
		    TestCreatePath("test_runtime_services_profile.txt");
		REQUIRE(&QueryProfiler::Get(*parent.context) == &parent.context->Services().GetQueryProfiler());
		REQUIRE(&QueryProfiler::Get(*child.context) != &QueryProfiler::Get(*parent.context));

		string parent_query = "SELECT 42 AS shared_services_parent_marker";
		string child_query = "SELECT 7 AS shared_services_child_marker";
		REQUIRE_NO_FAIL(parent.Query(parent_query));
		REQUIRE_NO_FAIL(child.Query(child_query));
		auto parent_output = parent.GetProfilingInformation(ProfilerPrintFormat::JSON());
		auto child_output = child.GetProfilingInformation(ProfilerPrintFormat::JSON());
		REQUIRE(parent_output.find(parent_query) != string::npos);
		REQUIRE(parent_output.find(child_query) == string::npos);
		REQUIRE(child_output.find(child_query) != string::npos);
		REQUIRE(child_output.find(parent_query) == string::npos);
	}
	SECTION("a child keeps the context owning the services alive") {
		Connection child = [&]() {
			Connection temporary_parent(db);
			REQUIRE_NO_FAIL(temporary_parent.Query("SET max_execution_time=12345"));
			return Connection::CreateChild(temporary_parent);
		}();
		auto result = child.Query("SELECT current_setting('max_execution_time')::VARCHAR");
		REQUIRE(CHECK_COLUMN(result, 0, {"12345"}));
		REQUIRE(child.context->Services().TryGetClientContext());
		REQUIRE(child.context->Services().TryGetClientContext().get() != child.context.get());
	}
}
