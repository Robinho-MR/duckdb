//===----------------------------------------------------------------------===//
//                         DuckDB
//
// icu_bridge_trunc_bucket.hpp
//
// The per-value kernels of date_trunc, time_bucket, make_timestamptz and the date/time casts of TIMESTAMPTZ. Std-only,
// the implementation sits on the vendored calendar engine.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "icu_bridge.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace icu_bridge {

//! The kind of an error a kernel raised
enum class ErrorKind : uint8_t { CONVERSION, OUT_OF_RANGE, NOT_IMPLEMENTED, INVALID_INPUT, OTHER };

//! Raised by the kernels below, the message is the bare error text
struct KernelError : public std::runtime_error {
	KernelError(ErrorKind kind_p, const std::string &message) : std::runtime_error(message), kind(kind_p) {
	}
	ErrorKind kind;
};

//! An interval as the three units it is kept in
struct IntervalValue {
	int32_t months = 0;
	int32_t days = 0;
	int64_t micros = 0;
	/// Set for the width of a bucket when it is the same constant for all rows
	bool constant = false;
};

//----------------------------------------------------------------------------------------------------------------------
// date_trunc
//----------------------------------------------------------------------------------------------------------------------
//! Resolves a part specifier ("day", "month", ...) to an opaque id. Throws for unknown and unsupported parts.
int ParseTruncPart(const std::string &part);
//! Truncates a finite instant to the part, in the time zone of the calendar
int64_t DateTrunc(Calendar &calendar, int part, int64_t instant);

//----------------------------------------------------------------------------------------------------------------------
// make_timestamptz
//----------------------------------------------------------------------------------------------------------------------
//! The instant of the wall clock parts in the time zone of the calendar
int64_t MakeTimestampTZ(Calendar &calendar, int64_t year, int64_t month, int64_t day, int64_t hour, int64_t minute,
                        double seconds);

//----------------------------------------------------------------------------------------------------------------------
// Casts
//----------------------------------------------------------------------------------------------------------------------
//! The date (days since the epoch) of a finite instant in the time zone of the calendar. False with the error when the
//! date cannot be represented.
bool InstantToDate(Calendar &calendar, int64_t instant, int32_t &days, std::string &error);
//! The time of day and the UTC offset (seconds) of a finite instant in the time zone of the calendar. False with the
//! error text when the instant cannot be converted.
bool InstantToTimeTZ(Calendar &calendar, int64_t instant, int64_t &time_micros, int32_t &offset, std::string &error);
//! The UTC offset (seconds) of the calendar at the current time. False with the error when a TIMETZ cannot hold it.
bool CurrentTimeTZOffset(Calendar &calendar, int32_t &offset, std::string &error);

//----------------------------------------------------------------------------------------------------------------------
// time_bucket
//----------------------------------------------------------------------------------------------------------------------
//! The calendar is expected to be set to the time zone the buckets are computed in. Finite and infinite instants are
//! accepted, an infinite one is returned as is.
int64_t TimeBucket(Calendar &calendar, const IntervalValue &width, int64_t instant);
int64_t TimeBucketOffset(Calendar &calendar, const IntervalValue &width, int64_t instant, const IntervalValue &offset);
//! The origin must be finite
int64_t TimeBucketOrigin(Calendar &calendar, const IntervalValue &width, int64_t instant, int64_t origin);
//! Buckets that start at the default origin as seen in the time zone of the calendar
int64_t TimeBucketZone(Calendar &calendar, const IntervalValue &width, int64_t instant);

} // namespace icu_bridge
