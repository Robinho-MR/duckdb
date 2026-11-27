#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <atomic>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: operators (table in-out functions) and the virtual columns a bind callback declares.
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
};

struct RepeatGlobal {
	std::atomic<idx_t> produced {0};
};

struct RepeatObserved {
	std::string arg_type;
	idx_t input_columns = 0;
	std::string input_type;
	std::vector<idx_t> columns;
	std::vector<idx_t> execute_columns;
	int user_data = 0;
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
	observed.input_columns = input.GetInputColumnCount();
	observed.input_type = observed.input_columns > 0 ? std::string(input.GetInputColumnType(0).GetName()) : "";
}

void RepeatInitGlobal(Operator::InitGlobalInput &input) {
	observed.columns.clear();
	for (idx_t i = 0; i < input.GetColumnCount(); i++) {
		observed.columns.push_back(input.GetColumnIndex(i));
	}
	observed.user_data = input.GetUserData<int>();
	input.SetGlobalState<RepeatGlobal>();
	input.SetMaxThreads(1);
}

void RepeatInitLocal(Operator::InitLocalInput &input) {
	REQUIRE(input.GetColumnCount() == observed.columns.size());
	input.SetLocalState<RepeatLocal>();
}

void RepeatExecute(Operator::ExecuteInput &input) {
	auto &local = input.GetLocalState<RepeatLocal>();
	auto &global = input.GetGlobalState<RepeatGlobal>();
	auto in = input.GetInputChunk();
	auto out = input.GetOutputChunk();
	auto n_vector = in.GetVector(0).GetView();
	const auto in_rows = in.GetRowCount();

	const auto column_count = input.GetColumnCount();
	observed.execute_columns.clear();
	for (idx_t c = 0; c < column_count; c++) {
		observed.execute_columns.push_back(input.GetColumnIndex(c));
	}
	idx_t produced = 0;
	while (local.row < in_rows && produced < BATCH_ROWS) {
		const auto sel = n_vector.SelAt(local.row);
		const auto n = n_vector.IsValid(local.row) ? n_vector.Data<int64_t>()[sel] : 0;
		while (local.emitted < n && produced < BATCH_ROWS) {
			for (idx_t c = 0; c < column_count; c++) {
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
				out.GetVector(c).GetDataMutable<int64_t>()[produced] = value;
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
	global.produced += produced;
	for (idx_t c = 0; c < column_count; c++) {
		out.GetVector(c).SetSize(produced);
	}
	if (local.row >= in_rows) {
		local.row = 0;
		input.SetResult(Operator::Result::NEED_MORE_INPUT);
		return;
	}
	input.SetResult(Operator::Result::HAVE_MORE_OUTPUT);
}

void RepeatProgress(Operator::ProgressInput &input) {
	auto global = input.TryGetGlobalState<RepeatGlobal>();
	input.SetProgress(global && global->produced > 0 ? 0.5 : 0.0);
}

void RegisterRepeat(Connection &conn) {
	auto op = Operator::Create(conn.GetFactory());
	op.SetUserData<int>(42)
	    .SetInitGlobalCallback(RepeatInitGlobal)
	    .SetInitLocalCallback(RepeatInitLocal)
	    .SetExecuteCallback(RepeatExecute)
	    .SetProgressCallback(RepeatProgress);
	// the same operator serves several functions
	for (auto name : {"cpp_repeat", "cpp_repeat2"}) {
		auto function = TableFunction::Create(conn.GetFactory());
		function.SetName(name);
		function.GetSignature().AddParameter("n", conn.GetFactory().ParseType("BIGINT"));
		function.SetBindCallback(RepeatBind)
		    .SetProjectionPushdown(true)
		    .SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY)
		    .SetOperator(op, TableFunction::OperatorInput::ARGUMENTS);
		conn.Register(function);
	}
}

// cpp_sum_table((SELECT ...)): sums the first BIGINT column of its table input, emitting the total from finalize.
struct SumLocal {
	int64_t total = 0;
	bool emitted = false;
};

void SumBind(TableFunction::BindInput &input) {
	REQUIRE(input.GetInputColumnCount() >= 1);
	input.AddResultColumn("total", input.GetContext().GetFactory().ParseType("BIGINT"));
}

void SumInitLocal(Operator::InitLocalInput &input) {
	input.SetLocalState<SumLocal>();
}

void SumExecute(Operator::ExecuteInput &input) {
	auto &local = input.GetLocalState<SumLocal>();
	auto in = input.GetInputChunk();
	auto vec = in.GetVector(0);
	vec.Flatten();
	auto view = vec.GetView();
	for (idx_t i = 0; i < in.GetRowCount(); i++) {
		if (view.IsValid(i)) {
			local.total += view.Data<int64_t>()[view.SelAt(i)];
		}
	}
	input.GetOutputChunk().GetVector(0).SetSize(0);
}

void SumFinalize(Operator::FinalizeInput &input) {
	auto &local = input.GetLocalState<SumLocal>();
	auto out = input.GetOutputChunk().GetVector(0);
	if (local.emitted) {
		out.SetSize(0);
		return;
	}
	REQUIRE(input.GetColumnCount() == 1);
	out.GetDataMutable<int64_t>()[0] = local.total;
	out.SetSize(1);
	local.emitted = true;
}

void NoopExec(TableFunction::ExecInput &) {
}

} // namespace

TEST_CASE("Stable C++API: operator over a lateral input", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterRepeat(conn);

	// Several output batches per input row, and rows that produce nothing (0 and NULL).
	REQUIRE(CollectRows(conn.Execute(
	            "SELECT r.i FROM (VALUES (3::BIGINT), (0), (NULL), (2)) t(n), cpp_repeat(t.n) r ORDER BY 1")) ==
	        Rows {{0}, {0}, {1}, {1}, {2}});

	// Columns of the input next to the function's own.
	REQUIRE(CollectRows(conn.Execute("SELECT t.k, r.i FROM (VALUES (1::BIGINT, 2::BIGINT), (2, 1)) t(k, n), "
	                                 "cpp_repeat(t.n) r ORDER BY ALL")) == Rows {{1, 0}, {1, 1}, {2, 0}});

	// The bind callback sees the type of the argument, and the input columns.
	REQUIRE(observed.arg_type == "BIGINT");
	REQUIRE(observed.input_columns == 1);
	REQUIRE(observed.input_type == "BIGINT");
	REQUIRE(observed.user_data == 42);
}

TEST_CASE("Stable C++API: operator on constant arguments", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterRepeat(conn);

	REQUIRE(CollectRows(conn.Execute("SELECT * FROM cpp_repeat(5)")) == Rows {{0, 5}, {1, 5}, {2, 5}, {3, 5}, {4, 5}});
	REQUIRE(CollectRows(conn.Execute("SELECT * FROM cpp_repeat2(1)")) == Rows {{0, 1}});
	REQUIRE(CollectRows(conn.Execute("SELECT count(*) FROM cpp_repeat(0)")) == Rows {{0}});
}

TEST_CASE("Stable C++API: operator projection pushdown and virtual columns", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterRepeat(conn);

	// Only the columns the query uses are requested, and virtual columns are recognized by their identifier.
	REQUIRE(CollectRows(conn.Execute("SELECT n FROM cpp_repeat(2)")) == Rows {{2}, {2}});
	REQUIRE(observed.columns == std::vector<idx_t> {1});
	REQUIRE(observed.execute_columns == std::vector<idx_t> {1});
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
	REQUIRE(CollectRows(conn.Execute("SELECT count(*) FROM (VALUES (3::BIGINT), (4)) t(n), cpp_repeat(t.n) r")) ==
	        Rows {{7}});
	REQUIRE(CollectRows(conn.Execute("SELECT sum(r.vsq) FROM (VALUES (3::BIGINT), (4)) t(n), cpp_repeat(t.n) r")) ==
	        Rows {{0 + 1 + 4 + 0 + 1 + 4 + 9}});

	// The virtual columns can be filtered and aggregated like any other column.
	REQUIRE(CollectRows(conn.Execute("SELECT i FROM cpp_repeat(6) WHERE vsq > 5")) == Rows {{3}, {4}, {5}});
}

TEST_CASE("Stable C++API: operator over a table input with finalize", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto op = Operator::Create(conn.GetFactory());
	op.SetInitLocalCallback(SumInitLocal).SetExecuteCallback(SumExecute).SetFinalizeCallback(SumFinalize);
	auto function = TableFunction::Create(conn.GetFactory());
	function.SetName("cpp_sum_table").SetBindCallback(SumBind).SetOperator(op, TableFunction::OperatorInput::TABLE);
	conn.Register(function);

	// One total per thread: summed up, they are the total of the input.
	REQUIRE(CollectRows(conn.Execute("SELECT sum(total) FROM cpp_sum_table((SELECT range AS x FROM range(10000)))")) ==
	        Rows {{49995000}});
}

TEST_CASE("Stable C++API: operator registration refusals", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto op = Operator::Create(conn.GetFactory());
	op.SetExecuteCallback(RepeatExecute);

	// An operator replaces the scan: an exec callback is refused.
	auto function = TableFunction::Create(conn.GetFactory());
	function.SetName("cpp_both").SetBindCallback(RepeatBind).SetExecCallback(NoopExec);
	function.GetSignature().AddParameter("n", conn.GetFactory().ParseType("BIGINT"));
	function.SetOperator(op, TableFunction::OperatorInput::ARGUMENTS);
	REQUIRE_THROWS_MATCHES(conn.Register(function), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));

	// An operator without an execute callback is refused.
	auto empty = Operator::Create(conn.GetFactory());
	auto other = TableFunction::Create(conn.GetFactory());
	other.SetName("cpp_no_execute").SetBindCallback(RepeatBind);
	other.GetSignature().AddParameter("n", conn.GetFactory().ParseType("BIGINT"));
	other.SetOperator(empty, TableFunction::OperatorInput::ARGUMENTS);
	REQUIRE_THROWS_MATCHES(conn.Register(other), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
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

struct OneRowGlobal {
	bool done = false;
};

void OneRowInitGlobal(TableFunction::InitGlobalInput &input) {
	input.SetGlobalState<OneRowGlobal>();
}

// Writes 7 into every requested column once
void OneRowExec(TableFunction::ExecInput &input) {
	auto &global = input.GetGlobalState<OneRowGlobal>();
	auto chunk = input.GetOutputChunk();
	if (global.done) {
		chunk.GetVector(0).SetSize(0);
		return;
	}
	global.done = true;
	for (idx_t c = 0; c < input.GetColumnCount(); c++) {
		chunk.GetVector(c).GetDataMutable<int64_t>()[0] = 7;
	}
	chunk.GetVector(0).SetSize(1);
}

} // namespace

TEST_CASE("Stable C++API: virtual columns declared at bind", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	// A virtual column needs projection pushdown, which is what lets the engine ask for it.
	{
		auto function = TableFunction::Create(conn.GetFactory());
		function.SetName("cpp_virtual_no_pushdown")
		    .SetBindCallback(NoPushdownBind)
		    .SetInitGlobalCallback(OneRowInitGlobal)
		    .SetExecCallback(OneRowExec);
		conn.Register(function);
		REQUIRE_THROWS_MATCHES(conn.Execute("SELECT * FROM cpp_virtual_no_pushdown()").Drain(), Exception,
		                       HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	// Refused declarations, and a scan producing a virtual column.
	{
		auto function = TableFunction::Create(conn.GetFactory());
		function.SetName("cpp_virtual_scan")
		    .SetBindCallback(BadVirtualBind)
		    .SetInitGlobalCallback(OneRowInitGlobal)
		    .SetExecCallback(OneRowExec)
		    .SetProjectionPushdown(true);
		conn.Register(function);
		REQUIRE(CollectRows(conn.Execute("SELECT * FROM cpp_virtual_scan()")) == Rows {{7}});
		REQUIRE(CollectRows(conn.Execute("SELECT v, i FROM cpp_virtual_scan()")) == Rows {{7, 7}});
	}
}
