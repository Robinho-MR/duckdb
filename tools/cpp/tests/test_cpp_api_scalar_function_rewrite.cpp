#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: a scalar function that replaces its call at bind time
// (BindInput::ReplaceCall) and bind data that survives plan serialization
// (ScalarFunction::SetSerializeCallbacks).
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

std::vector<int32_t> CollectInts(QueryResult result) {
	std::vector<int32_t> out;
	while (auto chunk = result.FetchChunk()) {
		auto view = chunk.GetVector(0).GetView();
		for (idx_t i = 0; i < chunk.GetRowCount(); i++) {
			REQUIRE(view.IsValid(i));
			out.push_back(view.Data<int32_t>()[view.SelAt(i)]);
		}
	}
	return out;
}

// EXPLAIN puts the rendered plan in column 1.
bool ExplainContains(Connection &conn, const std::string &sql, const std::string &needle) {
	auto result = conn.Execute("EXPLAIN " + sql);
	while (auto chunk = result.FetchChunk()) {
		auto view = chunk.GetVector(1).GetView();
		for (idx_t i = 0; i < chunk.GetRowCount(); i++) {
			if (std::string(view.Data<varchar_t>()[view.SelAt(i)].view()).find(needle) != std::string::npos) {
				return true;
			}
		}
	}
	return false;
}

//----------------------------------------------------------------------------------------------------------------------
// Replacing the call
//----------------------------------------------------------------------------------------------------------------------

// out[i] = a[i] + 1000
void SingleExec(ScalarFunction::ExecInput &input) {
	const auto a = input.GetArg(0).GetView();
	auto result = input.GetResult();
	auto *out = result.GetDataMutable<int32_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		out[i] = a.Data<int32_t>()[a.SelAt(i)] + 1000;
	}
}

// out[i] = b[i] * 2: the call a non-constant selector keeps
void PickExec(ScalarFunction::ExecInput &input) {
	const auto b = input.GetArg(1).GetView();
	auto result = input.GetResult();
	auto *out = result.GetDataMutable<int32_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		out[i] = b.Data<int32_t>()[b.SelAt(i)] * 2;
	}
}

void PickBind(ScalarFunction::BindInput &input) {
	if (input.TryGetConstantArgument(0)) {
		input.ReplaceCall("cpp_single", {1});
	}
}

// The replacement returns VARCHAR, the call INTEGER
void TextExec(ScalarFunction::ExecInput &input) {
	auto result = input.GetResult();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		result.AssignString(i, "text");
	}
}

void TypeMismatchBind(ScalarFunction::BindInput &input) {
	input.ReplaceCall("cpp_text", {0});
}

enum class MisuseMode {
	BIND_DATA_THEN_REPLACE,
	REPLACE_THEN_BIND_DATA,
	REPLACE_TWICE,
	UNKNOWN,
	OUT_OF_BOUNDS,
	SELF,
	STABILITY_THEN_REPLACE,
	REPLACE_THEN_STABILITY
};
std::atomic<MisuseMode> misuse_mode {MisuseMode::UNKNOWN};

void MisuseBind(ScalarFunction::BindInput &input) {
	switch (misuse_mode.load()) {
	case MisuseMode::BIND_DATA_THEN_REPLACE:
		input.SetBindData<int>(1);
		input.ReplaceCall("cpp_single", {0});
		break;
	case MisuseMode::REPLACE_THEN_BIND_DATA:
		input.ReplaceCall("cpp_single", {0});
		input.SetBindData<int>(1);
		break;
	case MisuseMode::REPLACE_TWICE:
		input.ReplaceCall("cpp_single", {0});
		input.ReplaceCall("cpp_single", {0});
		break;
	case MisuseMode::UNKNOWN:
		input.ReplaceCall("cpp_does_not_exist", {0});
		break;
	case MisuseMode::OUT_OF_BOUNDS:
		input.ReplaceCall("cpp_single", {5});
		break;
	case MisuseMode::SELF:
		input.ReplaceCall("cpp_misuse", {0});
		break;
	case MisuseMode::STABILITY_THEN_REPLACE:
		input.SetStability(FunctionStability::CONSISTENT);
		input.ReplaceCall("cpp_single", {0});
		break;
	case MisuseMode::REPLACE_THEN_STABILITY:
		input.ReplaceCall("cpp_single", {0});
		input.SetStability(FunctionStability::CONSISTENT);
		break;
	}
}

void RegisterRewriteFunctions(Connection &conn) {
	auto &factory = conn.GetFactory();
	const auto integer = factory.ParseType("INTEGER");

	auto single = ScalarFunction::Create(factory);
	single.SetName("cpp_single").SetExecCallback(SingleExec);
	single.GetSignature().AddParameter("a", integer).SetReturnType(integer);
	conn.Register(single);

	auto pick = ScalarFunction::Create(factory);
	pick.SetName("cpp_pick").SetBindCallback(PickBind).SetExecCallback(PickExec);
	pick.GetSignature().AddParameter("selector", integer).AddParameter("b", integer).SetReturnType(integer);
	conn.Register(pick);

	auto text = ScalarFunction::Create(factory);
	text.SetName("cpp_text").SetExecCallback(TextExec);
	text.GetSignature().AddParameter("a", integer).SetReturnType(factory.ParseType("VARCHAR"));
	conn.Register(text);

	auto mismatch = ScalarFunction::Create(factory);
	mismatch.SetName("cpp_mismatch").SetBindCallback(TypeMismatchBind).SetExecCallback(SingleExec);
	mismatch.GetSignature().AddParameter("a", integer).SetReturnType(integer);
	conn.Register(mismatch);

	auto misuse = ScalarFunction::Create(factory);
	misuse.SetName("cpp_misuse").SetBindCallback(MisuseBind).SetExecCallback(SingleExec);
	misuse.GetSignature().AddParameter("a", integer).SetReturnType(integer);
	conn.Register(misuse);
}

//----------------------------------------------------------------------------------------------------------------------
// Serialization
//----------------------------------------------------------------------------------------------------------------------

struct SerUser {
	int32_t magic;
};

struct SerData {
	int32_t add;
	std::string label;
	bool operator==(const SerData &other) const {
		return add == other.add && label == other.label;
	}
};

std::atomic<int> ser_bind_runs {0};
std::atomic<int> ser_serialize_runs {0};
std::atomic<int> ser_deserialize_runs {0};

void SerBind(ScalarFunction::BindInput &input) {
	ser_bind_runs++;
	input.SetBindData<SerData>(SerData {input.GetConstantArgument(1).Get<int32_t>(), "label"});
}

// out[i] = a[i] + bind data
void SerExec(ScalarFunction::ExecInput &input) {
	const auto add = input.GetBindData<SerData>().add;
	REQUIRE(input.GetBindData<SerData>().label == "label");
	const auto a = input.GetArg(0).GetView();
	auto result = input.GetResult();
	auto *out = result.GetDataMutable<int32_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		out[i] = a.Data<int32_t>()[a.SelAt(i)] + add;
	}
}

// The bytes: the user data magic, the addend, then the label, in three writes
void SerSerialize(ScalarFunction::SerializeInput &input) {
	ser_serialize_runs++;
	const auto &data = input.GetBindData<SerData>();
	const auto magic = input.GetUserData<SerUser>().magic;
	input.Write(&magic, sizeof(magic));
	input.Write(&data.add, sizeof(data.add));
	input.Write(data.label);
}

void SerDeserialize(ScalarFunction::DeserializeInput &input) {
	ser_deserialize_runs++;
	const auto bytes = input.GetData();
	REQUIRE(bytes.size() >= 2 * sizeof(int32_t));
	int32_t magic = 0;
	int32_t add = 0;
	std::memcpy(&magic, bytes.data(), sizeof(magic));
	std::memcpy(&add, bytes.data() + sizeof(magic), sizeof(add));
	REQUIRE(magic == input.GetUserData<SerUser>().magic);
	input.SetBindData<SerData>(SerData {add, std::string(bytes.substr(2 * sizeof(int32_t)))});
}

void SerFailingSerialize(ScalarFunction::SerializeInput &) {
	throw InvalidInputException("cannot serialize on purpose");
}

void SerNoopDeserialize(ScalarFunction::DeserializeInput &) {
}

void RegisterSerFunction(Connection &conn, const char *name, ScalarFunction::SerializeCallback serialize) {
	auto &factory = conn.GetFactory();
	const auto integer = factory.ParseType("INTEGER");
	auto function = ScalarFunction::Create(factory);
	function.SetName(name)
	    .SetUserData<SerUser>(SerUser {4711})
	    .SetBindCallback(SerBind)
	    .SetExecCallback(SerExec)
	    .SetSerializeCallbacks(serialize, serialize == SerSerialize ? SerDeserialize : SerNoopDeserialize);
	function.GetSignature().AddParameter("a", integer).AddParameter("k", integer).SetReturnType(integer);
	conn.Register(function);
}

} // namespace

TEST_CASE("Stable C++API: scalar function replaces its call at bind time", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterRewriteFunctions(conn);

	// A constant selector replaces the call by cpp_single(b)
	REQUIRE(CollectInts(conn.Execute("SELECT cpp_pick(1, i::INTEGER) FROM range(3) t(i)")) ==
	        std::vector<int32_t> {1000, 1001, 1002});
	REQUIRE(CollectInts(conn.Execute("SELECT cpp_pick(1, 5)")) == std::vector<int32_t> {1005});
	// The arguments reach the replacement cast to its parameter types
	REQUIRE(CollectInts(conn.Execute("SELECT cpp_pick(1, 7::TINYINT)")) == std::vector<int32_t> {1007});

	// A selector that is not constant keeps the original call
	REQUIRE(CollectInts(conn.Execute("SELECT cpp_pick(s, i) FROM (VALUES (1, 21)) t(s, i)")) ==
	        std::vector<int32_t> {42});

	// The plan shows the replacement
	REQUIRE(ExplainContains(conn, "SELECT cpp_pick(1, i::INTEGER) FROM range(3) t(i)", "cpp_single"));
	REQUIRE_FALSE(ExplainContains(conn, "SELECT cpp_pick(1, i::INTEGER) FROM range(3) t(i)", "cpp_pick"));
	REQUIRE(ExplainContains(conn, "SELECT cpp_pick(s, i) FROM (VALUES (1, 21)) t(s, i)", "cpp_pick"));
	REQUIRE_FALSE(ExplainContains(conn, "SELECT cpp_pick(s, i) FROM (VALUES (1, 21)) t(s, i)", "cpp_single"));
}

TEST_CASE("Stable C++API: the replaced call takes the return type of the replacement", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterRewriteFunctions(conn);

	// cpp_mismatch declares INTEGER, its replacement returns VARCHAR
	auto result = conn.Execute("SELECT typeof(cpp_mismatch(i::INTEGER)), cpp_mismatch(i::INTEGER) FROM range(1) t(i)");
	auto chunk = result.FetchChunk();
	REQUIRE(chunk);
	auto type_view = chunk.GetVector(0).GetView();
	auto value_view = chunk.GetVector(1).GetView();
	REQUIRE(std::string(type_view.Data<varchar_t>()[type_view.SelAt(0)].view()) == "VARCHAR");
	REQUIRE(std::string(value_view.Data<varchar_t>()[value_view.SelAt(0)].view()) == "text");
}

//----------------------------------------------------------------------------------------------------------------------
// Running without a client context
//----------------------------------------------------------------------------------------------------------------------

std::atomic<int> no_context_init_runs {0};
std::atomic<int> no_context_exec_runs {0};

void NoContextInit(ScalarFunction::InitInput &input) {
	if (!input.HasContext()) {
		no_context_init_runs++;
	}
}

// out[i] = a[i] + 5, without touching the context
void NoContextExec(ScalarFunction::ExecInput &input) {
	if (!input.HasContext()) {
		no_context_exec_runs++;
	}
	const auto a = input.GetArg(0).GetView();
	auto result = input.GetResult();
	auto *out = result.GetDataMutable<int32_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		out[i] = a.Data<int32_t>()[a.SelAt(i)] + 5;
	}
}

void ContextExec(ScalarFunction::ExecInput &input) {
	input.GetContext();
	SingleExec(input);
}

// INTEGER -> VARCHAR through the custom type, without touching the context
void NoContextCastExec(ScalarFunction::ExecInput &input) {
	if (!input.HasContext()) {
		no_context_exec_runs++;
	}
	auto in = input.GetArg(0);
	auto out = input.GetResult();
	auto view = in.GetView();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		out.AssignString(i, std::to_string(view.Data<int32_t>()[view.SelAt(i)]) + " units");
	}
}

void ContextCastExec(ScalarFunction::ExecInput &input) {
	input.GetContext();
	NoContextCastExec(input);
}

void RegisterNoContextFunction(Connection &conn, const char *name, ScalarFunction::ExecCallback exec) {
	auto &factory = conn.GetFactory();
	const auto integer = factory.ParseType("INTEGER");
	auto function = ScalarFunction::Create(factory);
	function.SetName(name).SetInitCallback(NoContextInit).SetExecCallback(exec);
	function.GetSignature().AddParameter("a", integer).SetReturnType(integer);
	conn.Register(function);
}

void RegisterUnitsCast(Connection &conn, ScalarFunction::ExecCallback exec) {
	auto &factory = conn.GetFactory();
	auto type = CustomType::Create(factory);
	type.SetName("UNITS").SetBaseType(factory.ParseType("INTEGER"));
	conn.Register(type);

	auto cast = ScalarFunction::Create(factory);
	cast.SetName("units_to_text").SetExecCallback(exec);
	cast.GetSignature()
	    .AddParameter("value", factory.ParseType("INTEGER").WithAlias(factory, "UNITS"))
	    .SetReturnType(factory.ParseType("VARCHAR"));
	conn.RegisterCast(cast);
}

TEST_CASE("Stable C++API: scalar function runs without a client context while an index is built", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterNoContextFunction(conn, "cpp_no_context", NoContextExec);
	RegisterNoContextFunction(conn, "cpp_needs_context", ContextExec);
	conn.Execute("CREATE TABLE t (x INTEGER)").Drain();
	conn.Execute("INSERT INTO t VALUES (1), (2), (3)").Drain();

	// With a context, the callbacks see it
	no_context_init_runs = 0;
	no_context_exec_runs = 0;
	REQUIRE(CollectInts(conn.Execute("SELECT cpp_no_context(x) FROM t ORDER BY x")) == std::vector<int32_t> {6, 7, 8});
	REQUIRE(no_context_exec_runs == 0);

	// The index maintains its key without a context
	conn.Execute("CREATE INDEX i ON t ((cpp_no_context(x)))").Drain();
	conn.Execute("INSERT INTO t VALUES (4)").Drain();
	REQUIRE(no_context_exec_runs >= 1);
	REQUIRE(no_context_init_runs >= 1);
	REQUIRE(CollectInts(conn.Execute("SELECT x FROM t WHERE cpp_no_context(x) = 9")) == std::vector<int32_t> {4});

	// A callback that asks for the context fails the query
	conn.Execute("CREATE INDEX j ON t ((cpp_needs_context(x)))").Drain();
	REQUIRE_THROWS_WITH(conn.Execute("INSERT INTO t VALUES (5)").Drain(),
	                    Catch::Matchers::Contains("runs without a client context"));
	REQUIRE(CollectInts(conn.Execute("SELECT cpp_needs_context(x) FROM t ORDER BY x")) ==
	        std::vector<int32_t> {1001, 1002, 1003, 1004});
}

TEST_CASE("Stable C++API: cast runs without a client context while an index is built", "[cpp_api]") {
	const auto create_index = "CREATE INDEX i ON t ((CAST(CAST(x AS UNITS) AS VARCHAR)))";
	{
		auto db = Instance(":memory:");
		auto conn = db.Connect();
		RegisterUnitsCast(conn, NoContextCastExec);
		conn.Execute("CREATE TABLE t (x INTEGER)").Drain();
		conn.Execute("INSERT INTO t VALUES (1), (2)").Drain();

		no_context_exec_runs = 0;
		conn.Execute(create_index).Drain();
		conn.Execute("INSERT INTO t VALUES (3)").Drain();
		REQUIRE(no_context_exec_runs >= 1);
	}
	{
		auto db = Instance(":memory:");
		auto conn = db.Connect();
		RegisterUnitsCast(conn, ContextCastExec);
		conn.Execute("CREATE TABLE t (x INTEGER)").Drain();
		conn.Execute("INSERT INTO t VALUES (1), (2)").Drain();

		conn.Execute(create_index).Drain();
		REQUIRE_THROWS_WITH(conn.Execute("INSERT INTO t VALUES (3)").Drain(),
		                    Catch::Matchers::Contains("runs without a client context"));
	}
}

TEST_CASE("Stable C++API: replacing the call rejects misuse", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterRewriteFunctions(conn);

	misuse_mode = MisuseMode::BIND_DATA_THEN_REPLACE;
	REQUIRE_THROWS_WITH(conn.Execute("SELECT cpp_misuse(1)").Drain(),
	                    Catch::Matchers::Contains("must be the only thing the bind callback does"));
	misuse_mode = MisuseMode::REPLACE_THEN_BIND_DATA;
	REQUIRE_THROWS_WITH(conn.Execute("SELECT cpp_misuse(1)").Drain(),
	                    Catch::Matchers::Contains("must be the only thing the bind callback does"));
	misuse_mode = MisuseMode::REPLACE_TWICE;
	REQUIRE_THROWS_WITH(conn.Execute("SELECT cpp_misuse(1)").Drain(),
	                    Catch::Matchers::Contains("can only be replaced once"));
	misuse_mode = MisuseMode::UNKNOWN;
	REQUIRE_THROWS_WITH(conn.Execute("SELECT cpp_misuse(1)").Drain(), Catch::Matchers::Contains("cpp_does_not_exist"));
	misuse_mode = MisuseMode::OUT_OF_BOUNDS;
	REQUIRE_THROWS_WITH(conn.Execute("SELECT cpp_misuse(1)").Drain(), Catch::Matchers::Contains("out of bounds"));
	misuse_mode = MisuseMode::SELF;
	REQUIRE_THROWS_WITH(conn.Execute("SELECT cpp_misuse(1)").Drain(),
	                    Catch::Matchers::Contains("cannot replace its own call"));
	misuse_mode = MisuseMode::STABILITY_THEN_REPLACE;
	REQUIRE_THROWS_WITH(conn.Execute("SELECT cpp_misuse(1)").Drain(),
	                    Catch::Matchers::Contains("replacement has its own stability"));
	misuse_mode = MisuseMode::REPLACE_THEN_STABILITY;
	REQUIRE_THROWS_WITH(conn.Execute("SELECT cpp_misuse(1)").Drain(),
	                    Catch::Matchers::Contains("replacement has its own stability"));
}

// VOLATILE as a function, CONSISTENT for a constant second argument; the bind data is set next to the stability
void StabilityBind(ScalarFunction::BindInput &input) {
	if (input.TryGetConstantArgument(1)) {
		input.SetStability(FunctionStability::CONSISTENT);
		input.SetBindData<int32_t>(input.GetConstantArgument(1).Get<int32_t>());
	} else {
		input.SetBindData<int32_t>(0);
	}
}

// out[i] = a[i] + bind data
void StabilityExec(ScalarFunction::ExecInput &input) {
	const auto add = input.GetBindData<int32_t>();
	const auto a = input.GetArg(0).GetView();
	auto result = input.GetResult();
	auto *out = result.GetDataMutable<int32_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		out[i] = a.Data<int32_t>()[a.SelAt(i)] + add;
	}
}

TEST_CASE("Stable C++API: the bind callback overrides the stability of the call", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	const auto integer = factory.ParseType("INTEGER");
	auto function = ScalarFunction::Create(factory);
	function.SetName("cpp_stab")
	    .SetStability(FunctionStability::VOLATILE)
	    .SetBindCallback(StabilityBind)
	    .SetExecCallback(StabilityExec);
	function.GetSignature().AddParameter("a", integer).AddParameter("k", integer).SetReturnType(integer);
	conn.Register(function);

	conn.Execute("CREATE TABLE t (x INTEGER, y INTEGER)").Drain();
	conn.Execute("INSERT INTO t VALUES (1, 10), (2, 20)").Drain();

	// The constant argument makes the call CONSISTENT, which an index key accepts
	conn.Execute("CREATE INDEX i ON t ((cpp_stab(x, 5)))").Drain();
	conn.Execute("INSERT INTO t VALUES (3, 30)").Drain();
	REQUIRE(CollectInts(conn.Execute("SELECT x FROM t WHERE cpp_stab(x, 5) = 8")) == std::vector<int32_t> {3});

	// A column argument keeps the VOLATILE property of the function
	REQUIRE_THROWS_WITH(conn.Execute("CREATE INDEX j ON t ((cpp_stab(x, y)))").Drain(),
	                    Catch::Matchers::Contains("Index keys cannot contain expressions with side effects"));
}

TEST_CASE("Stable C++API: scalar function bind data round-trips through plan serialization", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterSerFunction(conn, "cpp_ser", SerSerialize);

	const std::vector<int32_t> expected {10, 11, 12};
	ser_bind_runs = 0;
	REQUIRE(CollectInts(conn.Execute("SELECT cpp_ser(i::INTEGER, 10) FROM range(3) t(i)")) == expected);
	REQUIRE(ser_serialize_runs == 0);
	REQUIRE(ser_deserialize_runs == 0);

	// With the verification the plan is serialized and the query runs on the deserialized plan
	conn.Execute("SET debug_verify_serializer = true").Drain();
	ser_serialize_runs = 0;
	ser_deserialize_runs = 0;
	REQUIRE(CollectInts(conn.Execute("SELECT cpp_ser(i::INTEGER, 10) FROM range(3) t(i)")) == expected);
	REQUIRE(ser_serialize_runs >= 1);
	REQUIRE(ser_deserialize_runs >= 1);
}

TEST_CASE("Stable C++API: scalar function serialize callback failures surface", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterSerFunction(conn, "cpp_ser_fail", SerFailingSerialize);

	REQUIRE(CollectInts(conn.Execute("SELECT cpp_ser_fail(i::INTEGER, 10) FROM range(3) t(i)")) ==
	        std::vector<int32_t> {10, 11, 12});
	conn.Execute("SET debug_verify_serializer = true").Drain();
	REQUIRE_THROWS_WITH(conn.Execute("SELECT cpp_ser_fail(i::INTEGER, 10) FROM range(3) t(i)").Drain(),
	                    Catch::Matchers::Contains("cannot serialize on purpose"));
}

TEST_CASE("Stable C++API: scalar function serialize callbacks are set together", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto function = ScalarFunction::Create(conn.GetFactory());

	REQUIRE_THROWS_WITH(function.SetSerializeCallbacks(SerSerialize, nullptr),
	                    Catch::Matchers::Contains("must be set together"));
	REQUIRE_THROWS_WITH(function.SetSerializeCallbacks(nullptr, SerDeserialize),
	                    Catch::Matchers::Contains("must be set together"));
	// Both null clears them
	function.SetSerializeCallbacks(SerSerialize, SerDeserialize).SetSerializeCallbacks(nullptr, nullptr);
}

//----------------------------------------------------------------------------------------------------------------------
// Argument monotonicity and statistics propagation
//----------------------------------------------------------------------------------------------------------------------

namespace {

// out[i] = a[i] / 10
void DivTenExec(ScalarFunction::ExecInput &input) {
	const auto a = input.GetArg(0).GetView();
	auto result = input.GetResult();
	auto *out = result.GetDataMutable<int32_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		out[i] = a.Data<int32_t>()[a.SelAt(i)] / 10;
	}
}

void MonotoneBind(ScalarFunction::BindInput &input) {
	input.SetArgumentMonotonicity(0, Monotonicity::NON_DECREASING);
}

void ClearBind(ScalarFunction::BindInput &input) {
	input.SetStability(FunctionStability::CONSISTENT);
	input.SetBindData<int32_t>(1);
	input.SetArgumentMonotonicity(0, Monotonicity::UNKNOWN);
}

void BadIndexBind(ScalarFunction::BindInput &input) {
	input.SetArgumentMonotonicity(3, Monotonicity::NON_DECREASING);
}

void ReplaceThenMonotonicityBind(ScalarFunction::BindInput &input) {
	input.ReplaceCall("cpp_single", {0});
	input.SetArgumentMonotonicity(0, Monotonicity::NON_DECREASING);
}

void MonotonicityThenReplaceBind(ScalarFunction::BindInput &input) {
	input.SetArgumentMonotonicity(0, Monotonicity::NON_DECREASING);
	input.ReplaceCall("cpp_single", {0});
}

void NoopStats(ScalarFunction::StatsInput &) {
}

ScalarFunction MakeDivTen(Connection &conn, const char *name) {
	auto &factory = conn.GetFactory();
	const auto integer = factory.ParseType("INTEGER");
	auto function = ScalarFunction::Create(factory);
	function.SetName(name).SetExecCallback(DivTenExec);
	function.GetSignature().AddParameter("a", integer).SetReturnType(integer);
	return function;
}

// The optimized plan of a query, as one string
std::string OptimizedPlan(Connection &conn, const std::string &sql) {
	std::string plan;
	auto result = conn.Execute("EXPLAIN " + sql);
	while (auto chunk = result.FetchChunk()) {
		auto view = chunk.GetVector(1).GetView();
		for (idx_t i = 0; i < chunk.GetRowCount(); i++) {
			plan += std::string(view.Data<varchar_t>()[view.SelAt(i)].view());
		}
	}
	return plan;
}

} // namespace

TEST_CASE("Stable C++API: argument monotonicity turns a filter on the result into one on the argument", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto declared = MakeDivTen(conn, "cpp_declared");
	declared.SetArgumentMonotonicity(0, Monotonicity::NON_DECREASING)
	    .SetStatisticsPropagation(StatisticsPropagation::FINITE_MONOTONE_BOUNDS);
	conn.Register(declared);

	auto bound = MakeDivTen(conn, "cpp_bound");
	bound.SetBindCallback(MonotoneBind).SetStatisticsPropagation(StatisticsPropagation::FINITE_MONOTONE_BOUNDS);
	conn.Register(bound);

	auto cleared = MakeDivTen(conn, "cpp_cleared");
	cleared.SetArgumentMonotonicity(0, Monotonicity::NON_DECREASING)
	    .SetBindCallback(ClearBind)
	    .SetStatisticsPropagation(StatisticsPropagation::FINITE_MONOTONE_BOUNDS);
	conn.Register(cleared);

	auto none = MakeDivTen(conn, "cpp_none");
	none.SetArgumentMonotonicity(0, Monotonicity::NON_DECREASING);
	conn.Register(none);

	conn.Execute("PRAGMA explain_output = 'OPTIMIZED_ONLY'").Drain();
	conn.Execute("CREATE TABLE t AS SELECT i::INTEGER AS x FROM range(10000) r(i)").Drain();

	for (const char *name : {"cpp_declared", "cpp_bound"}) {
		auto plan = OptimizedPlan(conn, std::string("SELECT count(*) FROM t WHERE ") + name + "(x) = 3");
		INFO(plan);
		REQUIRE(plan.find(name) == std::string::npos);
		REQUIRE(plan.find(">=") != std::string::npos);
		REQUIRE(plan.find("<=") != std::string::npos);
		REQUIRE(CollectInts(conn.Execute("SELECT count(*)::INTEGER FROM t WHERE " + std::string(name) + "(x) = 3")) ==
		        std::vector<int32_t> {10});
	}

	// The bind callback clears the declaration
	auto cleared_plan = OptimizedPlan(conn, "SELECT count(*) FROM t WHERE cpp_cleared(x) = 3");
	INFO(cleared_plan);
	REQUIRE(cleared_plan.find("cpp_cleared") != std::string::npos);
	REQUIRE(CollectInts(conn.Execute("SELECT count(*)::INTEGER FROM t WHERE cpp_cleared(x) = 3")) ==
	        std::vector<int32_t> {10});

	// The declaration alone rewrites the filter; the statistics of the result need the propagation property: the
	// largest result is 999, so a filter on 5000 is empty only for the function that propagates its bounds
	REQUIRE(OptimizedPlan(conn, "SELECT count(*) FROM t WHERE cpp_declared(x) = 5000").find("Empty Result") !=
	        std::string::npos);
	REQUIRE(OptimizedPlan(conn, "SELECT count(*) FROM t WHERE cpp_cleared(x) = 5000").find("Empty Result") ==
	        std::string::npos);
	REQUIRE(CollectInts(conn.Execute("SELECT count(*)::INTEGER FROM t WHERE cpp_declared(x) = 5000")) ==
	        std::vector<int32_t> {0});
	REQUIRE(CollectInts(conn.Execute("SELECT count(*)::INTEGER FROM t WHERE cpp_none(x) = 5000")) ==
	        std::vector<int32_t> {0});
}

TEST_CASE("Stable C++API: argument monotonicity rejects misuse", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterRewriteFunctions(conn);

	// An index beyond the signature is rejected when the function is registered
	auto bad_index = MakeDivTen(conn, "cpp_bad_index");
	bad_index.SetArgumentMonotonicity(1, Monotonicity::NON_DECREASING);
	REQUIRE_THROWS_WITH(conn.Register(bad_index), Catch::Matchers::Contains("out of bounds"));

	// Propagating bounds needs the engine's callback: a stats callback of the function cannot go with it
	auto with_stats = MakeDivTen(conn, "cpp_with_stats");
	with_stats.SetStatsCallback(NoopStats).SetStatisticsPropagation(StatisticsPropagation::FINITE_MONOTONE_BOUNDS);
	REQUIRE_THROWS_WITH(conn.Register(with_stats),
	                    Catch::Matchers::Contains("cannot be combined with a stats callback"));

	// NONE next to a stats callback is fine
	auto none_with_stats = MakeDivTen(conn, "cpp_none_with_stats");
	none_with_stats.SetStatsCallback(NoopStats).SetStatisticsPropagation(StatisticsPropagation::NONE);
	conn.Register(none_with_stats);

	// A bind index beyond the arguments of the call fails the query
	auto bind_index = MakeDivTen(conn, "cpp_bind_index");
	bind_index.SetBindCallback(BadIndexBind);
	conn.Register(bind_index);
	REQUIRE_THROWS_WITH(conn.Execute("SELECT cpp_bind_index(1)").Drain(), Catch::Matchers::Contains("out of bounds"));

	// The replacement has its own properties
	auto replace_first = MakeDivTen(conn, "cpp_replace_first");
	replace_first.SetBindCallback(ReplaceThenMonotonicityBind);
	conn.Register(replace_first);
	REQUIRE_THROWS_WITH(conn.Execute("SELECT cpp_replace_first(1)").Drain(),
	                    Catch::Matchers::Contains("the replacement has its own properties"));
	auto monotonicity_first = MakeDivTen(conn, "cpp_monotonicity_first");
	monotonicity_first.SetBindCallback(MonotonicityThenReplaceBind);
	conn.Register(monotonicity_first);
	REQUIRE_THROWS_WITH(conn.Execute("SELECT cpp_monotonicity_first(1)").Drain(),
	                    Catch::Matchers::Contains("the replacement has its own properties"));
}
