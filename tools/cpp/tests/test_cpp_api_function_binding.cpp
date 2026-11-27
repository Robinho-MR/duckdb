#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <atomic>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: what a function can decide when it is bound -- the bind
// and init callbacks and the stability of a cast, the replacement of a scalar
// call by another function or by the overload it overrode, the stability and
// monotonicity of a call -- and table in-out functions built on an Operator.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

std::vector<std::string> CollectStrings(QueryResult result, idx_t column = 0) {
	std::vector<std::string> out;
	while (auto chunk = result.FetchChunk()) {
		auto view = chunk.GetVector(column).GetView();
		for (idx_t i = 0; i < chunk.GetRowCount(); i++) {
			out.emplace_back(view.IsValid(i) ? std::string(view.Data<varchar_t>()[view.SelAt(i)].view()) : "NULL");
		}
	}
	return out;
}

std::vector<int64_t> CollectBigints(QueryResult result) {
	std::vector<int64_t> out;
	while (auto chunk = result.FetchChunk()) {
		auto view = chunk.GetVector(0).GetView();
		for (idx_t i = 0; i < chunk.GetRowCount(); i++) {
			REQUIRE(view.IsValid(i));
			out.push_back(view.Data<int64_t>()[view.SelAt(i)]);
		}
	}
	return out;
}

std::string Plan(Connection &conn, const std::string &sql) {
	std::string plan;
	for (auto &line : CollectStrings(conn.Execute("EXPLAIN " + sql), 1)) {
		plan += line;
	}
	return plan;
}

//----------------------------------------------------------------------------------------------------------------------
// Casts
//----------------------------------------------------------------------------------------------------------------------
// Casts an INTEGER wrapped in a custom type to VARCHAR, with the setting `cast_prefix` read at bind time
struct PrefixBind {
	std::string prefix;
	bool operator==(const PrefixBind &other) const {
		return prefix == other.prefix;
	}
};

struct CallCounter {
	int64_t rows = 0;
};

std::atomic<int64_t> init_calls {0};
std::atomic<int64_t> bind_calls {0};
std::atomic<bool> bound_without_context {false};

void PrefixCastBind(CastFunction::BindInput &input) {
	bind_calls++;
	if (!input.HasContext()) {
		bound_without_context = true;
		input.SetBindData<PrefixBind>(PrefixBind {"?"});
		return;
	}
	REQUIRE_NOTHROW(input.GetSourceType());
	auto prefix = input.GetContext().GetConfig().GetOption("cast_prefix").value;
	if (prefix.IsNull()) {
		throw InvalidInputException("cast_prefix is not set");
	}
	input.SetBindData<PrefixBind>(PrefixBind {std::string(prefix.Get<varchar_t>().view())});
}

void PrefixCastInit(CastFunction::InitInput &input) {
	init_calls++;
	input.SetInitData<CallCounter>();
}

void PrefixCastExec(CastFunction::ExecInput &input) {
	auto &bind = input.GetBindData<PrefixBind>();
	auto &counter = input.GetInitData<CallCounter>();
	auto in = input.GetInput().GetView();
	auto out = input.GetOutput();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		counter.rows++;
		out.AssignString(i, bind.prefix + std::to_string(in.Data<int32_t>()[in.SelAt(i)]));
	}
}

void RegisterPrefixCast(Connection &conn) {
	auto &factory = conn.GetFactory();
	auto type = CustomType::Create(factory);
	type.SetName("PREFIXED").SetBaseType(factory.ParseType("INTEGER"));
	conn.Register(type);

	auto option = CustomOption::Create(factory);
	option.SetName("cast_prefix").SetType(factory.ParseType("VARCHAR"));
	conn.Register(option);

	auto cast = CastFunction::Create(factory);
	cast.SetSourceType(factory.ParseType("PREFIXED"))
	    .SetTargetType(factory.ParseType("VARCHAR"))
	    .SetBindCallback(PrefixCastBind)
	    .SetInitCallback(PrefixCastInit)
	    .SetExecCallback(PrefixCastExec)
	    .SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
	conn.Register(cast);
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar functions
//----------------------------------------------------------------------------------------------------------------------
void TimesTenExec(ScalarFunction::ExecInput &input) {
	ScalarExecutor::Execute<int64_t, int64_t>(input, [](int64_t a) { return a * 10; });
}

void PickExec(ScalarFunction::ExecInput &input) {
	ScalarExecutor::Execute<int64_t, int64_t, int64_t>(input, [](int64_t, int64_t b) { return b; });
}

// pick(selector, value): a constant selector 10 replaces the call with times_ten(value)
void PickBind(ScalarFunction::BindInput &input) {
	auto selector = input.TryGetConstantArgument(0);
	if (selector && !selector->IsNull() && selector->Get<int64_t>() == 10) {
		input.ReplaceCall("times_ten", {1});
	}
}

// A replacement of the built-in lower(VARCHAR) that only handles the text "magic"
void LowerExec(ScalarFunction::ExecInput &input) {
	auto result = input.GetResult();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		result.AssignString(i, "replaced");
	}
}

void LowerBind(ScalarFunction::BindInput &input) {
	auto text = input.TryGetConstantArgument(0);
	if (!text || text->IsNull() || text->Get<varchar_t>().view() != "magic") {
		input.CallOverridden();
	}
}

void NowishExec(ScalarFunction::ExecInput &input) {
	const auto now = input.GetContext().GetTransactionStartTime();
	ScalarExecutor::Execute<int64_t>(input, [&]() { return now.micros; });
}

void StableBind(ScalarFunction::BindInput &input) {
	input.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
	input.SetArgumentMonotonicity(0, FunctionMonotonicity::UNKNOWN);
}

void CountForeverExec(ScalarFunction::ExecInput &input) {
	// a long loop checks for interrupts
	while (true) {
		input.GetContext().InterruptCheck();
	}
}

//----------------------------------------------------------------------------------------------------------------------
// In-out table functions
//----------------------------------------------------------------------------------------------------------------------
struct RepeatState {
	idx_t row = 0;
	int64_t emitted = 0;
};

void RepeatBind(TableFunction::BindInput &input) {
	input.AddResultColumn("value", input.GetContext().GetFactory().ParseType("BIGINT"));
}

void RepeatInitLocal(Operator::InitLocalInput &input) {
	input.SetLocalState<RepeatState>();
}

// repeat_each(n): emits n, n times, for every input row
void RepeatExecute(Operator::ExecuteInput &input) {
	auto &state = input.GetLocalState<RepeatState>();
	auto chunk = input.GetInput();
	chunk.GetVector(0).Flatten();
	auto in = chunk.GetVector(0).GetView();
	auto output = input.GetOutput();
	auto out = output.GetVector(0);
	auto data = out.GetDataMutable<int64_t>();
	idx_t size = 0;
	while (state.row < chunk.GetRowCount() && size < output.GetCapacity()) {
		const auto n = in.Data<int64_t>()[in.SelAt(state.row)];
		if (state.emitted >= n) {
			state.row++;
			state.emitted = 0;
			continue;
		}
		data[size++] = n;
		state.emitted++;
	}
	out.SetSize(size);
	if (state.row >= chunk.GetRowCount()) {
		state.row = 0;
		input.SetResult(OperatorResult::NEED_MORE_INPUT);
	} else {
		input.SetResult(OperatorResult::HAVE_MORE_OUTPUT);
	}
}

} // namespace

TEST_CASE("Stable C++API: cast bind, init and stability", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterPrefixCast(conn);

	// The bind refuses the cast while the setting is unset, which fails the statement
	REQUIRE_THROWS_WITH(conn.Execute("SELECT 1::PREFIXED::VARCHAR").Drain(), Catch::Contains("cast_prefix is not set"));

	conn.Execute("SET cast_prefix = 'n='").Drain();
	init_calls = 0;
	REQUIRE(CollectStrings(conn.Execute("SELECT i::PREFIXED::VARCHAR FROM (VALUES (0::BIGINT), (1), (2)) t(i)")) ==
	        std::vector<std::string> {"n=0", "n=1", "n=2"});
	REQUIRE(init_calls > 0);

	// The bind reads the setting again for every query
	conn.Execute("SET cast_prefix = 'v='").Drain();
	REQUIRE(CollectStrings(conn.Execute("SELECT 7::PREFIXED::VARCHAR")) == std::vector<std::string> {"v=7"});

	// A cast that is consistent only within a query cannot be part of an index expression
	conn.Execute("CREATE TABLE t (i PREFIXED)").Drain();
	REQUIRE_THROWS(conn.Execute("CREATE INDEX idx ON t ((i::VARCHAR))").Drain());
}

TEST_CASE("Stable C++API: cast property and implicit cast cost", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	auto cost = CastFunction::GetImplicitCastCost(factory, factory.ParseType("INTEGER"), factory.ParseType("BIGINT"));
	REQUIRE(cost >= 0);
	REQUIRE(CastFunction::GetImplicitCastCost(factory, factory.ParseType("VARCHAR"), factory.ParseType("INTEGER")) ==
	        -1);

	auto cast = CastFunction::Create(factory);
	REQUIRE_THROWS_AS(cast.SetStability(FunctionStability::VOLATILE), Exception);
}

TEST_CASE("Stable C++API: a scalar call replaces itself with another function", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	auto bigint = factory.ParseType("BIGINT");

	auto times_ten = ScalarFunction::Create(factory);
	times_ten.SetName("times_ten").SetExecCallback(TimesTenExec);
	times_ten.GetSignature().AddParameter("a", bigint).SetReturnType(bigint);
	conn.Register(times_ten);

	auto pick = ScalarFunction::Create(factory);
	pick.SetName("pick").SetBindCallback(PickBind).SetExecCallback(PickExec);
	pick.GetSignature().AddParameter("selector", bigint).AddParameter("value", bigint).SetReturnType(bigint);
	conn.Register(pick);

	REQUIRE(CollectBigints(conn.Execute("SELECT pick(10, i) FROM (VALUES (0::BIGINT), (1), (2)) t(i)")) ==
	        std::vector<int64_t> {0, 10, 20});
	REQUIRE(Plan(conn, "SELECT pick(10, i) FROM (VALUES (0::BIGINT), (1), (2)) t(i)").find("times_ten") !=
	        std::string::npos);
	// a selector that is not constant keeps the call
	REQUIRE(CollectBigints(conn.Execute("SELECT pick(i, i) FROM (VALUES (0::BIGINT), (1), (2)) t(i)")) ==
	        std::vector<int64_t> {0, 1, 2});
}

TEST_CASE("Stable C++API: a scalar function hands calls back to the overload it overrode", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	auto varchar = factory.ParseType("VARCHAR");

	auto lower = ScalarFunction::Create(factory);
	lower.SetName("lower").SetBindCallback(LowerBind).SetExecCallback(LowerExec);
	lower.GetSignature().AddParameter("string", varchar).SetReturnType(varchar);
	conn.Register(lower);

	REQUIRE(CollectStrings(conn.Execute("SELECT lower('magic')")) == std::vector<std::string> {"replaced"});
	REQUIRE(CollectStrings(conn.Execute("SELECT lower('HeLLo')")) == std::vector<std::string> {"hello"});

	// A function that overrode nothing cannot hand a call back
	auto fresh = ScalarFunction::Create(factory);
	fresh.SetName("fresh_lower").SetBindCallback(LowerBind).SetExecCallback(LowerExec);
	fresh.GetSignature().AddParameter("string", varchar).SetReturnType(varchar);
	conn.Register(fresh);
	REQUIRE_THROWS_WITH(conn.Execute("SELECT fresh_lower('x')").Drain(),
	                    Catch::Contains("did not override an overload"));
}

TEST_CASE("Stable C++API: the stability and monotonicity of a call", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	auto bigint = factory.ParseType("BIGINT");

	// declared monotone at registration, withdrawn per call
	auto stable = ScalarFunction::Create(factory);
	stable.SetName("stable_ten").SetBindCallback(StableBind).SetExecCallback(TimesTenExec);
	stable.SetArgumentMonotonicity(0, FunctionMonotonicity::STRICTLY_INCREASING);
	stable.GetSignature().AddParameter("a", bigint).SetReturnType(bigint);
	conn.Register(stable);
	REQUIRE(CollectBigints(conn.Execute("SELECT stable_ten(4)")) == std::vector<int64_t> {40});
	conn.Execute("CREATE TABLE t (i BIGINT)").Drain();
	// a call that is consistent only within a query cannot be part of an index expression
	REQUIRE_THROWS(conn.Execute("CREATE INDEX idx ON t (stable_ten(i))").Drain());

	// the transaction start time is the same for every statement of a transaction
	auto nowish = ScalarFunction::Create(factory);
	nowish.SetName("nowish").SetExecCallback(NowishExec);
	nowish.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
	nowish.GetSignature().SetReturnType(bigint);
	conn.Register(nowish);
	conn.Execute("BEGIN").Drain();
	auto first = CollectBigints(conn.Execute("SELECT nowish()"));
	auto second = CollectBigints(conn.Execute("SELECT nowish()"));
	REQUIRE(first == second);
	REQUIRE(first == CollectBigints(conn.Execute("SELECT epoch_us(get_current_timestamp())")));
	conn.Execute("COMMIT").Drain();
}

TEST_CASE("Stable C++API: a long callback can be interrupted", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	auto bigint = factory.ParseType("BIGINT");

	auto forever = ScalarFunction::Create(factory);
	forever.SetName("count_forever").SetExecCallback(CountForeverExec);
	forever.SetStability(FunctionStability::VOLATILE);
	forever.GetSignature().SetReturnType(bigint);
	conn.Register(forever);
	conn.Execute("SET max_execution_time = 200").Drain();
	REQUIRE_THROWS_AS(conn.Execute("SELECT count_forever()").Drain(), InterruptException);
}

TEST_CASE("Stable C++API: a table in-out function on an operator", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	auto op = Operator::Create(factory);
	op.SetInitLocalCallback(RepeatInitLocal).SetExecuteCallback(RepeatExecute);

	auto repeat = TableFunction::Create(factory);
	repeat.SetName("repeat_each").SetBindCallback(RepeatBind);
	repeat.GetSignature().AddParameter("n", factory.ParseType("BIGINT"), FunctionParameterKind::POSITIONAL_ONLY);
	repeat.SetOperator(op, TableOperatorInput::ARGUMENTS);
	conn.Register(repeat);

	// constant arguments
	REQUIRE(CollectBigints(conn.Execute("SELECT * FROM repeat_each(3)")) == std::vector<int64_t> {3, 3, 3});
	// lateral arguments, one input row per row of the table
	REQUIRE(CollectBigints(conn.Execute("SELECT value FROM range(1, 4) t(i), repeat_each(i) ORDER BY ALL")) ==
	        std::vector<int64_t> {1, 2, 2, 3, 3, 3});
	// more rows than fit an output chunk
	REQUIRE(CollectBigints(conn.Execute("SELECT count(*) FROM repeat_each(5000)")) == std::vector<int64_t> {5000});
}

TEST_CASE("Stable C++API: typed exceptions", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	REQUIRE_THROWS_AS(conn.Execute("SELECT 'x'::INTEGER").Drain(), ConversionException);
	REQUIRE_THROWS_AS(conn.Execute("SELECT * FROM no_such_table").Drain(), Exception);
	REQUIRE_THROWS_AS(conn.Execute("SELECT 1 + 'a'::DATE").Drain(), Exception);
}
