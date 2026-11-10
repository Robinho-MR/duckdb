//===----------------------------------------------------------------------===//
//                         DuckDB
//
// icu_bridge_arithmetic.hpp
//
// Interval arithmetic, timestamp subtraction, age and date_sub/date_diff on a bridge calendar. Std-only.
// Instants are microseconds since the epoch (nanoseconds for the _ns variants). The calendars passed in are mutated
// (the time is set), so they must not be shared with other functions.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "icu_bridge.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace icu_bridge {

//! The kind of an error raised by the arithmetic kernels
enum class ArithmeticErrorKind : uint8_t { CONVERSION, INVALID_INPUT, OUT_OF_RANGE, NOT_IMPLEMENTED, INTERNAL };

struct ArithmeticError : public std::runtime_error {
	ArithmeticError(ArithmeticErrorKind kind_p, const std::string &message)
	    : std::runtime_error(message), kind(kind_p) {
	}
	ArithmeticErrorKind kind;
};

struct IntervalValue {
	int32_t months = 0;
	int32_t days = 0;
	int64_t micros = 0;
};

//! A date part specifier, resolved once and reused for many rows
struct DiffPart {
	uint8_t id = 0;
};
//! Throws ArithmeticError for unknown specifiers
DiffPart ParseDiffPart(const std::string &specifier);

//! Whether the calendar has fewer than 12 months, so that intervals work on it
bool SupportsIntervals(Calendar &calendar);
//! Whether the name is the gregorian calendar (or empty)
bool IsGregorianCalendar(const std::string &name);

//! timestamp + interval (infinite timestamps are returned as they are)
int64_t AddInterval(Calendar &calendar, int64_t timestamp, const IntervalValue &interval);
//! timestamp - interval
int64_t SubInterval(Calendar &calendar, int64_t timestamp, const IntervalValue &interval);
//! end - start as an interval, throws for infinite timestamps
IntervalValue SubTimestamps(Calendar &calendar, int64_t end, int64_t start);
//! age(end, start)
IntervalValue Age(Calendar &calendar, bool gregorian, int64_t end, int64_t start);
//! The first instant of the day of the instant
int64_t MidnightOf(Calendar &calendar, int64_t instant);

//! date_sub(part, start, end) for finite timestamps
int64_t DateSub(Calendar &calendar, DiffPart part, int64_t start, int64_t end);
//! date_diff(part, start, end) for finite timestamps
int64_t DateDiff(Calendar &calendar, DiffPart part, int64_t start, int64_t end);
//! date_diff(part, start, end) for finite nanosecond timestamps
int64_t DateDiffNanos(Calendar &calendar, DiffPart part, int64_t start, int64_t end);

} // namespace icu_bridge
