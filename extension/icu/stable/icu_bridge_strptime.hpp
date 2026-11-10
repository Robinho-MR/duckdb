//===----------------------------------------------------------------------===//
//                         DuckDB
//
// icu_bridge_strptime.hpp
//
// Parsing and formatting of timestamps with time zones, std-only: strptime, strftime and the string casts. Instants are
// int64 microseconds (or nanoseconds, where stated) since the epoch.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "icu_bridge.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace icu_bridge {

enum class ErrorKind : uint8_t { NONE, INVALID_INPUT, CONVERSION, OUT_OF_RANGE, NOT_IMPLEMENTED };

//! Why a kernel failed
struct Failure {
	ErrorKind kind = ErrorKind::NONE;
	std::string message;
};

//! Finds the calendar of a time zone name, nullptr (and the error) when the zone is unknown
using ZoneLookup = std::function<Calendar *(const std::string &zone, std::string &error)>;

enum class TimestampKind : uint8_t { TIMESTAMP, TIMESTAMP_NS, TIMESTAMP_TZ, TIMESTAMP_TZ_NS };

//! The compiled format(s) of one strptime call
class StrptimeFormats {
public:
	~StrptimeFormats();

	//! Compiles the formats. Returns nullptr and fills the failure when one of them is not valid.
	static std::unique_ptr<StrptimeFormats> Create(const std::vector<std::string> &formats, bool is_list,
	                                               Failure &failure);

	//! The type of the result
	TimestampKind Kind() const;

	//! Parses the input with the first format that matches (a time zone name selects the calendar of that zone, a
	//! missing one the session calendar). The result is in nanoseconds for the _NS kinds. In try mode, a string that
	//! cannot be parsed returns false without a failure.
	bool Parse(std::string_view input, Calendar &calendar, const ZoneLookup &zones, bool try_mode, int64_t &result,
	           Failure &failure) const;

private:
	StrptimeFormats();
	struct Impl;
	std::unique_ptr<Impl> impl;
};

//! A strftime format, compiled lazily and kept as long as the format string stays the same
class Strftime {
public:
	Strftime();
	~Strftime();

	bool SetFormat(std::string_view format, Failure &failure);
	//! Renders a finite instant (nanoseconds if is_nanos) in the calendar's time zone. The time zone name is what %Z
	//! prints.
	bool Format(Calendar &calendar, int64_t instant, bool is_nanos, const std::string &zone_name, std::string &result,
	            Failure &failure);

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

//! The cast TIMESTAMPTZ -> VARCHAR for a finite instant (nanoseconds if is_nanos)
bool TimestampTZToString(Calendar &calendar, int64_t instant, bool is_nanos, std::string &result, Failure &failure);

//! The cast VARCHAR -> TIMESTAMPTZ. Strings without an offset are read in their time zone, or in the calendar's.
bool VarcharToTimestampTZ(Calendar &calendar, const ZoneLookup &zones, std::string_view input, bool is_nanos,
                          int64_t &result, Failure &failure);

//! The cast VARCHAR -> TIMETZ. Strings without an offset get the current offset of the calendar's time zone.
bool VarcharToTimeTZ(Calendar &calendar, std::string_view input, int64_t &micros, int32_t &offset_seconds,
                     Failure &failure);

} // namespace icu_bridge
