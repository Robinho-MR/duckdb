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

enum class MisuseMode { BIND_DATA_THEN_REPLACE, REPLACE_THEN_BIND_DATA, REPLACE_TWICE, UNKNOWN, OUT_OF_BOUNDS, SELF };
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

TEST_CASE("Stable C++API: replacing the call checks the return type", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterRewriteFunctions(conn);

	REQUIRE_THROWS_WITH(conn.Execute("SELECT cpp_mismatch(1)").Drain(),
	                    Catch::Matchers::Contains("returns VARCHAR, but the call resolved to return INTEGER"));
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
