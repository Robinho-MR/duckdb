#include "catch.hpp"
#include "duckdb.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/thread.hpp"

using namespace duckdb;

static string ExportSnapshot(Connection &con) {
	auto result = con.Query("SELECT pg_export_snapshot()");
	REQUIRE_NO_FAIL(*result);
	return result->Collection().GetValue(0, 0).ToString();
}

TEST_CASE("Test sharing a transaction between connections", "[api][transaction]") {
	DuckDB db(nullptr);
	Connection con1(db);
	Connection con2(db);
	Connection con3(db);
	REQUIRE_NO_FAIL(con1.Query("CREATE TABLE tbl(i INTEGER)"));
	REQUIRE_NO_FAIL(con1.Query("INSERT INTO tbl VALUES (1)"));

	SECTION("local changes and temporary tables are shared, changes flow both ways") {
		REQUIRE_NO_FAIL(con1.Query("BEGIN TRANSACTION"));
		REQUIRE_NO_FAIL(con1.Query("INSERT INTO tbl VALUES (2)"));
		REQUIRE_NO_FAIL(con1.Query("CREATE TEMPORARY TABLE tmp(i INTEGER)"));
		REQUIRE_NO_FAIL(con1.Query("INSERT INTO tmp VALUES (10)"));
		auto snapshot = ExportSnapshot(con1);

		// not visible outside of the transaction
		auto result = con3.Query("SELECT SUM(i) FROM tbl");
		REQUIRE(CHECK_COLUMN(result, 0, {1}));
		REQUIRE_FAIL(con3.Query("SELECT * FROM tmp"));

		REQUIRE_NO_FAIL(con2.Query("BEGIN TRANSACTION"));
		REQUIRE_NO_FAIL(con2.Query("SET TRANSACTION SNAPSHOT '" + snapshot + "'"));
		result = con2.Query("SELECT SUM(i) FROM tbl");
		REQUIRE(CHECK_COLUMN(result, 0, {3}));
		result = con2.Query("SELECT i FROM tmp");
		REQUIRE(CHECK_COLUMN(result, 0, {10}));

		// changes by the second connection are visible to the first
		REQUIRE_NO_FAIL(con2.Query("INSERT INTO tbl VALUES (3)"));
		REQUIRE_NO_FAIL(con2.Query("INSERT INTO tmp VALUES (20)"));
		REQUIRE_NO_FAIL(con2.Query("CREATE TEMPORARY TABLE tmp2 AS SELECT 5 AS i"));
		result = con1.Query("SELECT SUM(i) FROM tbl");
		REQUIRE(CHECK_COLUMN(result, 0, {6}));
		result = con1.Query("SELECT SUM(i) FROM tmp");
		REQUIRE(CHECK_COLUMN(result, 0, {30}));
		result = con1.Query("SELECT i FROM tmp2");
		REQUIRE(CHECK_COLUMN(result, 0, {5}));

		// committing from the second connection commits for both
		REQUIRE_NO_FAIL(con2.Query("COMMIT"));
		result = con3.Query("SELECT SUM(i) FROM tbl");
		REQUIRE(CHECK_COLUMN(result, 0, {6}));

		// the first connection learns that its transaction ended
		REQUIRE_FAIL(con1.Query("SELECT 42"));
		result = con1.Query("SELECT 42");
		REQUIRE(CHECK_COLUMN(result, 0, {42}));
		REQUIRE_FAIL(con1.Query("COMMIT"));
		// the snapshot is no longer valid
		REQUIRE_NO_FAIL(con3.Query("BEGIN TRANSACTION"));
		REQUIRE_FAIL(con3.Query("SET TRANSACTION SNAPSHOT '" + snapshot + "'"));
	}
	SECTION("rollback ends the shared transaction for everyone") {
		REQUIRE_NO_FAIL(con1.Query("BEGIN TRANSACTION"));
		REQUIRE_NO_FAIL(con1.Query("INSERT INTO tbl VALUES (2)"));
		auto snapshot = ExportSnapshot(con1);
		REQUIRE_NO_FAIL(con2.Query("BEGIN TRANSACTION"));
		REQUIRE_NO_FAIL(con2.Query("SET TRANSACTION SNAPSHOT '" + snapshot + "'"));
		REQUIRE_NO_FAIL(con2.Query("ROLLBACK"));
		REQUIRE_FAIL(con1.Query("SELECT 42"));
		auto result = con3.Query("SELECT SUM(i) FROM tbl");
		REQUIRE(CHECK_COLUMN(result, 0, {1}));
	}
	SECTION("exporting the same transaction twice returns the same snapshot") {
		REQUIRE_NO_FAIL(con1.Query("BEGIN TRANSACTION"));
		auto snapshot = ExportSnapshot(con1);
		REQUIRE(snapshot == ExportSnapshot(con1));
		REQUIRE_NO_FAIL(con1.Query("COMMIT"));
	}
	SECTION("the owner closing the connection rolls the transaction back") {
		auto owner = make_uniq<Connection>(db);
		REQUIRE_NO_FAIL(owner->Query("BEGIN TRANSACTION"));
		REQUIRE_NO_FAIL(owner->Query("INSERT INTO tbl VALUES (2)"));
		auto snapshot = ExportSnapshot(*owner);
		REQUIRE_NO_FAIL(con2.Query("BEGIN TRANSACTION"));
		REQUIRE_NO_FAIL(con2.Query("SET TRANSACTION SNAPSHOT '" + snapshot + "'"));
		owner.reset();
		REQUIRE_FAIL(con2.Query("SELECT 42"));
		auto result = con3.Query("SELECT SUM(i) FROM tbl");
		REQUIRE(CHECK_COLUMN(result, 0, {1}));
	}
	SECTION("a joined connection closing leaves the transaction running") {
		REQUIRE_NO_FAIL(con1.Query("BEGIN TRANSACTION"));
		REQUIRE_NO_FAIL(con1.Query("INSERT INTO tbl VALUES (2)"));
		auto snapshot = ExportSnapshot(con1);
		auto joined = make_uniq<Connection>(db);
		REQUIRE_NO_FAIL(joined->Query("BEGIN TRANSACTION"));
		REQUIRE_NO_FAIL(joined->Query("SET TRANSACTION SNAPSHOT '" + snapshot + "'"));
		REQUIRE_NO_FAIL(joined->Query("INSERT INTO tbl VALUES (3)"));
		joined.reset();
		REQUIRE_NO_FAIL(con1.Query("COMMIT"));
		auto result = con3.Query("SELECT SUM(i) FROM tbl");
		REQUIRE(CHECK_COLUMN(result, 0, {6}));
	}
	SECTION("concurrent use of a shared transaction is serialized") {
		REQUIRE_NO_FAIL(con1.Query("BEGIN TRANSACTION"));
		auto snapshot = ExportSnapshot(con1);
		REQUIRE_NO_FAIL(con2.Query("BEGIN TRANSACTION"));
		REQUIRE_NO_FAIL(con2.Query("SET TRANSACTION SNAPSHOT '" + snapshot + "'"));
		static constexpr idx_t ROWS_PER_THREAD = 100;
		auto insert = [&](Connection &con, int64_t base) {
			for (idx_t i = 0; i < ROWS_PER_THREAD; i++) {
				auto result = con.Query("INSERT INTO tbl VALUES (" + to_string(base + NumericCast<int64_t>(i)) + ")");
				REQUIRE_NO_FAIL(*result);
			}
		};
		thread t1([&]() { insert(con1, 1000); });
		thread t2([&]() { insert(con2, 2000); });
		t1.join();
		t2.join();
		auto result = con1.Query("SELECT COUNT(*) FROM tbl");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(1 + 2 * ROWS_PER_THREAD)}));
		REQUIRE_NO_FAIL(con1.Query("COMMIT"));
		result = con3.Query("SELECT COUNT(*) FROM tbl");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(1 + 2 * ROWS_PER_THREAD)}));
	}
	SECTION("errors") {
		// exporting requires a transaction
		REQUIRE_FAIL(con1.Query("SELECT pg_export_snapshot()"));
		// importing requires a transaction
		REQUIRE_FAIL(con2.Query("SET TRANSACTION SNAPSHOT 'unknown'"));
		REQUIRE_NO_FAIL(con2.Query("BEGIN TRANSACTION"));
		// unknown snapshot
		REQUIRE_FAIL(con2.Query("SET TRANSACTION SNAPSHOT 'unknown'"));
		REQUIRE_NO_FAIL(con2.Query("ROLLBACK"));

		// the transaction must not have been used yet
		REQUIRE_NO_FAIL(con1.Query("BEGIN TRANSACTION"));
		auto snapshot = ExportSnapshot(con1);
		REQUIRE_NO_FAIL(con2.Query("BEGIN TRANSACTION"));
		REQUIRE_NO_FAIL(con2.Query("SELECT * FROM tbl"));
		REQUIRE_FAIL(con2.Query("SET TRANSACTION SNAPSHOT '" + snapshot + "'"));
		REQUIRE_NO_FAIL(con2.Query("ROLLBACK"));
		REQUIRE_NO_FAIL(con1.Query("COMMIT"));
	}
}
