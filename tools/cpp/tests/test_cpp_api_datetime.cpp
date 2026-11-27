#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <string>

// ---------------------------------------------------------------------------
// Stable C++ API tests: the engine's date and time text formats -- strptime and
// strftime formats, and the text of the TIMESTAMPTZ and TIMETZ casts.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

} // namespace

TEST_CASE("Stable C++API: strptime formats parse parts", "[cpp_api]") {
	auto format = StrpTimeFormat::Create("%Y-%m-%d %H:%M:%S.%n %Z");
	REQUIRE(format.HasTimeZoneName());
	REQUIRE(format.HasNanoseconds());
	REQUIRE(!format.HasUtcOffset());

	StrpTimeResult result;
	REQUIRE(format.Parse("2026-11-27 10:30:15.123456789 Europe/Amsterdam", result));
	REQUIRE(!result.IsSpecial());
	auto &parts = result.GetParts();
	REQUIRE(parts.year == 2026);
	REQUIRE(parts.month == 11);
	REQUIRE(parts.day == 27);
	REQUIRE(parts.hour == 10);
	REQUIRE(parts.minute == 30);
	REQUIRE(parts.second == 15);
	REQUIRE(parts.fraction == 123456789);
	REQUIRE(result.GetTimeZoneName() == "Europe/Amsterdam");

	// The offset of %z, in seconds
	auto offset_format = StrpTimeFormat::Create("%H:%M %z");
	REQUIRE(offset_format.HasUtcOffset());
	REQUIRE(offset_format.Parse("10:30 +0130", result));
	REQUIRE(result.GetParts().utc_offset == 90 * 60);
	REQUIRE(result.GetTimeZoneName().empty());

	// A text that does not match is not an error, and explains itself like strptime
	REQUIRE(!format.Parse("not a date", result));
	auto error = result.GetError("not a date", "%Y-%m-%d %H:%M:%S.%n %Z");
	REQUIRE(error.find("Could not parse string \"not a date\" according to format specifier") != std::string::npos);

	// Special dates
	auto date_format = StrpTimeFormat::Create("%Y-%m-%d");
	REQUIRE(date_format.Parse("infinity", result));
	REQUIRE(result.IsSpecial());
	REQUIRE(result.GetSpecialDate().days == std::numeric_limits<int32_t>::max());
}

TEST_CASE("Stable C++API: invalid time formats", "[cpp_api]") {
	REQUIRE_THROWS_MATCHES(StrpTimeFormat::Create("%Q"), InvalidInputException,
	                       Catch::Predicate<InvalidInputException>([](const InvalidInputException &ex) {
		                       return std::string(ex.what()).find("Failed to parse format specifier %Q") !=
		                              std::string::npos;
	                       }));
	REQUIRE_THROWS_AS(StrfTimeFormat::Create("%Q"), InvalidInputException);
}

TEST_CASE("Stable C++API: strftime formats write parts", "[cpp_api]") {
	TimeParts parts;
	parts.year = 2026;
	parts.month = 11;
	parts.day = 27;
	parts.hour = 10;
	parts.minute = 30;
	parts.second = 15;
	parts.fraction = 123456;
	parts.utc_offset = 3600;

	std::string out;
	StrfTimeFormat::Create("%A, %-d %B %Y %H:%M:%S.%f %z %Z").Write(parts, false, "Europe/Amsterdam", out);
	REQUIRE(out == "Friday, 27 November 2026 10:30:15.123456 +01 Europe/Amsterdam");

	// nanoseconds
	parts.fraction = 123456789;
	StrfTimeFormat::Create("%n").Write(parts, true, "", out);
	REQUIRE(out == "123456789");

	// A text longer than the buffer the first attempt uses
	std::string long_format(200, 'x');
	StrfTimeFormat::Create(long_format + "%Y").Write(parts, true, "", out);
	REQUIRE(out == long_format + "2026");
}

TEST_CASE("Stable C++API: the text of the time zone casts", "[cpp_api]") {
	TimestampText parsed;
	REQUIRE(ParseTimestampTzText("2026-11-27 10:30:00+01", parsed) == TimestampTextResult::SUCCESS);
	REQUIRE(parsed.has_offset);
	// 2026-11-27 09:30:00 UTC
	REQUIRE(parsed.micros == 1795771800000000LL);
	REQUIRE(parsed.tz_name.empty());

	REQUIRE(ParseTimestampTzText("2026-11-27 10:30:00.000000123 Europe/Amsterdam", parsed, true) ==
	        TimestampTextResult::SUCCESS);
	REQUIRE(!parsed.has_offset);
	// The local time, to place in the zone
	REQUIRE(parsed.micros == 1795775400000000LL);
	REQUIRE(parsed.nanos == 123);
	REQUIRE(parsed.tz_name == "Europe/Amsterdam");

	// without nanoseconds, the fraction rounds half to even like the cast
	REQUIRE(ParseTimestampTzText("2026-11-27 10:30:00.0000005+00", parsed) == TimestampTextResult::SUCCESS);
	REQUIRE(parsed.micros == 1795775400000000LL);
	REQUIRE(parsed.nanos == 0);
	REQUIRE(ParseTimestampTzText("2026-11-27 10:30:00.0000015+00", parsed) == TimestampTextResult::SUCCESS);
	REQUIRE(parsed.micros == 1795775400000002LL);

	REQUIRE(ParseTimestampTzText("infinity", parsed) == TimestampTextResult::SUCCESS);
	REQUIRE(parsed.micros == std::numeric_limits<int64_t>::max());

	REQUIRE(ParseTimestampTzText("not a timestamp", parsed) == TimestampTextResult::ERROR_FORMAT);
	REQUIRE(ParseTimestampTzText("300000-01-01 00:00:00", parsed) == TimestampTextResult::ERROR_RANGE);

	int64_t micros;
	int32_t offset;
	bool has_offset;
	REQUIRE(ParseTimeTzText("10:30:00+01", micros, offset, has_offset));
	REQUIRE(micros == (10 * 60 + 30) * 60 * 1000000LL);
	REQUIRE(has_offset);
	REQUIRE(offset == 3600);
	REQUIRE(ParseTimeTzText("10:30:00", micros, offset, has_offset));
	REQUIRE(!has_offset);
	REQUIRE(!ParseTimeTzText("not a time", micros, offset, has_offset));
}
