//===----------------------------------------------------------------------===//
//                         DuckDB
//
// icu-datetime.hpp
//
// The date and time arithmetic of the engine's temporal types, on the value
// types of the stable C++ API, so that the ICU functions compute exactly what
// the engine's built-in functions compute.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "icu_common.hpp"

#include <cmath>
#include <cstdlib>

namespace duckdb_icu {

using duckdb::cxx::date_t;
using duckdb::cxx::dtime_t;
using duckdb::cxx::dtime_tz_t;
using duckdb::cxx::interval_t;
using duckdb::cxx::timestamp_ms_t;
using duckdb::cxx::timestamp_ns_t;
using duckdb::cxx::timestamp_s_t;
using duckdb::cxx::timestamp_t;
using duckdb::cxx::timestamp_tz_ns_t;
using duckdb::cxx::timestamp_tz_t;

//! The microseconds, or nanoseconds, of every timestamp type, with its precision per second
template <class T>
struct TimestampTraits;

#define DUCKDB_ICU_TIMESTAMP_TRAITS(TYPE, FIELD, PRECISION_VALUE)                                                      \
	template <>                                                                                                        \
	struct TimestampTraits<TYPE> {                                                                                     \
		static constexpr int64_t PRECISION = PRECISION_VALUE;                                                          \
		static int64_t Get(const TYPE &value) {                                                                        \
			return value.FIELD;                                                                                        \
		}                                                                                                              \
		static TYPE Make(int64_t value) {                                                                              \
			TYPE result;                                                                                               \
			result.FIELD = value;                                                                                      \
			return result;                                                                                             \
		}                                                                                                              \
	}

DUCKDB_ICU_TIMESTAMP_TRAITS(timestamp_t, micros, 1000000);
DUCKDB_ICU_TIMESTAMP_TRAITS(timestamp_tz_t, micros, 1000000);
DUCKDB_ICU_TIMESTAMP_TRAITS(timestamp_ns_t, nanos, 1000000000);
DUCKDB_ICU_TIMESTAMP_TRAITS(timestamp_tz_ns_t, nanos, 1000000000);
DUCKDB_ICU_TIMESTAMP_TRAITS(timestamp_ms_t, millis, 1000);
DUCKDB_ICU_TIMESTAMP_TRAITS(timestamp_s_t, seconds, 1);

#undef DUCKDB_ICU_TIMESTAMP_TRAITS

constexpr int64_t TIMESTAMP_INFINITY = std::numeric_limits<int64_t>::max();
constexpr int64_t TIMESTAMP_NINFINITY = -std::numeric_limits<int64_t>::max();
constexpr int32_t DATE_INFINITY = std::numeric_limits<int32_t>::max();
constexpr int32_t DATE_NINFINITY = -std::numeric_limits<int32_t>::max();

template <class T>
inline int64_t Raw(const T &value) {
	return TimestampTraits<T>::Get(value);
}

template <class T>
inline T Make(int64_t value) {
	return TimestampTraits<T>::Make(value);
}

template <class T>
inline bool IsFinite(const T &value) {
	return Raw(value) != TIMESTAMP_INFINITY && Raw(value) != TIMESTAMP_NINFINITY;
}

inline bool IsFinite(const date_t &value) {
	return value.days != DATE_INFINITY && value.days != DATE_NINFINITY;
}

//! "infinity" or "-infinity"
template <class T>
inline const char *ToInfinity(const T &value) {
	return Raw(value) == TIMESTAMP_INFINITY ? "infinity" : "-infinity";
}

//===--------------------------------------------------------------------===//
// Checked arithmetic
//===--------------------------------------------------------------------===//
inline bool TryAdd(int64_t a, int64_t b, int64_t &result) {
	if ((b > 0 && a > NumericLimits<int64_t>::Maximum() - b) || (b < 0 && a < NumericLimits<int64_t>::Minimum() - b)) {
		return false;
	}
	result = a + b;
	return true;
}

inline bool TrySubtract(int64_t a, int64_t b, int64_t &result) {
	if ((b < 0 && a > NumericLimits<int64_t>::Maximum() + b) || (b > 0 && a < NumericLimits<int64_t>::Minimum() + b)) {
		return false;
	}
	result = a - b;
	return true;
}

inline bool TryMultiply(int64_t a, int64_t b, int64_t &result) {
	constexpr auto MAX = NumericLimits<int64_t>::Maximum();
	constexpr auto MIN = NumericLimits<int64_t>::Minimum();
	if (a > 0) {
		if (b > 0 ? a > MAX / b : b < MIN / a) {
			return false;
		}
	} else if (a < 0) {
		if (b > 0 ? a < MIN / b : (b != 0 && b < MAX / a)) {
			return false;
		}
	}
	result = a * b;
	return true;
}

//! The subtraction of the engine's SubtractOperatorOverflowCheck
inline int64_t SubtractOverflowCheck(int64_t left, int64_t right) {
	int64_t result;
	if (!TrySubtract(left, right, result)) {
		throw OutOfRangeException("Overflow in subtraction of INT64 (%lld - %lld)!", static_cast<long long>(left),
		                          static_cast<long long>(right));
	}
	return result;
}

inline int32_t SubtractOverflowCheck(int32_t left, int32_t right) {
	const auto result = int64_t(left) - int64_t(right);
	if (result < NumericLimits<int32_t>::Minimum() || result > NumericLimits<int32_t>::Maximum()) {
		throw OutOfRangeException("Overflow in subtraction of INT32 (%d - %d)!", left, right);
	}
	return int32_t(result);
}

//===--------------------------------------------------------------------===//
// Interval
//===--------------------------------------------------------------------===//
struct Interval {
	static constexpr const int32_t MONTHS_PER_MILLENIUM = 12000;
	static constexpr const int32_t MONTHS_PER_CENTURY = 1200;
	static constexpr const int32_t MONTHS_PER_DECADE = 120;
	static constexpr const int32_t MONTHS_PER_YEAR = 12;
	static constexpr const int32_t MONTHS_PER_QUARTER = 3;
	static constexpr const int32_t DAYS_PER_WEEK = 7;
	static constexpr const int64_t DAYS_PER_MONTH = 30;
	static constexpr const int64_t MSECS_PER_SEC = 1000;
	static constexpr const int32_t SECS_PER_MINUTE = 60;
	static constexpr const int32_t MINS_PER_HOUR = 60;
	static constexpr const int32_t HOURS_PER_DAY = 24;
	static constexpr const int32_t SECS_PER_HOUR = SECS_PER_MINUTE * MINS_PER_HOUR;
	static constexpr const int32_t SECS_PER_DAY = SECS_PER_HOUR * HOURS_PER_DAY;
	static constexpr const int64_t MICROS_PER_MSEC = 1000;
	static constexpr const int64_t MICROS_PER_SEC = MICROS_PER_MSEC * MSECS_PER_SEC;
	static constexpr const int64_t MICROS_PER_MINUTE = MICROS_PER_SEC * SECS_PER_MINUTE;
	static constexpr const int64_t MICROS_PER_HOUR = MICROS_PER_MINUTE * MINS_PER_HOUR;
	static constexpr const int64_t MICROS_PER_DAY = MICROS_PER_HOUR * HOURS_PER_DAY;
	static constexpr const int64_t MICROS_PER_MONTH = MICROS_PER_DAY * DAYS_PER_MONTH;
	static constexpr const int64_t NANOS_PER_MICRO = 1000;
	static constexpr const int64_t NANOS_PER_MSEC = NANOS_PER_MICRO * MICROS_PER_MSEC;
	static constexpr const int64_t NANOS_PER_SEC = NANOS_PER_MSEC * MSECS_PER_SEC;
	static constexpr const int64_t NANOS_PER_DAY = NANOS_PER_SEC * SECS_PER_DAY;

	static interval_t Make(int32_t months, int32_t days, int64_t micros) {
		interval_t result;
		result.months = months;
		result.days = days;
		result.micros = micros;
		return result;
	}

	static interval_t FromMicro(int64_t delta_us) {
		return Make(0, static_cast<int32_t>(delta_us / MICROS_PER_DAY), delta_us % MICROS_PER_DAY);
	}

	static bool TryGetMicro(const interval_t &val, int64_t &micro_total) {
		int64_t micro_month, micro_day;
		micro_total = val.micros;
		if (!TryMultiply(int64_t(val.months), MICROS_PER_MONTH, micro_month)) {
			return false;
		}
		if (!TryMultiply(int64_t(val.days), MICROS_PER_DAY, micro_day)) {
			return false;
		}
		if (!TryAdd(micro_total, micro_month, micro_total)) {
			return false;
		}
		return TryAdd(micro_total, micro_day, micro_total);
	}

	static interval_t Invert(interval_t interval) {
		if (interval.days == NumericLimits<int32_t>::Minimum()) {
			throw OutOfRangeException("Interval days value out of range");
		}
		if (interval.micros == NumericLimits<int64_t>::Minimum()) {
			throw OutOfRangeException("Interval micros value out of range");
		}
		if (interval.months == NumericLimits<int32_t>::Minimum()) {
			throw OutOfRangeException("Interval months value out of range");
		}
		interval.days = -interval.days;
		interval.micros = -interval.micros;
		interval.months = -interval.months;
		return interval;
	}

	//! The number of whole units of `micros_per_unit` from `start_us` to `end_us`
	static int64_t GetDifferenceInUnits(int64_t start_us, int64_t end_us, int64_t micros_per_unit) {
		int64_t delta_us;
		if (TrySubtract(end_us, start_us, delta_us)) {
			return delta_us / micros_per_unit;
		}
		// the difference overflows: split both into whole units and the rest, whose differences fit
		auto units = end_us / micros_per_unit - start_us / micros_per_unit;
		const auto rest = end_us % micros_per_unit - start_us % micros_per_unit;
		units += rest / micros_per_unit;
		const auto remainder = rest % micros_per_unit;
		// the quotient truncates towards zero, as the division of the difference would
		if (units > 0 && remainder < 0) {
			units--;
		} else if (units < 0 && remainder > 0) {
			units++;
		}
		return units;
	}

	//! The time of day `left` moved by the time part of `right`, carrying whole days into `date`
	static dtime_t Add(dtime_t left, interval_t right, date_t &date) {
		int64_t diff = right.micros - ((right.micros / MICROS_PER_DAY) * MICROS_PER_DAY);
		left.micros += diff;
		if (left.micros >= MICROS_PER_DAY) {
			left.micros -= MICROS_PER_DAY;
			date.days++;
		} else if (left.micros < 0) {
			left.micros += MICROS_PER_DAY;
			date.days--;
		}
		return left;
	}

	//! The text of an interval, as the engine writes it
	static string ToString(const interval_t &interval);
};

//===--------------------------------------------------------------------===//
// Date
//===--------------------------------------------------------------------===//
struct Date {
	static constexpr const int32_t DATE_MIN_YEAR = -5877641;
	static constexpr const int32_t DATE_MIN_MONTH = 6;
	static constexpr const int32_t DATE_MIN_DAY = 25;
	static constexpr const int32_t DATE_MAX_YEAR = 5881580;
	static constexpr const int32_t DATE_MAX_MONTH = 7;
	static constexpr const int32_t DATE_MAX_DAY = 10;

	static const char *const MONTH_NAMES[12];
	static const char *const DAY_NAMES[7];
	static const int32_t NORMAL_DAYS[13];
	static const int32_t LEAP_DAYS[13];

	static bool IsLeapYear(int32_t year) {
		return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
	}

	static bool IsValid(int32_t year, int32_t month, int32_t day) {
		if (month < 1 || month > 12) {
			return false;
		}
		if (day < 1) {
			return false;
		}
		if (year <= DATE_MIN_YEAR) {
			if (year < DATE_MIN_YEAR) {
				return false;
			} else if (year == DATE_MIN_YEAR) {
				if (month < DATE_MIN_MONTH || (month == DATE_MIN_MONTH && day < DATE_MIN_DAY)) {
					return false;
				}
			}
		}
		if (year >= DATE_MAX_YEAR) {
			if (year > DATE_MAX_YEAR) {
				return false;
			} else if (year == DATE_MAX_YEAR) {
				if (month > DATE_MAX_MONTH || (month == DATE_MAX_MONTH && day > DATE_MAX_DAY)) {
					return false;
				}
			}
		}
		return IsLeapYear(year) ? day <= LEAP_DAYS[month] : day <= NORMAL_DAYS[month];
	}

	//! Days since 1970-01-01 of a proleptic Gregorian date, which must be valid
	static int32_t DaysFromCivil(int32_t year, int32_t month, int32_t day) {
		const int64_t y = int64_t(year) - (month <= 2);
		const int64_t era = (y >= 0 ? y : y - 399) / 400;
		const int64_t yoe = y - era * 400;
		const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
		const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
		return static_cast<int32_t>(era * 146097 + doe - 719468);
	}

	static bool TryFromDate(int32_t year, int32_t month, int32_t day, date_t &result) {
		if (!IsValid(year, month, day)) {
			return false;
		}
		result.days = DaysFromCivil(year, month, day);
		return true;
	}

	static date_t FromDate(int32_t year, int32_t month, int32_t day) {
		date_t result;
		if (!TryFromDate(year, month, day, result)) {
			throw ConversionException("Date out of range: %d-%d-%d", year, month, day);
		}
		return result;
	}

	static void Convert(date_t d, int32_t &year, int32_t &month, int32_t &day) {
		const int64_t z = int64_t(d.days) + 719468;
		const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
		const int64_t doe = z - era * 146097;
		const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
		const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
		const int64_t mp = (5 * doy + 2) / 153;
		day = static_cast<int32_t>(doy - (153 * mp + 2) / 5 + 1);
		month = static_cast<int32_t>(mp < 10 ? mp + 3 : mp - 9);
		year = static_cast<int32_t>(yoe + era * 400 + (month <= 2));
	}

	static date_t EpochToDate(int64_t epoch) {
		date_t result;
		result.days = static_cast<int32_t>(epoch / Interval::SECS_PER_DAY);
		return result;
	}

	//! The text of a date, as the engine writes it
	static string ToString(date_t date);
};

//===--------------------------------------------------------------------===//
// Time
//===--------------------------------------------------------------------===//
struct Time {
	static dtime_t FromTime(int32_t hour, int32_t minute, int32_t second, int32_t microseconds) {
		int64_t result = hour;
		result = result * Interval::MINS_PER_HOUR + minute;
		result = result * Interval::SECS_PER_MINUTE + second;
		result = result * Interval::MICROS_PER_SEC + microseconds;
		dtime_t time;
		time.micros = result;
		return time;
	}

	static bool IsValidTime(int32_t hour, int32_t minute, int32_t second, int32_t microseconds) {
		if (hour < 0 || hour >= 24) {
			return (hour == 24) && (minute == 0) && (second == 0) && (microseconds == 0);
		}
		if (minute < 0 || minute >= 60) {
			return false;
		}
		if (second < 0 || second > 60) {
			return false;
		}
		if (microseconds < 0 || microseconds > 1000000) {
			return false;
		}
		const int64_t total_micros =
		    ((int64_t(hour) * 60 + minute) * 60 + second) * Interval::MICROS_PER_SEC + microseconds;
		return total_micros <= Interval::MICROS_PER_DAY;
	}

	static void Convert(dtime_t dtime, int32_t &hour, int32_t &min, int32_t &sec, int32_t &micros) {
		int64_t time = dtime.micros;
		hour = int32_t(time / Interval::MICROS_PER_HOUR);
		time -= int64_t(hour) * Interval::MICROS_PER_HOUR;
		min = int32_t(time / Interval::MICROS_PER_MINUTE);
		time -= int64_t(min) * Interval::MICROS_PER_MINUTE;
		sec = int32_t(time / Interval::MICROS_PER_SEC);
		time -= int64_t(sec) * Interval::MICROS_PER_SEC;
		micros = int32_t(time);
	}

	//! The time of a TIME WITH TIME ZONE at UTC
	static dtime_t NormalizeTimeTZ(dtime_tz_t timetz) {
		date_t date;
		dtime_t time;
		time.micros = timetz.GetMicros();
		return Interval::Add(time, Interval::Make(0, 0, -int64_t(timetz.GetOffset()) * Interval::MICROS_PER_SEC), date);
	}

	//! The text of a UTC offset, e.g. "+05:30"
	static string ToUTCOffset(int hour_offset, int minute_offset, int second_offset);
};

//===--------------------------------------------------------------------===//
// Timestamp
//===--------------------------------------------------------------------===//
struct Timestamp {
	//! Splits `value` into whole units of `unit` (towards -infinity) and the rest
	static void SplitTime(int64_t value, int64_t unit, int64_t &whole, int64_t &part) {
		whole = value / unit;
		part = value % unit;
		if (part < 0) {
			--whole;
			part += unit;
		}
	}

	static bool TryCombineUnits(int64_t whole, int64_t part, int64_t unit, int64_t &result) {
		int64_t whole_units;
		if (TryMultiply(whole, unit, whole_units)) {
			return TryAdd(whole_units, part, result);
		}
		if (whole >= 0) {
			return false;
		}
		// start from the next whole unit, which still fits when the sum does
		if (!TryMultiply(whole + 1, unit, whole_units)) {
			return false;
		}
		return TryAdd(whole_units, part - unit, result);
	}

	static date_t GetDate(timestamp_t timestamp) {
		date_t result;
		if (timestamp.micros == TIMESTAMP_INFINITY) {
			result.days = DATE_INFINITY;
		} else if (timestamp.micros == TIMESTAMP_NINFINITY) {
			result.days = DATE_NINFINITY;
		} else {
			result.days = static_cast<int32_t>((timestamp.micros + (timestamp.micros < 0)) / Interval::MICROS_PER_DAY -
			                                   (timestamp.micros < 0));
		}
		return result;
	}

	static dtime_t GetTime(timestamp_t timestamp) {
		if (!IsFinite(timestamp)) {
			throw ConversionException("Can't get TIME of infinite TIMESTAMP");
		}
		dtime_t result;
		result.micros = timestamp.micros % Interval::MICROS_PER_DAY;
		if (result.micros < 0) {
			result.micros += Interval::MICROS_PER_DAY;
		}
		return result;
	}

	static void Convert(timestamp_t timestamp, date_t &out_date, dtime_t &out_time) {
		out_date = GetDate(timestamp);
		out_time = GetTime(timestamp);
	}

	static bool TryFromDatetime(date_t date, dtime_t time, timestamp_t &result) {
		if (!TryCombineUnits(date.days, time.micros, Interval::MICROS_PER_DAY, result.micros)) {
			return false;
		}
		return IsFinite(result);
	}

	static timestamp_t FromDatetime(date_t date, dtime_t time) {
		timestamp_t result;
		if (!TryFromDatetime(date, time, result)) {
			throw ConversionException("Date and time not in timestamp range");
		}
		return result;
	}

	static bool TryFromTimestampNanos(timestamp_t input, int32_t nanos, timestamp_ns_t &result) {
		if (!IsFinite(input)) {
			result.nanos = input.micros;
			return true;
		}
		if (!TryCombineUnits(input.micros, nanos, Interval::NANOS_PER_MICRO, result.nanos)) {
			return false;
		}
		return IsFinite(result);
	}

	//! The text of a timestamp, as the engine writes it
	static string ToString(timestamp_t timestamp);
};

//! The parts of a timestamp in some calendar
struct TimestampComponents {
	int32_t year = 0;
	int32_t month = 0;
	int32_t day = 0;
	int32_t hour = 0;
	int32_t minute = 0;
	int32_t second = 0;
	int32_t microsecond = 0;
	int32_t nanosecond = 0;
};

//! The age between two timestamps, as the engine computes it from their parts
interval_t GetAge(const TimestampComponents &ts1, const TimestampComponents &ts2, bool is_negative);

//===--------------------------------------------------------------------===//
// Casts between the timestamp precisions
//===--------------------------------------------------------------------===//
//! The engine's TryCast between timestamp precisions: scales up with an overflow check, and rounds away from the
//! epoch when scaling down
template <class SRC, class DST>
bool TryCastTimestamp(const SRC &input, DST &result) {
	constexpr auto SRC_PRECISION = TimestampTraits<SRC>::PRECISION;
	constexpr auto DST_PRECISION = TimestampTraits<DST>::PRECISION;
	const auto value = Raw(input);
	if (SRC_PRECISION == DST_PRECISION || !IsFinite(input)) {
		result = Make<DST>(value);
		return true;
	}
	if (SRC_PRECISION < DST_PRECISION) {
		int64_t scaled;
		if (!TryMultiply(value, DST_PRECISION / SRC_PRECISION, scaled)) {
			return false;
		}
		result = Make<DST>(scaled);
		return true;
	}
	const int64_t power_of_ten = SRC_PRECISION / DST_PRECISION;
	const int64_t scaling = power_of_ten / 2;
	int64_t scaled = value / scaling;
	if (scaled < 0) {
		--scaled;
	} else {
		++scaled;
	}
	scaled /= 2;
	if (DST_PRECISION < Interval::MICROS_PER_SEC) {
		int64_t micros;
		if (!TryMultiply(scaled, Interval::MICROS_PER_SEC / DST_PRECISION, micros) ||
		    !IsFinite(Make<timestamp_t>(micros))) {
			return false;
		}
	}
	result = Make<DST>(scaled);
	return true;
}

//! The engine's TryCast from a DATE to a timestamp: midnight of the date
template <class DST>
bool TryCastDate(date_t input, DST &result) {
	timestamp_t micros;
	if (input.days == DATE_INFINITY) {
		micros.micros = TIMESTAMP_INFINITY;
	} else if (input.days == DATE_NINFINITY) {
		micros.micros = TIMESTAMP_NINFINITY;
	} else if (!Timestamp::TryFromDatetime(input, dtime_t {}, micros)) {
		return false;
	}
	return TryCastTimestamp(micros, result);
}

//! Truncates a timestamp to a coarser precision, towards -infinity rather than the epoch
template <typename SRC, typename DST>
inline DST TruncCast(const SRC &src) {
	constexpr auto SRC_PRECISION = TimestampTraits<SRC>::PRECISION;
	constexpr auto DST_PRECISION = TimestampTraits<DST>::PRECISION;
	if (SRC_PRECISION == DST_PRECISION || !IsFinite(src)) {
		return Make<DST>(Raw(src));
	}
	int64_t whole;
	int64_t frac;
	Timestamp::SplitTime(Raw(src), SRC_PRECISION / DST_PRECISION, whole, frac);
	return Make<DST>(whole);
}

//===--------------------------------------------------------------------===//
// Text of the date and time types
//===--------------------------------------------------------------------===//
struct DateToStringCast {
	//! The length of a date with `year`, which is made positive for the " (BC)" suffix
	static idx_t YearLength(int32_t &year, idx_t &year_length, bool &add_bc);
	static void Format(char *data, int32_t year, int32_t month, int32_t day, idx_t year_length, bool add_bc);
};

struct TimeToStringCast {
	//! Formats the fraction of a second into `micro_buffer`, returning the number of trailing zeros
	static int32_t FormatMicros(int32_t microseconds, char micro_buffer[], int32_t nanos = 0);
	static idx_t MicrosLength(int32_t micros, char micro_buffer[], int32_t nanos = 0);
	static void FormatTwoDigits(char *ptr, int32_t value);
	static void Format(char *data, idx_t length, int32_t hour, int32_t minute, int32_t second, char micro_buffer[]);
};

//===--------------------------------------------------------------------===//
// Date parts
//===--------------------------------------------------------------------===//
enum class DatePartSpecifier : uint8_t {
	//	BIGINT values
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
	//	DOUBLE values
	EPOCH,
	JULIAN_DAY,
	//	Invalid
	INVALID,
};

inline bool IsBigintDatepart(DatePartSpecifier part_code) {
	return uint8_t(part_code) < uint8_t(DatePartSpecifier::EPOCH);
}

//! The part a name or one of its abbreviations names, case-insensitively
bool TryGetDatePartSpecifier(std::string_view specifier, DatePartSpecifier &result);
//! The part a name names, throwing the engine's error otherwise
DatePartSpecifier GetDatePartSpecifier(std::string_view specifier);
//! The name of the function of a single part, e.g. "dayofweek"
string DatePartFunctionName(DatePartSpecifier part);

} // namespace duckdb_icu
