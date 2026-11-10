#include "icu_bridge_strptime.hpp"

#include "icu_bridge_internal.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/scalar/strftime_format.hpp"

#include <chrono>

namespace icu_bridge {

using duckdb::datetime::CAL_DATE;
using duckdb::datetime::CAL_DST_OFFSET;
using duckdb::datetime::CAL_EXTENDED_YEAR;
using duckdb::datetime::CAL_HOUR_OF_DAY;
using duckdb::datetime::CAL_MILLISECOND;
using duckdb::datetime::CAL_MINUTE;
using duckdb::datetime::CAL_MONTH;
using duckdb::datetime::CAL_SECOND;
using duckdb::datetime::CAL_ZONE_OFFSET;
using duckdb::datetime::CalendarField;

using NativeCalendar = duckdb::datetime::Calendar;

//----------------------------------------------------------------------------------------------------------------------
// Helpers
//----------------------------------------------------------------------------------------------------------------------
static void SetFailure(Failure &failure, ErrorKind kind, std::string message) {
	failure.kind = kind;
	failure.message = std::move(message);
}

static void SetFailureFromException(Failure &failure, const std::exception &ex) {
	duckdb::ErrorData data(ex);
	auto kind = ErrorKind::INVALID_INPUT;
	switch (data.Type()) {
	case duckdb::ExceptionType::CONVERSION:
		kind = ErrorKind::CONVERSION;
		break;
	case duckdb::ExceptionType::OUT_OF_RANGE:
		kind = ErrorKind::OUT_OF_RANGE;
		break;
	case duckdb::ExceptionType::NOT_IMPLEMENTED:
		kind = ErrorKind::NOT_IMPLEMENTED;
		break;
	default:
		break;
	}
	SetFailure(failure, kind, data.RawMessage());
}

//! Extracts a field, a date the calendar cannot represent is something the query asked for
static int32_t ExtractField(NativeCalendar &calendar, CalendarField field) {
	const auto result = calendar.Get(field);
	if (calendar.HasFailed()) {
		throw duckdb::ConversionException("Unable to extract calendar part");
	}
	return result;
}

static bool IsFiniteInstant(int64_t value) {
	return value != std::numeric_limits<int64_t>::max() && value != -std::numeric_limits<int64_t>::max();
}

//! The calendar time (whole milliseconds) combined with the remaining microseconds
static bool TryGetTime(NativeCalendar &calendar, uint64_t micros, int64_t &result) {
	const auto millis = calendar.GetTime();
	if (calendar.HasFailed()) {
		return false;
	}
	if (!duckdb::Timestamp::TryCombineUnits(millis, int64_t(micros), duckdb::Interval::MICROS_PER_MSEC, result)) {
		return false;
	}
	return IsFiniteInstant(result);
}

static bool TryGetTimeNS(NativeCalendar &calendar, uint64_t nanos, int64_t &result) {
	int64_t micros;
	if (!TryGetTime(calendar, nanos / duckdb::Interval::NANOS_PER_MICRO, micros)) {
		return false;
	}
	nanos %= duckdb::Interval::NANOS_PER_MICRO;
	duckdb::timestamp_ns_t ns;
	if (!duckdb::Timestamp::TryFromTimestampNanos(duckdb::timestamp_t(micros), int32_t(nanos), ns)) {
		return false;
	}
	result = ns.value;
	return true;
}

//----------------------------------------------------------------------------------------------------------------------
// strptime
//----------------------------------------------------------------------------------------------------------------------
using ParseResult = duckdb::StrpTimeFormat::ParseResult;

struct StrptimeFormats::Impl {
	std::vector<duckdb::StrpTimeFormat> formats;
	//! Whether the calendar converts the parsed parts (formats with a time zone, or a list with an offset)
	bool use_calendar = false;
	TimestampKind kind = TimestampKind::TIMESTAMP;
};

StrptimeFormats::StrptimeFormats() : impl(new Impl()) {
}

StrptimeFormats::~StrptimeFormats() = default;

std::unique_ptr<StrptimeFormats> StrptimeFormats::Create(const std::vector<std::string> &formats, bool is_list,
                                                         Failure &failure) {
	std::unique_ptr<StrptimeFormats> result(new StrptimeFormats());
	bool has_zone_name = false;
	bool has_offset = false;
	bool has_nanos = false;
	for (const auto &specifier : formats) {
		duckdb::StrpTimeFormat format;
		format.format_specifier = specifier;
		const auto error = duckdb::StrTimeFormat::ParseFormatSpecifier(specifier, format);
		if (!error.empty()) {
			SetFailure(failure, ErrorKind::INVALID_INPUT,
			           "Failed to parse format specifier " + specifier + ": " + error);
			return nullptr;
		}
		has_zone_name = has_zone_name || format.HasFormatSpecifier(duckdb::StrTimeSpecifier::TZ_NAME);
		has_offset = has_offset || format.HasFormatSpecifier(duckdb::StrTimeSpecifier::UTC_OFFSET);
		has_nanos = has_nanos || format.HasFormatSpecifier(duckdb::StrTimeSpecifier::NANOSECOND_PADDED);
		result->impl->formats.push_back(std::move(format));
	}
	const auto has_zone = has_zone_name || has_offset;
	result->impl->use_calendar = is_list ? has_zone : has_zone_name;
	if (has_zone) {
		result->impl->kind = has_nanos ? TimestampKind::TIMESTAMP_TZ_NS : TimestampKind::TIMESTAMP_TZ;
	} else {
		result->impl->kind = has_nanos ? TimestampKind::TIMESTAMP_NS : TimestampKind::TIMESTAMP;
	}
	return result;
}

TimestampKind StrptimeFormats::Kind() const {
	return impl->kind;
}

//! Sets the parts of the parsed string in the calendar, returns the microseconds (or nanoseconds) the calendar lacks
static uint64_t SetParsedParts(NativeCalendar &calendar, const ParseResult &parsed,
                               const duckdb::StrpTimeFormat &format, bool is_nanos) {
	uint64_t remainder;
	int32_t millis;
	if (is_nanos) {
		remainder = uint64_t(parsed.data[6]);
		millis = int32_t(remainder / duckdb::Interval::NANOS_PER_MSEC);
		remainder %= duckdb::Interval::NANOS_PER_MSEC;
	} else {
		remainder = uint64_t(parsed.GetMicros());
		millis = int32_t(remainder / duckdb::Interval::MICROS_PER_MSEC);
		remainder %= duckdb::Interval::MICROS_PER_MSEC;
	}
	// Forget what the previous string left in the calendar
	calendar.SetTime(0);
	// strptime doesn't understand eras
	calendar.Set(CAL_EXTENDED_YEAR, parsed.data[0]);
	calendar.Set(CAL_MONTH, parsed.data[1] - 1);
	calendar.Set(CAL_DATE, parsed.data[2]);
	calendar.Set(CAL_HOUR_OF_DAY, parsed.data[3]);
	calendar.Set(CAL_MINUTE, parsed.data[4]);
	calendar.Set(CAL_SECOND, parsed.data[5]);
	calendar.Set(CAL_MILLISECOND, millis);

	// An offset overrides the time zone of the calendar. The DST setting is not a concern, the two just combine.
	if (format.HasFormatSpecifier(duckdb::StrTimeSpecifier::UTC_OFFSET)) {
		calendar.Set(CAL_ZONE_OFFSET, int32_t(parsed.data[7] * duckdb::Interval::MSECS_PER_SEC));
	}
	return remainder;
}

bool StrptimeFormats::Parse(std::string_view input, Calendar &calendar, const ZoneLookup &zones, bool try_mode,
                            int64_t &result, Failure &failure) const {
	const auto is_nanos = impl->kind == TimestampKind::TIMESTAMP_NS || impl->kind == TimestampKind::TIMESTAMP_TZ_NS;
	ParseResult parsed;
	try {
		for (auto &format : impl->formats) {
			if (!format.Parse(input.data(), input.size(), parsed)) {
				continue;
			}
			if (!impl->use_calendar) {
				if (try_mode) {
					if (is_nanos) {
						duckdb::timestamp_ns_t ns;
						if (parsed.TryToTimestampNS(ns)) {
							result = ns.value;
							return true;
						}
					} else {
						duckdb::timestamp_t us;
						if (parsed.TryToTimestamp(us)) {
							result = us.value;
							return true;
						}
					}
					continue;
				}
				result = is_nanos ? parsed.ToTimestampNS().value : parsed.ToTimestamp().value;
				return true;
			}
			if (parsed.is_special) {
				result = parsed.ToTimestamp().value;
				return true;
			}
			// Use the calendar of the parsed time zone, if any - the session calendar keeps the session zone
			Calendar *row_calendar = &calendar;
			if (!parsed.tz.empty()) {
				std::string error;
				row_calendar = zones(parsed.tz, error);
				if (!row_calendar) {
					if (try_mode) {
						continue;
					}
					SetFailure(failure, ErrorKind::NOT_IMPLEMENTED, error);
					return false;
				}
			}
			auto &native = CalendarAccess::Native(*row_calendar);
			const auto remainder = SetParsedParts(native, parsed, format, is_nanos);
			const auto converted =
			    is_nanos ? TryGetTimeNS(native, remainder, result) : TryGetTime(native, remainder, result);
			// The calendar is shared, do not leave the parsed fields in it
			native.SetTime(0);
			if (converted) {
				return true;
			}
			if (try_mode) {
				continue;
			}
			SetFailure(failure, ErrorKind::CONVERSION,
			           is_nanos ? "ICU date overflows timestamp_ns range" : "ICU date overflows timestamp range");
			return false;
		}
		if (!try_mode) {
			SetFailure(failure, ErrorKind::INVALID_INPUT,
			           parsed.FormatError(duckdb::string_t(input.data(), uint32_t(input.size())),
			                              impl->formats[0].format_specifier));
		}
	} catch (std::exception &ex) {
		if (!try_mode) {
			SetFailureFromException(failure, ex);
		}
	}
	return false;
}

//----------------------------------------------------------------------------------------------------------------------
// strftime
//----------------------------------------------------------------------------------------------------------------------
struct Strftime::Impl {
	duckdb::StrfTimeFormat format;
	std::string specifier;
	bool compiled = false;
};

Strftime::Strftime() : impl(new Impl()) {
}

Strftime::~Strftime() = default;

bool Strftime::SetFormat(std::string_view format, Failure &failure) {
	if (impl->compiled && impl->specifier == format) {
		return true;
	}
	impl->compiled = false;
	impl->format = duckdb::StrfTimeFormat();
	impl->specifier = std::string(format);
	const auto error = duckdb::StrTimeFormat::ParseFormatSpecifier(impl->specifier, impl->format);
	if (!error.empty()) {
		SetFailure(failure, ErrorKind::INVALID_INPUT,
		           "Failed to parse format specifier " + impl->specifier + ": " + error);
		return false;
	}
	impl->compiled = true;
	return true;
}

bool Strftime::Format(Calendar &calendar, int64_t instant, bool is_nanos, const std::string &zone_name,
                      std::string &result, Failure &failure) {
	auto &native = CalendarAccess::Native(calendar);
	try {
		// Get the parts in the given time zone
		const int64_t unit = is_nanos ? duckdb::Interval::NANOS_PER_MSEC : duckdb::Interval::MICROS_PER_MSEC;
		int64_t millis = instant / unit;
		int64_t rest = instant % unit;
		if (rest < 0) {
			--millis;
			rest += unit;
		}
		native.SetTime(millis);

		int32_t data[8];
		// strftime doesn't understand eras
		data[0] = ExtractField(native, CAL_EXTENDED_YEAR);
		data[1] = ExtractField(native, CAL_MONTH) + 1;
		data[2] = ExtractField(native, CAL_DATE);
		data[3] = ExtractField(native, CAL_HOUR_OF_DAY);
		data[4] = ExtractField(native, CAL_MINUTE);
		data[5] = ExtractField(native, CAL_SECOND);
		data[6] = int32_t(ExtractField(native, CAL_MILLISECOND) * unit + rest);
		data[7] = ExtractField(native, CAL_ZONE_OFFSET) + ExtractField(native, CAL_DST_OFFSET);
		data[7] /= duckdb::Interval::MSECS_PER_SEC;

		const auto date = duckdb::Date::FromDate(data[0], data[1], data[2]);
		duckdb::idx_t length;
		if (is_nanos) {
			length = impl->format.GetLength(date, data, zone_name.c_str());
		} else {
			const auto time = duckdb::Time::FromTime(data[3], data[4], data[5], data[6]);
			length = impl->format.GetLength(date, time, data[7], zone_name.c_str());
		}
		result.assign(length + 1, '\0');
		if (is_nanos) {
			impl->format.FormatStringNS(date, data, zone_name.c_str(), &result[0]);
		} else {
			impl->format.FormatString(date, data, zone_name.c_str(), &result[0]);
		}
		result.resize(length);
	} catch (std::exception &ex) {
		SetFailureFromException(failure, ex);
		return false;
	}
	return true;
}

//----------------------------------------------------------------------------------------------------------------------
// Casts
//----------------------------------------------------------------------------------------------------------------------
bool TimestampTZToString(Calendar &calendar, int64_t instant, bool is_nanos, std::string &result, Failure &failure) {
	auto &native = CalendarAccess::Native(calendar);
	try {
		int64_t micros = instant;
		int64_t nanos = 0;
		if (is_nanos) {
			duckdb::Timestamp::SplitTime(instant, duckdb::Interval::NANOS_PER_MICRO, micros, nanos);
		}
		// Decompose the instant in the time zone
		int64_t millis = micros / duckdb::Interval::MICROS_PER_MSEC;
		int64_t rest = micros % duckdb::Interval::MICROS_PER_MSEC;
		if (rest < 0) {
			--millis;
			rest += duckdb::Interval::MICROS_PER_MSEC;
		}
		native.SetTime(millis);
		int32_t year = ExtractField(native, CAL_EXTENDED_YEAR);
		const auto month = ExtractField(native, CAL_MONTH) + 1;
		const auto day = ExtractField(native, CAL_DATE);
		const auto hour = ExtractField(native, CAL_HOUR_OF_DAY);
		const auto minute = ExtractField(native, CAL_MINUTE);
		const auto second = ExtractField(native, CAL_SECOND);
		const auto microsecond =
		    int32_t(ExtractField(native, CAL_MILLISECOND) * duckdb::Interval::MICROS_PER_MSEC + rest);

		duckdb::idx_t year_length;
		bool add_bc;
		const auto date_len = duckdb::DateToStringCast::YearLength(year, year_length, add_bc);

		char micro_buffer[9];
		const auto time_len = duckdb::TimeToStringCast::MicrosLength(microsecond, micro_buffer, int32_t(nanos));

		auto offset = ExtractField(native, CAL_ZONE_OFFSET) + ExtractField(native, CAL_DST_OFFSET);
		offset /= duckdb::Interval::MSECS_PER_SEC;
		const int hour_offset = offset / duckdb::Interval::SECS_PER_HOUR;
		const int minute_offset = (offset % duckdb::Interval::SECS_PER_HOUR) / duckdb::Interval::SECS_PER_MINUTE;
		const int second_offset = offset % duckdb::Interval::SECS_PER_MINUTE;
		const auto offset_str = duckdb::Time::ToUTCOffset(hour_offset, minute_offset, second_offset);

		const auto len = date_len + 1 + time_len + offset_str.size();
		result.assign(len, '\0');
		auto buffer = &result[0];
		duckdb::DateToStringCast::Format(buffer, year, month, day, year_length, add_bc);
		buffer += date_len;
		*buffer++ = ' ';
		duckdb::TimeToStringCast::Format(buffer, time_len, hour, minute, second, 0, micro_buffer);
		buffer += time_len;
		memcpy(buffer, offset_str.c_str(), offset_str.size());
	} catch (std::exception &ex) {
		SetFailureFromException(failure, ex);
		return false;
	}
	return true;
}

bool VarcharToTimestampTZ(Calendar &calendar, const ZoneLookup &zones, std::string_view input, bool is_nanos,
                          int64_t &result, Failure &failure) {
	try {
		duckdb::timestamp_t us;
		duckdb::string_t zone(nullptr, 0);
		bool has_offset = false;
		int32_t nanos = 0;
		const auto status = duckdb::Timestamp::TryConvertTimestampTZ(input.data(), input.size(), us, true, has_offset,
		                                                             zone, is_nanos ? &nanos : nullptr);
		if (status != duckdb::TimestampCastResult::SUCCESS) {
			const auto text = std::string(input);
			SetFailure(failure, ErrorKind::CONVERSION,
			           status == duckdb::TimestampCastResult::ERROR_RANGE ? duckdb::Timestamp::RangeError(text)
			                                                              : duckdb::Timestamp::FormatError(text));
			return false;
		}
		int64_t instant = us.value;
		if (!has_offset && IsFiniteInstant(instant)) {
			// Convert the parts to the time zone, given or the session's, if no offset was provided
			Calendar *row_calendar = &calendar;
			if (zone.GetSize()) {
				std::string error;
				row_calendar = zones(std::string(zone.GetData(), zone.GetSize()), error);
				if (!row_calendar) {
					SetFailure(failure, ErrorKind::CONVERSION, error);
					return false;
				}
			}
			// The calendar is shared, forget the era and fields of what it converted before
			CalendarAccess::Native(*row_calendar).SetTime(0);
			if (!row_calendar->FromLocal(us.value, instant)) {
				SetFailure(failure, ErrorKind::CONVERSION, "ICU date overflows timestamp range");
				return false;
			}
		}
		if (is_nanos) {
			duckdb::timestamp_ns_t ns;
			if (!duckdb::Timestamp::TryFromTimestampNanos(duckdb::timestamp_t(instant), nanos, ns)) {
				SetFailure(failure, ErrorKind::CONVERSION, duckdb::Timestamp::RangeError(std::string(input)));
				return false;
			}
			instant = ns.value;
		}
		result = instant;
	} catch (std::exception &ex) {
		SetFailureFromException(failure, ex);
		return false;
	}
	return true;
}

bool VarcharToTimeTZ(Calendar &calendar, std::string_view input, int64_t &micros, int32_t &offset_seconds,
                     Failure &failure) {
	try {
		duckdb::dtime_tz_t parsed;
		bool has_offset = false;
		duckdb::idx_t pos = 0;
		if (!duckdb::Time::TryConvertTimeTZ(input.data(), input.size(), pos, parsed, has_offset, false)) {
			SetFailure(failure, ErrorKind::CONVERSION, duckdb::Time::ConversionError(std::string(input)));
			return false;
		}
		micros = parsed.time().value;
		offset_seconds = parsed.offset();
		if (!has_offset) {
			// The offset of the time zone now
			const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
			                     std::chrono::system_clock::now().time_since_epoch())
			                     .count();
			int64_t offset;
			if (!calendar.GetPart(now, Part::ZONE_OFFSET, offset)) {
				SetFailure(failure, ErrorKind::CONVERSION, "Unable to extract calendar part");
				return false;
			}
			if (offset < duckdb::dtime_tz_t::MIN_OFFSET || offset > duckdb::dtime_tz_t::MAX_OFFSET) {
				duckdb::interval_t interval {0, 0, offset * duckdb::Interval::MICROS_PER_SEC};
				SetFailure(failure, ErrorKind::CONVERSION,
				           duckdb::StringUtil::Format("Time zone offset %s is out of range, expected a value between "
				                                      "-15:59:59 and +15:59:59",
				                                      duckdb::Interval::ToString(interval)));
				return false;
			}
			offset_seconds = int32_t(offset);
		}
	} catch (std::exception &ex) {
		SetFailureFromException(failure, ex);
		return false;
	}
	return true;
}

} // namespace icu_bridge
