//===----------------------------------------------------------------------===//
//                         DuckDB
//
// icu_bridge_datepart.hpp
//
// Date part extraction of the calendar engine for the stable-API surface. Only uses the standard library.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "icu_bridge.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace icu_bridge {

//! The parts date_part can extract: the BIGINT parts first, then the DOUBLE parts
enum class DatePart : uint8_t {
	YEAR,
	MONTH,
	DAY,
	DECADE,
	CENTURY,
	MILLENNIUM,
	MICROSECONDS,
	MILLISECONDS,
	SECOND,
	MINUTE,
	HOUR,
	DOW,
	ISODOW,
	WEEK,
	ISOYEAR,
	QUARTER,
	DOY,
	YEARWEEK,
	ERA,
	TIMEZONE,
	TIMEZONE_HOUR,
	TIMEZONE_MINUTE,
	EPOCH,
	JULIAN_DAY
};

inline bool IsDoublePart(DatePart part) {
	return part == DatePart::EPOCH || part == DatePart::JULIAN_DAY;
}

//! Parses a part specifier such as "year", "dow" or "epoch". False when it is not recognized.
bool TryParseDatePart(const std::string &name, DatePart &part);

//! Extraction of an instant (microseconds since the epoch) in the calendar. They return false when the calendar cannot
//! represent the instant. The calendar must not be shared with other users: the week parts change its week rules.
bool ExtractBigintPart(Calendar &calendar, DatePart part, int64_t instant, int64_t &result);
bool ExtractDoublePart(Calendar &calendar, DatePart part, int64_t instant, double &result);
//! The last day of the month of the instant, as days since the epoch
bool ExtractLastDay(Calendar &calendar, int64_t instant, int32_t &days);
//! The English name of the month or the day of the week of the instant
bool ExtractMonthName(Calendar &calendar, int64_t instant, std::string_view &name);
bool ExtractDayName(Calendar &calendar, int64_t instant, std::string_view &name);

} // namespace icu_bridge
