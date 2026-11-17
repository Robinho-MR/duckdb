#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: CustomType and CastFunction. A custom type TEMPERATURE
// (an alias of INTEGER) plus the two casts between it and VARCHAR, reached
// through CAST, TRY_CAST and implicit argument conversion.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

// Collect a single VARCHAR column, asserting every row valid.
std::vector<std::string> CollectCastStrings(QueryResult result) {
	std::vector<std::string> out;
	while (auto chunk = result.FetchChunk()) {
		auto view = chunk.GetVector(0).GetView();
		for (idx_t i = 0; i < chunk.GetRowCount(); i++) {
			REQUIRE(view.IsValid(i));
			out.emplace_back(view.Data<varchar_t>()[view.SelAt(i)].view());
		}
	}
	return out;
}

// Collect a single INTEGER column, reading a NULL row as nullopt.
std::vector<std::optional<int32_t>> CollectMaybeInts(QueryResult result) {
	std::vector<std::optional<int32_t>> out;
	while (auto chunk = result.FetchChunk()) {
		auto view = chunk.GetVector(0).GetView();
		for (idx_t i = 0; i < chunk.GetRowCount(); i++) {
			if (!view.IsValid(i)) {
				out.emplace_back();
			} else {
				out.emplace_back(view.Data<int32_t>()[view.SelAt(i)]);
			}
		}
	}
	return out;
}

// The suffix a temperature renders with. Together with the digits it is longer than blob_t::INLINE_LENGTH, so the
// bytes land in the vector's arena: an assertion build compiles the engine with DUCKDB_DEBUG_NO_INLINE, whose
// string_t reads the pointer even for values the API would inline.
constexpr const char *SUFFIX = " degrees celsius";

// Carried through SetUserData to pin that the exec callback sees it.
struct CastTag {
	std::string value;
};

// The mode the last cast ran in, latched for the tests to assert on.
CastMode last_mode = CastMode::NORMAL;

// TEMPERATURE -> VARCHAR. Infallible: every value renders.
void TempToText(CastFunction::ExecInput &input) {
	last_mode = input.GetMode();
	auto in = input.GetInput();
	auto out = input.GetOutput();
	auto view = in.GetView();
	auto validity = out.GetValidityMutable();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!view.IsValid(i)) {
			validity.SetInvalid(i);
			continue;
		}
		out.AssignString(i, std::to_string(view.Data<int32_t>()[view.SelAt(i)]) + SUFFIX);
	}
}

// VARCHAR -> TEMPERATURE. A row that does not parse is left NULL and reported by throwing: a normal cast aborts on
// the exception, a try cast swallows it and keeps the NULL.
void TextToTemp(CastFunction::ExecInput &input) {
	last_mode = input.GetMode();
	REQUIRE(input.GetUserData<CastTag>().value == "secret");

	auto in = input.GetInput();
	auto out = input.GetOutput();
	auto view = in.GetView();
	auto validity = out.GetValidityMutable();
	auto *values = out.GetDataMutable<int32_t>();

	std::string failure;
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!view.IsValid(i)) {
			validity.SetInvalid(i);
			continue;
		}
		const auto text = std::string(view.Data<varchar_t>()[view.SelAt(i)].view());
		const auto suffix = std::string(SUFFIX);
		if (text.size() <= suffix.size() || text.compare(text.size() - suffix.size(), suffix.size(), suffix) != 0) {
			validity.SetInvalid(i);
			failure = text;
			continue;
		}
		try {
			values[i] = std::stoi(text.substr(0, text.size() - suffix.size()));
		} catch (const std::exception &) {
			validity.SetInvalid(i);
			failure = text;
		}
	}
	// Reported once the whole batch is written, so the rows that did convert are in place either way.
	if (!failure.empty()) {
		throw InvalidInputException("could not convert '" + failure + "' to TEMPERATURE_CELSIUS");
	}
}

void NoopCast(CastFunction::ExecInput &) {
}

// Registers the TEMPERATURE type and hands back a logical type handle for it.
auto RegisterTemperature(Connection &conn) -> LogicalType {
	auto &factory = conn.GetFactory();
	auto type = CustomType::Create(conn.GetFactory());
	type.SetName("TEMPERATURE_CELSIUS").SetBaseType(factory.ParseType("INTEGER"));
	conn.Register(type);
	return factory.ParseType("INTEGER").WithAlias(factory, "TEMPERATURE_CELSIUS");
}

// Registers both casts, with `cost` as the VARCHAR -> TEMPERATURE implicit cast cost.
void RegisterTemperatureCasts(Connection &conn, const LogicalType &temperature, int64_t cost) {
	auto &factory = conn.GetFactory();
	const auto varchar = factory.ParseType("VARCHAR");

	auto to_text = CastFunction::Create(conn.GetFactory());
	to_text.SetSourceType(temperature).SetTargetType(varchar).SetExecCallback(TempToText);
	conn.Register(to_text);

	auto from_text = CastFunction::Create(conn.GetFactory());
	from_text.SetSourceType(varchar).SetTargetType(temperature).SetImplicitCastCost(cost).SetExecCallback(TextToTemp);
	from_text.SetUserData<CastTag>(CastTag {"secret"});
	conn.Register(from_text);
}

// reading(t TEMPERATURE_CELSIUS) -> INTEGER: an identity scalar function, to observe implicit argument conversion.
void ReadingExec(ScalarFunction::ExecInput &input) {
	auto view = input.GetArg(0).GetView();
	auto *out = input.GetResult().GetDataMutable<int32_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		out[i] = view.Data<int32_t>()[view.SelAt(i)];
	}
}

void RegisterReading(Connection &conn, const LogicalType &temperature) {
	auto &factory = conn.GetFactory();
	auto function = ScalarFunction::Create(conn.GetFactory());
	function.SetName("reading");
	function.WithSignature([&](FunctionSignature &sig) {
		sig.AddParameter("t", temperature);
		sig.SetReturnType(factory.ParseType("INTEGER"));
	});
	function.SetExecCallback(ReadingExec);
	conn.Register(function);
}

} // namespace

TEST_CASE("Stable C++API: custom type registers and resolves in SQL", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	auto type = CustomType::Create(conn.GetFactory());
	type.SetName("TEMPERATURE_CELSIUS").SetBaseType(factory.ParseType("INTEGER"));
	conn.Register(type);

	// The name resolves case-insensitively, and a value of it reports the custom name rather than the base type's.
	REQUIRE(CollectCastStrings(conn.Execute("SELECT typeof(CAST(42 AS temperature_celsius))")) ==
	        std::vector<std::string> {"TEMPERATURE_CELSIUS"});
	// The representation is the base type's, so the base type's operations still apply.
	REQUIRE(CollectMaybeInts(conn.Execute("SELECT CAST(42 AS temperature_celsius)::INTEGER + 1")) ==
	        std::vector<std::optional<int32_t>> {43});
}

TEST_CASE("Stable C++API: custom type registration errors", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();

	// No name.
	{
		auto type = CustomType::Create(conn.GetFactory());
		type.SetBaseType(factory.ParseType("INTEGER"));
		REQUIRE_THROWS_MATCHES(conn.Register(type), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	// No base type.
	{
		auto type = CustomType::Create(conn.GetFactory());
		type.SetName("no_base");
		REQUIRE_THROWS_MATCHES(conn.Register(type), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
}

TEST_CASE("Stable C++API: cast function round-trips a custom type", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto temperature = RegisterTemperature(conn);
	RegisterTemperatureCasts(conn, temperature, -1);

	last_mode = CastMode::TRY;
	REQUIRE(CollectCastStrings(conn.Execute("SELECT CAST(CAST(42 AS TEMPERATURE_CELSIUS) AS VARCHAR)")) ==
	        std::vector<std::string> {"42 degrees celsius"});
	// An explicit CAST runs in normal mode.
	REQUIRE(last_mode == CastMode::NORMAL);

	// And back the other way, through the base type so the result reads as an integer.
	REQUIRE(
	    CollectMaybeInts(conn.Execute("SELECT CAST(CAST('100 degrees celsius' AS TEMPERATURE_CELSIUS) AS INTEGER)")) ==
	    std::vector<std::optional<int32_t>> {100});

	// NULLs reach the callback, which propagates them.
	REQUIRE(CollectMaybeInts(conn.Execute("SELECT CAST(CAST(NULL AS TEMPERATURE_CELSIUS) AS VARCHAR)::INTEGER")) ==
	        std::vector<std::optional<int32_t>> {std::nullopt});

	// Several rows in one batch, so the callback sees a vector rather than a constant.
	REQUIRE(CollectCastStrings(conn.Execute("SELECT CAST(CAST(t AS TEMPERATURE_CELSIUS) AS VARCHAR) "
	                                        "FROM (VALUES (1), (2)) v(t) ORDER BY t")) ==
	        std::vector<std::string> {"1 degrees celsius", "2 degrees celsius"});
}

TEST_CASE("Stable C++API: cast function normal and try modes", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto temperature = RegisterTemperature(conn);
	RegisterTemperatureCasts(conn, temperature, -1);

	// A normal cast: the callback's exception aborts the query.
	last_mode = CastMode::TRY;
	REQUIRE_THROWS_AS(conn.Execute("SELECT CAST('not-a-temp' AS TEMPERATURE_CELSIUS)").Drain(), Exception);
	REQUIRE(last_mode == CastMode::NORMAL);

	// A try cast: the exception is swallowed and the row the callback left NULL is kept.
	last_mode = CastMode::NORMAL;
	REQUIRE(CollectMaybeInts(conn.Execute("SELECT TRY_CAST('not-a-temp' AS TEMPERATURE_CELSIUS)::INTEGER")) ==
	        std::vector<std::optional<int32_t>> {std::nullopt});
	REQUIRE(last_mode == CastMode::TRY);

	// A try cast over valid input still converts.
	REQUIRE(CollectMaybeInts(conn.Execute("SELECT TRY_CAST('12 degrees celsius' AS TEMPERATURE_CELSIUS)::INTEGER")) ==
	        std::vector<std::optional<int32_t>> {12});
}

TEST_CASE("Stable C++API: cast function implicit cast cost", "[cpp_api]") {
	// The probe is a scalar function whose only parameter is the custom type: reaching it from a VARCHAR column is
	// exactly the implicit conversion the cost governs. The argument has to come from a column, since a string
	// literal is bound loosely and reaches any parameter type regardless of the cast's cost.
	const auto probe = "SELECT reading(v) FROM (VALUES ('30 degrees celsius')) t(v)";

	// A negative cost -- the default -- keeps the cast out of implicit conversion.
	{
		auto db = Instance(":memory:");
		auto conn = db.Connect();
		auto temperature = RegisterTemperature(conn);
		RegisterTemperatureCasts(conn, temperature, -1);
		RegisterReading(conn, temperature);

		REQUIRE(CollectMaybeInts(conn.Execute("SELECT reading(CAST('30 degrees celsius' AS TEMPERATURE_CELSIUS))")) ==
		        std::vector<std::optional<int32_t>> {30});
		REQUIRE_THROWS_AS(conn.Execute(probe).Drain(), Exception);
	}

	// A non-negative cost makes the same cast available to the binder.
	{
		auto db = Instance(":memory:");
		auto conn = db.Connect();
		auto temperature = RegisterTemperature(conn);
		RegisterTemperatureCasts(conn, temperature, 0);
		RegisterReading(conn, temperature);

		REQUIRE(CollectMaybeInts(conn.Execute(probe)) == std::vector<std::optional<int32_t>> {30});
	}
}

TEST_CASE("Stable C++API: cast function registration errors", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	const auto integer = factory.ParseType("INTEGER");
	const auto varchar = factory.ParseType("VARCHAR");

	// Nothing configured, then each missing piece in turn.
	auto function = CastFunction::Create(conn.GetFactory());
	REQUIRE_THROWS_MATCHES(conn.Register(function), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	function.SetSourceType(integer);
	REQUIRE_THROWS_MATCHES(conn.Register(function), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	function.SetTargetType(varchar);
	REQUIRE_THROWS_MATCHES(conn.Register(function), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	function.SetExecCallback(NoopCast);
	conn.Register(function);

	// A cast whose exec callback never asks for user data still needs none.
	auto other = CastFunction::Create(conn.GetFactory());
	other.SetSourceType(varchar).SetTargetType(integer).SetExecCallback(NoopCast);
	conn.Register(other);
}

namespace {

// Source wildcards: renders the concrete type of the input, proving the callback was reached for that type.
void SourceTypeToText(CastFunction::ExecInput &input) {
	auto in = input.GetInput();
	auto out = input.GetOutput();
	const auto text = in.GetLogicalType().ToText();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		out.AssignString(i, text);
	}
}

std::string last_target_type;

// Target wildcards: latches the concrete type of the output and leaves every row NULL.
void TargetTypeProbe(CastFunction::ExecInput &input) {
	auto out = input.GetOutput();
	last_target_type = out.GetLogicalType().ToText();
	auto validity = out.GetValidityMutable();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		validity.SetInvalid(i);
	}
}

void ExactListCast(CastFunction::ExecInput &input) {
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		input.GetOutput().AssignString(i, "exact");
	}
}

auto RegisterTag(Connection &conn) -> LogicalType {
	auto &factory = conn.GetFactory();
	auto type = CustomType::Create(factory);
	type.SetName("WILD_TAG").SetBaseType(factory.ParseType("VARCHAR"));
	conn.Register(type);
	return factory.ParseType("VARCHAR").WithAlias(factory, "WILD_TAG");
}

const std::vector<CastWildcardKind> WILDCARD_KINDS = {
    CastWildcardKind::LIST,  CastWildcardKind::STRUCT,  CastWildcardKind::MAP,  CastWildcardKind::UNION,
    CastWildcardKind::ARRAY, CastWildcardKind::DECIMAL, CastWildcardKind::ENUM, CastWildcardKind::TUPLE};

} // namespace

TEST_CASE("Stable C++API: wildcard source casts match every type of a kind", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto tag = RegisterTag(conn);
	for (auto kind : WILDCARD_KINDS) {
		auto function = CastFunction::Create(conn.GetFactory());
		function.SetSourceKind(kind).SetTargetType(tag).SetExecCallback(SourceTypeToText);
		conn.Register(function);
	}
	auto text = [&](const std::string &expr) {
		return CollectCastStrings(conn.Execute("SELECT CAST(CAST(" + expr + " AS WILD_TAG) AS VARCHAR)"));
	};
	REQUIRE(text("[1, 2]") == std::vector<std::string> {"INTEGER[]"});
	REQUIRE(text("[['a']]") == std::vector<std::string> {"VARCHAR[][]"});
	REQUIRE(text("{'a': 1, 'b': 'x'}") == std::vector<std::string> {"STRUCT(a INTEGER, b VARCHAR)"});
	REQUIRE(text("map {1: 'x'}") == std::vector<std::string> {"MAP(INTEGER, VARCHAR)"});
	REQUIRE(text("union_value(num := 1)") == std::vector<std::string> {"UNION(num INTEGER)"});
	REQUIRE(text("[1, 2, 3]::INTEGER[3]") == std::vector<std::string> {"INTEGER[3]"});
	REQUIRE(text("[1]::BIGINT[1]") == std::vector<std::string> {"BIGINT[1]"});
	REQUIRE(text("1.5::DECIMAL(4, 1)") == std::vector<std::string> {"DECIMAL(4,1)"});
	REQUIRE(text("1::DECIMAL(30, 2)") == std::vector<std::string> {"DECIMAL(30,2)"});
	conn.Execute("CREATE TYPE mood AS ENUM ('sad', 'ok')").Drain();
	REQUIRE(text("'ok'::mood") == std::vector<std::string> {"ENUM('sad', 'ok')"});
}

TEST_CASE("Stable C++API: wildcard target casts match every type of a kind", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto tag = RegisterTag(conn);
	for (auto kind : WILDCARD_KINDS) {
		auto function = CastFunction::Create(conn.GetFactory());
		function.SetSourceType(tag).SetTargetKind(kind).SetExecCallback(TargetTypeProbe);
		conn.Register(function);
	}
	conn.Execute("CREATE TYPE mood AS ENUM ('sad', 'ok')").Drain();
	const std::vector<std::pair<std::string, std::string>> cases = {
	    {"INTEGER[]", "INTEGER[]"},
	    {"STRUCT(a INTEGER, b VARCHAR)", "STRUCT(a INTEGER, b VARCHAR)"},
	    {"MAP(VARCHAR, INTEGER)", "MAP(VARCHAR, INTEGER)"},
	    {"UNION(num INTEGER, str VARCHAR)", "UNION(num INTEGER, str VARCHAR)"},
	    {"INTEGER[3]", "INTEGER[3]"},
	    {"DECIMAL(10, 2)", "DECIMAL(10,2)"},
	    {"mood", "ENUM('sad', 'ok')"},
	};
	for (auto &test : cases) {
		last_target_type.clear();
		conn.Execute("SELECT CAST(CAST('x' AS WILD_TAG) AS " + test.first + ")").Drain();
		REQUIRE(last_target_type == test.second);
	}
}

TEST_CASE("Stable C++API: wildcard cast types: a concrete cast wins, other ids are refused", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	auto tag = RegisterTag(conn);

	auto wildcard = CastFunction::Create(factory);
	wildcard.SetSourceKind(CastWildcardKind::LIST).SetTargetType(tag).SetExecCallback(SourceTypeToText);
	conn.Register(wildcard);
	auto exact = CastFunction::Create(factory);
	exact.SetSourceType(factory.ParseType("INTEGER[]")).SetTargetType(tag).SetExecCallback(ExactListCast);
	conn.Register(exact);
	REQUIRE(CollectCastStrings(conn.Execute("SELECT CAST(CAST([1] AS WILD_TAG) AS VARCHAR)")) ==
	        std::vector<std::string> {"exact"});
	REQUIRE(CollectCastStrings(conn.Execute("SELECT CAST(CAST(['a'] AS WILD_TAG) AS VARCHAR)")) ==
	        std::vector<std::string> {"VARCHAR[]"});

	// An unknown kind is refused.
	auto function = CastFunction::Create(factory);
	auto bogus = static_cast<CastWildcardKind>(99);
	REQUIRE_THROWS_MATCHES(function.SetSourceKind(bogus), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	REQUIRE_THROWS_MATCHES(function.SetTargetKind(bogus), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));

	// ANY is not a concrete source type, and a wildcard does not make it one.
	auto any_source = CastFunction::Create(factory);
	any_source.SetSourceType(factory.CreateType(LogicalTypeId::ANY)).SetTargetType(tag).SetExecCallback(NoopCast);
	REQUIRE_THROWS_MATCHES(conn.Register(any_source), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));

	// Setting a concrete type replaces the wildcard, so the function is complete again.
	auto replaced = CastFunction::Create(factory);
	replaced.SetSourceKind(CastWildcardKind::LIST).SetSourceType(factory.ParseType("INTEGER"));
	replaced.SetTargetType(tag).SetExecCallback(NoopCast);
	conn.Register(replaced);
}

// Casts run in expressions evaluated without a client context (index keys, generated columns) too.
TEST_CASE("Stable C++API: cast function runs in an index key and a generated column", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	auto temperature = RegisterTemperature(conn);
	RegisterTemperatureCasts(conn, temperature, -1);

	conn.Execute("CREATE TABLE temps (t VARCHAR, g INTEGER GENERATED ALWAYS AS "
	             "(CAST(CAST(t AS TEMPERATURE_CELSIUS) AS INTEGER)) VIRTUAL)")
	    .Drain();
	conn.Execute("CREATE UNIQUE INDEX temps_ix ON temps ((CAST(CAST(t AS TEMPERATURE_CELSIUS) AS INTEGER)))").Drain();
	conn.Execute("INSERT INTO temps VALUES ('1 degrees celsius'), ('2 degrees celsius')").Drain();
	REQUIRE(CollectMaybeInts(conn.Execute("SELECT g FROM temps ORDER BY g")) ==
	        std::vector<std::optional<int32_t>> {1, 2});
	REQUIRE_THROWS(conn.Execute("INSERT INTO temps VALUES ('1 degrees celsius')").Drain());
}
