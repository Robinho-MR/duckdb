#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: in-out table functions and virtual columns of table functions.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

// The first virtual column of the test functions, an identifier of the function's own.
constexpr idx_t VSQ_IDENTIFIER = TableFunction::VIRTUAL_COLUMN_START;

// Collect rows of BIGINTs, treating NULL as -999.
std::vector<std::vector<int64_t>> CollectRows(QueryResult result) {
	std::vector<std::vector<int64_t>> rows;
	while (auto chunk = result.FetchChunk()) {
		for (idx_t row = 0; row < chunk.GetRowCount(); row++) {
			std::vector<int64_t> values;
			for (idx_t col = 0; col < chunk.GetVectorCount(); col++) {
				auto view = chunk.GetVector(col).GetView();
				values.push_back(view.IsValid(row) ? view.Data<int64_t>()[view.SelAt(row)] : -999);
			}
			rows.push_back(std::move(values));
		}
	}
	return rows;
}

using Rows = std::vector<std::vector<int64_t>>;

// ---------------------------------------------------------------------------
// cpp_repeat(n): for every input row n, the rows (i, n) for i in [0, n). It declares the virtual columns vsq (= i * i),
// rowid (a running count) and the empty column, and supports projection pushdown.
// ---------------------------------------------------------------------------

constexpr idx_t BATCH_ROWS = 2;

struct RepeatLocal {
	idx_t row = 0;
	int64_t emitted = 0;
	int64_t total = 0;
	bool final_called = false;
};

struct RepeatObserved {
	std::string arg_type;
	bool constant_available = false;
	bool constant_refused = false;
	std::vector<idx_t> columns;
};

RepeatObserved observed;

void RepeatBind(TableFunction::BindInput &input) {
	auto &factory = input.GetContext().GetFactory();
	const auto bigint = factory.ParseType("BIGINT");
	input.AddResultColumn("i", bigint);
	input.AddResultColumn("n", bigint);
	input.AddVirtualColumn(VSQ_IDENTIFIER, "vsq", bigint);
	input.AddVirtualColumn(TableFunction::COLUMN_IDENTIFIER_ROW_ID, "rowid", bigint);
	input.AddVirtualColumn(TableFunction::COLUMN_IDENTIFIER_EMPTY, "", factory.ParseType("BOOLEAN"));

	observed.arg_type = std::string(input.GetArgType(0).GetName());
	observed.constant_available = input.TryGetConstantArgument(0).has_value();
	observed.constant_refused = false;
	try {
		input.GetConstantArgument(0);
	} catch (const InvalidInputException &) {
		observed.constant_refused = true;
	}
}

void RepeatInitLocal(TableFunction::InitLocalInput &input) {
	input.SetLocalState<RepeatLocal>();
}

void RepeatInitGlobal(TableFunction::InitGlobalInput &input) {
	input.SetMaxThreads(1);
	observed.columns.clear();
	for (idx_t i = 0; i < input.GetColumnCount(); i++) {
		observed.columns.push_back(input.GetColumnIndex(i));
	}
}

void RepeatInOut(TableFunction::InOutInput &input) {
	auto &local = input.GetLocalState<RepeatLocal>();
	auto in = input.GetInputChunk();
	auto out = input.GetOutputChunk();
	auto n_vector = in.GetVector(0).GetView();
	const auto in_rows = in.GetRowCount();

	const auto column_count = input.GetColumnCount();
	idx_t produced = 0;
	while (local.row < in_rows && produced < BATCH_ROWS) {
		const auto sel = n_vector.SelAt(local.row);
		const auto n = n_vector.IsValid(local.row) ? n_vector.Data<int64_t>()[sel] : 0;
		while (local.emitted < n && produced < BATCH_ROWS) {
			for (idx_t c = 0; c < column_count; c++) {
				auto vec = out.GetVector(c);
				const auto column = input.GetColumnIndex(c);
				if (column == TableFunction::COLUMN_IDENTIFIER_EMPTY) {
					continue;
				}
				int64_t value = 0;
				if (column == 0) {
					value = local.emitted;
				} else if (column == 1) {
					value = n;
				} else if (column == VSQ_IDENTIFIER) {
					value = local.emitted * local.emitted;
				} else if (column == TableFunction::COLUMN_IDENTIFIER_ROW_ID) {
					value = local.total;
				}
				vec.GetDataMutable<int64_t>()[produced] = value;
			}
			local.emitted++;
			local.total++;
			produced++;
		}
		if (local.emitted >= n) {
			local.row++;
			local.emitted = 0;
		}
	}
	for (idx_t c = 0; c < column_count; c++) {
		out.GetVector(c).SetSize(produced);
	}
	if (produced == 0) {
		// the input is exhausted: an empty output also ends the scan when the function runs as a source
		local.row = 0;
		input.SetResult(TableFunction::InOutResult::NEED_MORE_INPUT);
		return;
	}
	input.SetResult(TableFunction::InOutResult::HAVE_MORE_OUTPUT);
}

void RepeatFinal(TableFunction::InOutInput &input) {
	input.SetResult(TableFunction::InOutResult::FINISHED);
}

TableFunction MakeRepeat(Connection &conn, const std::string &name, bool with_final) {
	auto function = TableFunction::Create(conn.GetFactory());
	function.SetName(name);
	function.GetSignature().AddParameter("n", conn.GetFactory().ParseType("BIGINT"));
	function.SetBindCallback(RepeatBind)
	    .SetInitGlobalCallback(RepeatInitGlobal)
	    .SetInitLocalCallback(RepeatInitLocal)
	    .SetInOutCallback(RepeatInOut)
	    .SetProjectionPushdown(true);
	if (with_final) {
		function.SetInOutFinalCallback(RepeatFinal);
	}
	return function;
}

void NoopExec(TableFunction::ExecInput &) {
}

void NoopInOut(TableFunction::InOutInput &) {
}

void RegisterRepeat(Connection &conn) {
	auto function = MakeRepeat(conn, "cpp_repeat", false);
	conn.Register(function);
}

} // namespace

TEST_CASE("Stable C++API: in-out table function over a lateral input", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterRepeat(conn);

	// Several output batches per input row, and rows that produce nothing (0 and NULL).
	REQUIRE(CollectRows(conn.Execute("SELECT r.i FROM (VALUES (3::BIGINT), (0), (NULL), (2)) t(n), cpp_repeat(t.n) r ORDER BY 1")) ==
	        Rows {{0}, {0}, {1}, {1}, {2}});

	// Columns of the input next to the function's own.
	REQUIRE(CollectRows(conn.Execute("SELECT t.k, r.i FROM (VALUES (1::BIGINT, 2::BIGINT), (2, 1)) t(k, n), "
	                                 "cpp_repeat(t.n) r ORDER BY ALL")) == Rows {{1, 0}, {1, 1}, {2, 0}});

	// The bind callback sees the type of the argument, but no value for a column of the input.
	REQUIRE(observed.arg_type == "BIGINT");
	REQUIRE_FALSE(observed.constant_available);
	REQUIRE(observed.constant_refused);
}

TEST_CASE("Stable C++API: in-out table function as a source", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterRepeat(conn);

	// An all-constant call runs the function as a source: the callback runs until it produces nothing.
	REQUIRE(CollectRows(conn.Execute("SELECT * FROM cpp_repeat(5)")) ==
	        Rows {{0, 5}, {1, 5}, {2, 5}, {3, 5}, {4, 5}});
	// ... and the bind callback sees the value of the argument.
	REQUIRE(observed.constant_available);
	REQUIRE_FALSE(observed.constant_refused);
	REQUIRE(CollectRows(conn.Execute("SELECT count(*) FROM cpp_repeat(0)")) == Rows {{0}});
}

TEST_CASE("Stable C++API: in-out table function projection pushdown and virtual columns", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterRepeat(conn);

	// Only the columns the query uses are requested, and virtual columns are recognized by their identifier.
	REQUIRE(CollectRows(conn.Execute("SELECT n FROM cpp_repeat(2)")) == Rows {{2}, {2}});
	REQUIRE(observed.columns == std::vector<idx_t> {1});
	REQUIRE(CollectRows(conn.Execute("SELECT vsq, i FROM cpp_repeat(4)")) == Rows {{0, 0}, {1, 1}, {4, 2}, {9, 3}});
	REQUIRE(observed.columns == std::vector<idx_t> {0, VSQ_IDENTIFIER});
	REQUIRE(CollectRows(conn.Execute("SELECT rowid FROM cpp_repeat(3)")) == Rows {{0}, {1}, {2}});
	REQUIRE(observed.columns == std::vector<idx_t> {TableFunction::COLUMN_IDENTIFIER_ROW_ID});

	// Virtual columns are not part of SELECT *.
	REQUIRE(conn.Execute("SELECT * FROM cpp_repeat(1)").GetSchema().GetFieldCount() == 2);

	// A query needing the rows only gets the empty column.
	REQUIRE(CollectRows(conn.Execute("SELECT count(*) FROM cpp_repeat(5)")) == Rows {{5}});
	REQUIRE(observed.columns == std::vector<idx_t> {TableFunction::COLUMN_IDENTIFIER_EMPTY});

	// ... also with a lateral input.
	REQUIRE(CollectRows(conn.Execute(
	            "SELECT count(*) FROM (VALUES (3::BIGINT), (4)) t(n), cpp_repeat(t.n) r")) == Rows {{7}});
	REQUIRE(CollectRows(conn.Execute("SELECT sum(r.vsq) FROM (VALUES (3::BIGINT), (4)) t(n), cpp_repeat(t.n) r")) ==
	        Rows {{0 + 1 + 4 + 0 + 1 + 4 + 9}});

	// The virtual columns can be filtered and aggregated like any other column.
	REQUIRE(CollectRows(conn.Execute("SELECT i FROM cpp_repeat(6) WHERE vsq > 5")) == Rows {{3}, {4}, {5}});
}

TEST_CASE("Stable C++API: in-out final callback registers", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto function = MakeRepeat(conn, "cpp_repeat_final", true);
	conn.Register(function);
	// The engine does not run the final callback of a function that is fed by a subquery (it raises an internal
	// error), so only registration is checked here.
}

TEST_CASE("Stable C++API: in-out table function registration refusals", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	// An exec callback and an in-out callback are mutually exclusive.
	{
		auto function = MakeRepeat(conn, "cpp_both", false);
		function.SetExecCallback(NoopExec);
		REQUIRE_THROWS_MATCHES(conn.Register(function), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	// The final callback needs the in-out callback.
	{
		auto function = TableFunction::Create(conn.GetFactory());
		function.SetName("cpp_final_only").SetBindCallback(RepeatBind).SetExecCallback(NoopExec);
		function.SetInOutFinalCallback(NoopInOut);
		REQUIRE_THROWS_MATCHES(conn.Register(function), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	// The batch callbacks do not apply to an in-out function.
	{
		auto function = MakeRepeat(conn, "cpp_in_out_claim", false);
		function.SetClaimBatchCallback([](TableFunction::ClaimBatchInput &) {});
		REQUIRE_THROWS_MATCHES(conn.Register(function), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
}

namespace {

void NoPushdownBind(TableFunction::BindInput &input) {
	auto &factory = input.GetContext().GetFactory();
	input.AddResultColumn("i", factory.ParseType("BIGINT"));
	input.AddVirtualColumn(TableFunction::VIRTUAL_COLUMN_START, "v", factory.ParseType("BIGINT"));
}

void BadVirtualBind(TableFunction::BindInput &input) {
	auto &factory = input.GetContext().GetFactory();
	const auto bigint = factory.ParseType("BIGINT");
	input.AddResultColumn("i", bigint);
	input.AddVirtualColumn(TableFunction::VIRTUAL_COLUMN_START, "v", bigint);
	// each of these is refused, and leaves the declared virtual columns alone
	const std::vector<std::pair<idx_t, std::string>> refused {
	    {TableFunction::VIRTUAL_COLUMN_START, "w"},     // identifier taken
	    {TableFunction::VIRTUAL_COLUMN_START + 1, "v"}, // name taken
	    {TableFunction::VIRTUAL_COLUMN_START + 1, "i"}, // name of a result column
	    {TableFunction::VIRTUAL_COLUMN_START + 1, ""},  // no name
	    {5, "w"},                                       // not in the virtual range
	};
	for (auto &entry : refused) {
		REQUIRE_THROWS_MATCHES(input.AddVirtualColumn(entry.first, entry.second, bigint), Exception,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
}

void OneRowExec(TableFunction::ExecInput &input) {
	auto vec = input.GetOutputChunk().GetVector(0);
	vec.GetDataMutable<int64_t>()[0] = 7;
	vec.SetSize(0);
}

} // namespace

TEST_CASE("Stable C++API: virtual columns", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	// A virtual column needs projection pushdown, which is what lets the engine ask for it.
	{
		auto function = TableFunction::Create(conn.GetFactory());
		function.SetName("cpp_virtual_no_pushdown").SetBindCallback(NoPushdownBind).SetExecCallback(OneRowExec);
		conn.Register(function);
		REQUIRE_THROWS_MATCHES(conn.Execute("SELECT * FROM cpp_virtual_no_pushdown()").Drain(), Exception,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	// Refused declarations.
	{
		auto function = TableFunction::Create(conn.GetFactory());
		function.SetName("cpp_virtual_bad")
		    .SetBindCallback(BadVirtualBind)
		    .SetExecCallback(OneRowExec)
		    .SetProjectionPushdown(true);
		conn.Register(function);
		REQUIRE(conn.Execute("SELECT * FROM cpp_virtual_bad()").Drain() == 0);
	}
}
