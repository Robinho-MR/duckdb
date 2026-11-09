//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_strftime.hpp
//
// strftime formatting of dates and timestamps, for the date/timestamp format options of COPY ... TO ... (FORMAT JSON).
// The stable C++ API has no access to the engine's strftime, so the formatter is local. The specifiers and the error
// messages follow the engine's StrfTimeFormat.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "json_common.hpp"

namespace duckdb {
namespace json {

class JSONTimeFormat {
public:
	//! Parses a strftime format string. Throws an InvalidInputException on an invalid format ("Failed to parse ...").
	//! `name` is only used in the error message ("date", "timestamp")
	static JSONTimeFormat Parse(const string &format, const string &name);
	//! Like Parse, but returns the error message instead of throwing it (empty on success)
	static string TryParse(const string &format, JSONTimeFormat &result);

	//! Formats a DATE (days since 1970-01-01)
	string FormatDate(int32_t days) const;
	//! Formats a timestamp given as microseconds since the epoch
	string FormatTimestampMicros(int64_t micros) const;
	//! Formats a timestamp given as nanoseconds since the epoch
	string FormatTimestampNanos(int64_t nanos) const;

	const string &GetFormatString() const {
		return format_string;
	}

private:
	enum class Specifier : uint8_t {
		ABBREVIATED_WEEKDAY_NAME,
		FULL_WEEKDAY_NAME,
		WEEKDAY_DECIMAL,
		WEEKDAY_ISO,
		DAY_OF_MONTH_PADDED,
		DAY_OF_MONTH,
		ABBREVIATED_MONTH_NAME,
		FULL_MONTH_NAME,
		MONTH_DECIMAL_PADDED,
		MONTH_DECIMAL,
		YEAR_WITHOUT_CENTURY_PADDED,
		YEAR_WITHOUT_CENTURY,
		YEAR_DECIMAL,
		YEAR_ISO,
		HOUR_24_PADDED,
		HOUR_24_DECIMAL,
		HOUR_12_PADDED,
		HOUR_12_DECIMAL,
		AM_PM,
		MINUTE_PADDED,
		MINUTE_DECIMAL,
		SECOND_PADDED,
		SECOND_DECIMAL,
		NANOSECOND_PADDED,
		MICROSECOND_PADDED,
		MILLISECOND_PADDED,
		UTC_OFFSET,
		TZ_NAME,
		DAY_OF_YEAR_PADDED,
		DAY_OF_YEAR_DECIMAL,
		WEEK_NUMBER_PADDED_SUN_FIRST,
		WEEK_NUMBER_PADDED_MON_FIRST,
		WEEK_NUMBER_ISO
	};

	void AddSpecifier(string literal, Specifier specifier);
	string Format(int32_t days, int64_t nanos_of_day) const;

private:
	string format_string;
	//! literals.size() == specifiers.size() + 1: the literal before every specifier, and the one after the last
	vector<string> literals;
	vector<Specifier> specifiers;
};

} // namespace json
} // namespace duckdb
