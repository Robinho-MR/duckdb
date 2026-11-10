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

// The tests call the C function setters directly, to pass what the C++ setters cannot express.
namespace duckdb::cxx::detail {
template <>
struct HandleTraits<CastFunction> {
	using handle = duckdb_v2_cast_function_handle;
};
} // namespace duckdb::cxx::detail

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

TEST_CASE("Stable C++API: try cast reports a failed row through the error slot", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	auto temperature = RegisterTemperature(conn);
	RegisterTemperatureCasts(conn, temperature, -1);

	std::vector<LogicalType> types;
	types.push_back(factory.ParseType("VARCHAR"));
	types.push_back(std::move(temperature));
	DataChunk chunk(factory, types);
	auto source = chunk.GetVector(0);
	auto target = chunk.GetVector(1);
	// Padded past the inline string length, see the vector tests
	const std::string good = std::string(16, ' ') + "5" + SUFFIX;
	source.SetSize(3);
	source.AssignString(0, good);
	source.AssignString(1, "not-a-temp-and-long-enough");
	source.AssignString(2, good);

	// The row the cast could not convert is NULL and the cast reports that not every row converted
	std::string error;
	REQUIRE_FALSE(source.TryCast(factory, target, &error));
	REQUIRE_THAT(error, Catch::Matchers::Contains("not-a-temp"));
	auto view = target.GetView();
	REQUIRE(view.IsValid(0));
	REQUIRE_FALSE(view.IsValid(1));
	REQUIRE(view.IsValid(2));

	// A row that is NULL in the input is not a failure
	source.SetNull(1);
	error.clear();
	REQUIRE(source.TryCast(factory, target, &error));
	REQUIRE(error.empty());
	REQUIRE_FALSE(target.GetView().IsValid(1));
}

// ---------------------------------------------------------------------------
// Stability, bind callback and bind data
// ---------------------------------------------------------------------------

namespace {

constexpr const char *OFFSET_OPTION = "cast_offset";
constexpr const char *FORBID_OPTION = "cast_forbidden";

// Carried as bind data: what the bind callback saw.
struct BindRecord {
	std::string types;
	int64_t offset = 0;
};

idx_t bind_calls = 0;

void RegisterOptions(Connection &conn) {
	auto &factory = conn.GetFactory();
	auto offset = CustomOption::Create(factory);
	offset.SetName(OFFSET_OPTION)
	    .SetType(factory.ParseType("BIGINT"))
	    .SetDefaultValue(Value::Create(factory, int64_t {0}));
	conn.Register(offset);
	auto forbid = CustomOption::Create(factory);
	forbid.SetName(FORBID_OPTION).SetType(factory.ParseType("BOOLEAN")).SetDefaultValue(Value::Create(factory, false));
	conn.Register(forbid);
}

// TEMPERATURE -> VARCHAR: renders the value plus the offset option, read while executing.
void OffsetTempToText(CastFunction::ExecInput &input) {
	const auto offset = input.GetContext().GetConfig().GetOption(OFFSET_OPTION).value.Get<int64_t>();
	auto in = input.GetInput();
	auto out = input.GetOutput();
	auto view = in.GetView();
	auto validity = out.GetValidityMutable();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!view.IsValid(i)) {
			validity.SetInvalid(i);
			continue;
		}
		out.AssignString(i, std::to_string(view.Data<int32_t>()[view.SelAt(i)] + offset) + SUFFIX);
	}
}

// TEMPERATURE -> VARCHAR: renders the bind data the bind callback produced.
void BindDataTempToText(CastFunction::ExecInput &input) {
	const auto &record = input.GetBindData<BindRecord>();
	auto in = input.GetInput();
	auto out = input.GetOutput();
	auto view = in.GetView();
	auto validity = out.GetValidityMutable();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!view.IsValid(i)) {
			validity.SetInvalid(i);
			continue;
		}
		out.AssignString(i, record.types + " offset " + std::to_string(record.offset) + SUFFIX);
	}
}

void RecordingBind(CastFunction::BindInput &input) {
	bind_calls++;
	BindRecord record;
	record.types = input.GetUserData<CastTag>().value;
	record.types += input.GetSourceType().GetTypeId() == LogicalTypeId::INTEGER ? " INTEGER->" : " ?->";
	record.types += input.GetTargetType().GetTypeId() == LogicalTypeId::VARCHAR ? "VARCHAR" : "?";
	if (auto *context = input.GetContext()) {
		record.offset = context->GetConfig().GetOption(OFFSET_OPTION).value.Get<int64_t>();
	}
	input.SetBindData<BindRecord>(std::move(record));
}

void ForbiddingBind(CastFunction::BindInput &input) {
	auto *context = input.GetContext();
	if (context && context->GetConfig().GetOption(FORBID_OPTION).value.Get<bool>()) {
		throw InvalidInputException("temperature casts are forbidden right now");
	}
}

void RegisterOffsetCast(Connection &conn, const LogicalType &temperature, FunctionStability stability) {
	auto cast = CastFunction::Create(conn.GetFactory());
	cast.SetSourceType(temperature).SetTargetType(conn.GetFactory().ParseType("VARCHAR"));
	cast.SetExecCallback(OffsetTempToText).SetStability(stability);
	conn.Register(cast);
}

} // namespace

TEST_CASE("Stable C++API: cast function stability decides whether a prepared cast follows a setting", "[cpp_api]") {
	const auto query = "PREPARE p AS SELECT upper(CAST(CAST(5 AS TEMPERATURE_CELSIUS) AS VARCHAR))";
	{
		// A setting-dependent cast is not folded while preparing
		auto db = Instance(":memory:");
		auto conn = db.Connect();
		RegisterOptions(conn);
		auto temperature = RegisterTemperature(conn);
		RegisterOffsetCast(conn, temperature, FunctionStability::CONSISTENT_WITHIN_QUERY);
		conn.Execute(query).Drain();
		REQUIRE(CollectCastStrings(conn.Execute("EXECUTE p")) == std::vector<std::string> {"5 DEGREES CELSIUS"});
		conn.Execute("SET cast_offset = 100").Drain();
		REQUIRE(CollectCastStrings(conn.Execute("EXECUTE p")) == std::vector<std::string> {"105 DEGREES CELSIUS"});
	}
	{
		// A consistent cast is folded when the statement is prepared
		auto db = Instance(":memory:");
		auto conn = db.Connect();
		RegisterOptions(conn);
		auto temperature = RegisterTemperature(conn);
		RegisterOffsetCast(conn, temperature, FunctionStability::CONSISTENT);
		conn.Execute(query).Drain();
		conn.Execute("SET cast_offset = 100").Drain();
		REQUIRE(CollectCastStrings(conn.Execute("EXECUTE p")) == std::vector<std::string> {"5 DEGREES CELSIUS"});
	}
}

TEST_CASE("Stable C++API: cast function stability decides whether an index expression is accepted", "[cpp_api]") {
	const auto create_index = "CREATE INDEX i ON t ((CAST(CAST(x AS TEMPERATURE_CELSIUS) AS VARCHAR)))";
	for (auto stability : {FunctionStability::CONSISTENT_WITHIN_QUERY, FunctionStability::VOLATILE}) {
		auto db = Instance(":memory:");
		auto conn = db.Connect();
		RegisterOptions(conn);
		auto temperature = RegisterTemperature(conn);
		RegisterOffsetCast(conn, temperature, stability);
		conn.Execute("CREATE TABLE t (x INTEGER)").Drain();
		REQUIRE_THROWS_WITH(conn.Execute(create_index).Drain(),
		                    Catch::Matchers::Contains("Index keys cannot contain expressions with side effects"));
	}
}

TEST_CASE("Stable C++API: cast function bind callback sets bind data for the exec callback", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterOptions(conn);
	auto temperature = RegisterTemperature(conn);

	auto cast = CastFunction::Create(conn.GetFactory());
	cast.SetSourceType(temperature).SetTargetType(conn.GetFactory().ParseType("VARCHAR"));
	cast.SetUserData<CastTag>(CastTag {"tag"});
	cast.SetBindCallback(RecordingBind).SetExecCallback(BindDataTempToText);
	conn.Register(cast);

	bind_calls = 0;
	conn.Execute("SET cast_offset = 3").Drain();
	REQUIRE(CollectCastStrings(conn.Execute("SELECT CAST(CAST(5 AS TEMPERATURE_CELSIUS) AS VARCHAR)")) ==
	        std::vector<std::string> {"tag INTEGER->VARCHAR offset 3 degrees celsius"});
	REQUIRE(bind_calls >= 1);
	const auto after_first = bind_calls;

	// Each binding runs the callback again and sees the settings of that moment
	conn.Execute("SET cast_offset = 4").Drain();
	REQUIRE(CollectCastStrings(conn.Execute("SELECT CAST(CAST(5 AS TEMPERATURE_CELSIUS) AS VARCHAR)")) ==
	        std::vector<std::string> {"tag INTEGER->VARCHAR offset 4 degrees celsius"});
	REQUIRE(bind_calls > after_first);

	// Casting a table column works the same
	conn.Execute("CREATE TABLE t (x INTEGER)").Drain();
	conn.Execute("INSERT INTO t VALUES (1), (NULL)").Drain();
	REQUIRE(CollectCastStrings(conn.Execute("SELECT CAST(CAST(x AS TEMPERATURE_CELSIUS) AS VARCHAR) FROM t WHERE x "
	                                        "IS NOT NULL")) ==
	        std::vector<std::string> {"tag INTEGER->VARCHAR offset 4 degrees celsius"});
}

TEST_CASE("Stable C++API: cast function bind callback errors surface at bind time", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterOptions(conn);
	auto temperature = RegisterTemperature(conn);

	auto cast = CastFunction::Create(conn.GetFactory());
	cast.SetSourceType(temperature).SetTargetType(conn.GetFactory().ParseType("VARCHAR"));
	cast.SetBindCallback(ForbiddingBind).SetExecCallback(OffsetTempToText);
	conn.Register(cast);

	const std::string sql = "SELECT CAST(CAST(5 AS TEMPERATURE_CELSIUS) AS VARCHAR)";
	REQUIRE(CollectCastStrings(conn.Execute(sql)) == std::vector<std::string> {"5 degrees celsius"});

	conn.Execute("SET cast_forbidden = true").Drain();
	const auto message = Catch::Matchers::Contains("temperature casts are forbidden right now");
	REQUIRE_THROWS_WITH(conn.Execute(sql).Drain(), message);
	REQUIRE_THROWS_WITH(conn.Execute("EXPLAIN " + sql).Drain(), message);
	REQUIRE_THROWS_WITH(conn.Execute("PREPARE p AS " + sql).Drain(), message);

	// Once the option allows it again, the same statements bind
	conn.Execute("SET cast_forbidden = false").Drain();
	conn.Execute("PREPARE p AS " + sql).Drain();
	REQUIRE(CollectCastStrings(conn.Execute("EXECUTE p")) == std::vector<std::string> {"5 degrees celsius"});
}

TEST_CASE("Stable C++API: cast function property and bind data misuse", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto function = CastFunction::Create(conn.GetFactory());

	// Only the stability key is valid for a cast
	REQUIRE(duckdb_v2_cast_function_set_property(function.handle(), DUCKDB_V2_FUNCTION_PROPERTY_NULL_HANDLING,
	                                             DUCKDB_V2_FUNCTION_PROPERTY_NULL_HANDLING_SPECIAL,
	                                             nullptr) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_cast_function_set_property(function.handle(), DUCKDB_V2_FUNCTION_PROPERTY_FALLIBILITY,
	                                             DUCKDB_V2_FUNCTION_PROPERTY_FALLIBILITY_FALLIBLE,
	                                             nullptr) != DUCKDB_V2_ERROR_NONE);
	// A value that does not belong to the key
	REQUIRE(duckdb_v2_cast_function_set_property(function.handle(), DUCKDB_V2_FUNCTION_PROPERTY_STABILITY,
	                                             DUCKDB_V2_FUNCTION_PROPERTY_NULL_HANDLING_DEFAULT,
	                                             nullptr) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_cast_function_set_property(nullptr, DUCKDB_V2_FUNCTION_PROPERTY_STABILITY,
	                                             DUCKDB_V2_FUNCTION_PROPERTY_STABILITY_CONSISTENT,
	                                             nullptr) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_cast_function_set_property(function.handle(), DUCKDB_V2_FUNCTION_PROPERTY_STABILITY,
	                                             DUCKDB_V2_FUNCTION_PROPERTY_STABILITY_VOLATILE,
	                                             nullptr) == DUCKDB_V2_ERROR_NONE);

	// Null arguments of the bind accessors
	void *data = nullptr;
	REQUIRE(duckdb_v2_cast_function_bind_get_user_data(nullptr, &data, nullptr) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_cast_function_exec_get_bind_data(nullptr, &data, nullptr) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_cast_function_set_bind_callback(nullptr, nullptr, nullptr) != DUCKDB_V2_ERROR_NONE);

	// The convenience setter works and a bind callback can be removed again
	function.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
	function.SetBindCallback(ForbiddingBind).SetBindCallback(nullptr);
}
