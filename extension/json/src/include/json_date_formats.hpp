//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_date_formats.hpp
//
// strptime-style parsing of dates and timestamps for the dateformat / timestampformat options of read_json and the
// date/timestamp format detection of the schema inference. The stable C++ API has no strptime, so the parser is local;
// its specifiers and error messages follow the engine's StrpTimeFormat.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "json_schema_inference.hpp"
#include "json_transform.hpp"

#include <map>

namespace duckdb {
namespace json {

//! A parsed strptime format string
class JSONStrpTimeFormat {
public:
	//! Parses a format string. Returns the error message, empty on success
	static string ParseFormatSpecifier(const string &format_string, JSONStrpTimeFormat &format);

	//! Parses a date into the number of days since 1970-01-01. Returns false (with `error` set) on mismatch
	bool TryParseDate(string_view input, int32_t &days, string &error) const;
	//! Parses a timestamp into the number of microseconds since 1970-01-01
	bool TryParseTimestamp(string_view input, int64_t &micros, string &error) const;

	const string &GetFormatSpecifier() const {
		return format_specifier;
	}

public:
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

private:
	struct ParseResult;
	bool Parse(string_view input, ParseResult &result) const;
	void AddSpecifier(string literal, Specifier specifier);
	string FormatError(string_view input, const ParseResult &result) const;

private:
	string format_specifier;
	//! literals.size() == specifiers.size() + 1
	vector<string> literals;
	vector<Specifier> specifiers;
	vector<int> numeric_width;
};

//! The date/timestamp formats of a read_json scan: the user-provided ones, or the candidates tried by the detection.
//! Read-only once the scan is bound.
class JSONDateFormatMap : public JSONDateFormats, public JSONInferenceDateFormats {
public:
	//! Adds a format for DATE or TIMESTAMP. Throws an InvalidInputException on a malformed format string
	void AddFormat(LogicalTypeId type, const string &format_string);
	//! Adds the formats the detection tries when the user did not specify one
	void AddDetectionFormats();

	//! JSONDateFormats and JSONInferenceDateFormats
	bool HasFormats(LogicalTypeId type) const override;
	idx_t FormatCount(LogicalTypeId type) const override;
	bool TryParse(LogicalTypeId type, idx_t format_idx, string_view input, int64_t &result,
	              string &error) const override;
	bool MatchesAnyFormat(LogicalTypeId type, const Vector &strings, idx_t count) override;

private:
	std::map<uint32_t, vector<JSONStrpTimeFormat>> formats;
};

} // namespace json
} // namespace duckdb
