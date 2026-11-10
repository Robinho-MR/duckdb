#include "icu_bridge_arithmetic.hpp"

#include "icu_bridge_internal.hpp"

#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "tz_calendar.hpp"

namespace duckdb {
namespace icu_arith {

//----------------------------------------------------------------------------------------------------------------------
// Calendar helpers
//----------------------------------------------------------------------------------------------------------------------
static timestamp_tz_t GetTimeUnsafe(Calendar *calendar, uint64_t micros = 0) {
	const auto millis = calendar->GetTime();
	if (calendar->HasFailed()) {
		throw InternalException("Unable to get calendar time.");
	}
	int64_t result;
	if (!Timestamp::TryCombineUnits(millis, int64_t(micros), Interval::MICROS_PER_MSEC, result)) {
		throw OutOfRangeException("Unable to get calendar time: timestamp out of range");
	}
	return timestamp_tz_t(result);
}

static bool TryGetTime(Calendar *calendar, uint64_t micros, timestamp_tz_t &result) {
	auto millis = calendar->GetTime();
	if (calendar->HasFailed()) {
		return false;
	}
	if (!Timestamp::TryCombineUnits(millis, int64_t(micros), Interval::MICROS_PER_MSEC, result.value)) {
		return false;
	}
	return result.IsFinite();
}

static timestamp_tz_t GetTime(Calendar *calendar, uint64_t micros = 0) {
	timestamp_tz_t result;
	if (!TryGetTime(calendar, micros, result)) {
		throw ConversionException("ICU date overflows timestamp range");
	}
	return result;
}

static uint64_t SetTime(Calendar *calendar, timestamp_tz_t date) {
	int64_t millis = date.value / Interval::MICROS_PER_MSEC;
	int64_t micros = date.value % Interval::MICROS_PER_MSEC;
	if (micros < 0) {
		--millis;
		micros += Interval::MICROS_PER_MSEC;
	}
	calendar->SetTime(millis);
	return uint64_t(micros);
}

static uint64_t SetTimeNS(Calendar *calendar, timestamp_tz_ns_t date) {
	int64_t millis = date.value / Interval::NANOS_PER_MSEC;
	int64_t nanos = date.value % Interval::NANOS_PER_MSEC;
	if (nanos < 0) {
		--millis;
		nanos += Interval::NANOS_PER_MSEC;
	}
	calendar->SetTime(millis);
	return uint64_t(nanos);
}

static int32_t ExtractField(Calendar *calendar, CalendarField field) {
	const auto result = calendar->Get(field);
	if (calendar->HasFailed()) {
		throw ConversionException("Unable to extract calendar part");
	}
	return result;
}

static int32_t SubtractField(Calendar *calendar, CalendarField field, timestamp_tz_t end_date) {
	const int64_t millis = end_date.value / Interval::MICROS_PER_MSEC;
	const auto sub = calendar->FieldDifference(millis, field);
	if (calendar->HasFailed()) {
		throw ConversionException("Unable to subtract calendar part");
	}
	return sub;
}

//----------------------------------------------------------------------------------------------------------------------
// Truncation (used by date_diff and the midnight of the current day)
//----------------------------------------------------------------------------------------------------------------------
typedef void (*part_trunc_t)(Calendar *calendar, uint64_t &micros);
typedef int64_t (*part_sub_t)(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date);

static void PreserveOffsets(Calendar *calendar) {
	//	We have to extract _everything_ before setting anything, otherwise ICU will clear the fStamp fields
	const auto zone_offset = ExtractField(calendar, CAL_ZONE_OFFSET);
	const auto dst_offset = ExtractField(calendar, CAL_DST_OFFSET);

	calendar->Set(CAL_ZONE_OFFSET, zone_offset);
	calendar->Set(CAL_DST_OFFSET, dst_offset);
}

static void TruncMicrosecondInternal(Calendar *calendar, uint64_t &micros) {
}

static void TruncMicrosecond(Calendar *calendar, uint64_t &micros) {
	PreserveOffsets(calendar);
	TruncMicrosecondInternal(calendar, micros);
}

static void TruncMillisecondInternal(Calendar *calendar, uint64_t &micros) {
	TruncMicrosecondInternal(calendar, micros);
	micros = 0;
}

static void TruncMillisecond(Calendar *calendar, uint64_t &micros) {
	PreserveOffsets(calendar);
	TruncMillisecondInternal(calendar, micros);
}

static void TruncSecondInternal(Calendar *calendar, uint64_t &micros) {
	TruncMillisecondInternal(calendar, micros);
	calendar->Set(CAL_MILLISECOND, 0);
}

static void TruncSecond(Calendar *calendar, uint64_t &micros) {
	PreserveOffsets(calendar);
	TruncSecondInternal(calendar, micros);
}

static void TruncMinuteInternal(Calendar *calendar, uint64_t &micros) {
	TruncSecondInternal(calendar, micros);
	calendar->Set(CAL_SECOND, 0);
}

static void TruncMinute(Calendar *calendar, uint64_t &micros) {
	PreserveOffsets(calendar);
	TruncMinuteInternal(calendar, micros);
}

static void TruncHour(Calendar *calendar, uint64_t &micros) {
	TruncMinuteInternal(calendar, micros);
	calendar->Set(CAL_MINUTE, 0);
}

static void TruncDay(Calendar *calendar, uint64_t &micros) {
	TruncHour(calendar, micros);
	calendar->Set(CAL_HOUR_OF_DAY, 0);
}

static void TruncWeek(Calendar *calendar, uint64_t &micros) {
	calendar->SetFirstDayOfWeek(CAL_MONDAY);
	calendar->SetMinimalDaysInFirstWeek(4);
	TruncDay(calendar, micros);
	calendar->Set(CAL_DAY_OF_WEEK, CAL_MONDAY);
}

static void TruncMonth(Calendar *calendar, uint64_t &micros) {
	TruncDay(calendar, micros);
	calendar->Set(CAL_DATE, 1);
}

static void TruncQuarter(Calendar *calendar, uint64_t &micros) {
	TruncMonth(calendar, micros);
	auto mm = ExtractField(calendar, CAL_MONTH);
	calendar->Set(CAL_MONTH, (mm / 3) * 3);
}

static void TruncYear(Calendar *calendar, uint64_t &micros) {
	TruncMonth(calendar, micros);
	calendar->Set(CAL_MONTH, CAL_JANUARY);
}

static void TruncISOYear(Calendar *calendar, uint64_t &micros) {
	TruncWeek(calendar, micros);
	calendar->Set(CAL_WEEK_OF_YEAR, 1);
}

static void TruncDecade(Calendar *calendar, uint64_t &micros) {
	TruncYear(calendar, micros);
	auto yyyy = ExtractField(calendar, CAL_YEAR) / 10;
	calendar->Set(CAL_YEAR, yyyy * 10);
}

static void TruncCentury(Calendar *calendar, uint64_t &micros) {
	TruncYear(calendar, micros);
	auto yyyy = ExtractField(calendar, CAL_YEAR) / 100;
	calendar->Set(CAL_YEAR, yyyy * 100);
}

static void TruncMillenium(Calendar *calendar, uint64_t &micros) {
	TruncYear(calendar, micros);
	auto yyyy = ExtractField(calendar, CAL_YEAR) / 1000;
	calendar->Set(CAL_YEAR, yyyy * 1000);
}

static void TruncEra(Calendar *calendar, uint64_t &micros) {
	TruncYear(calendar, micros);
	auto era = ExtractField(calendar, CAL_ERA);
	calendar->Set(CAL_YEAR, 0);
	calendar->Set(CAL_ERA, era);
}

static part_trunc_t TruncationFactory(DatePartSpecifier type) {
	switch (type) {
	case DatePartSpecifier::ERA:
		return TruncEra;
	case DatePartSpecifier::MILLENNIUM:
		return TruncMillenium;
	case DatePartSpecifier::CENTURY:
		return TruncCentury;
	case DatePartSpecifier::DECADE:
		return TruncDecade;
	case DatePartSpecifier::YEAR:
		return TruncYear;
	case DatePartSpecifier::QUARTER:
		return TruncQuarter;
	case DatePartSpecifier::MONTH:
		return TruncMonth;
	case DatePartSpecifier::WEEK:
	case DatePartSpecifier::YEARWEEK:
		return TruncWeek;
	case DatePartSpecifier::ISOYEAR:
		return TruncISOYear;
	case DatePartSpecifier::DAY:
	case DatePartSpecifier::DOW:
	case DatePartSpecifier::ISODOW:
	case DatePartSpecifier::DOY:
	case DatePartSpecifier::JULIAN_DAY:
		return TruncDay;
	case DatePartSpecifier::HOUR:
		return TruncHour;
	case DatePartSpecifier::MINUTE:
		return TruncMinute;
	case DatePartSpecifier::SECOND:
	case DatePartSpecifier::EPOCH:
		return TruncSecond;
	case DatePartSpecifier::MILLISECONDS:
		return TruncMillisecond;
	case DatePartSpecifier::MICROSECONDS:
		return TruncMicrosecond;
	default:
		throw NotImplementedException("Specifier type not implemented for ICU DATETRUNC");
	}
}

//----------------------------------------------------------------------------------------------------------------------
// Subtraction by part
//----------------------------------------------------------------------------------------------------------------------
//	ICU only has 32 bit precision for date parts, so it can overflow a high resolution.
//	Since there is no difference between ICU and the obvious calculations,
//	we make these using the DuckDB internal type.
static int64_t SubtractMicrosecond(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
	return SubtractOperatorOverflowCheck::Operation<int64_t, int64_t, int64_t>(end_date.value, start_date.value);
}

template <int64_t MICROS_PER_UNIT>
static int64_t SubtractUnits(timestamp_tz_t start_date, timestamp_tz_t end_date) {
	int64_t delta_us;
	if (TrySubtractOperator::Operation(end_date.value, start_date.value, delta_us)) {
		return delta_us / MICROS_PER_UNIT;
	}
	return Interval::GetDifferenceInUnits(start_date.value, end_date.value, MICROS_PER_UNIT);
}

static int64_t SubtractMillisecond(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
	return SubtractUnits<Interval::MICROS_PER_MSEC>(start_date, end_date);
}

static int64_t SubtractSecond(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
	return SubtractUnits<Interval::MICROS_PER_SEC>(start_date, end_date);
}

static int64_t SubtractMinute(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
	return SubtractUnits<Interval::MICROS_PER_MINUTE>(start_date, end_date);
}

static int64_t SubtractHour(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
	SetTime(calendar, start_date);
	return SubtractField(calendar, CAL_HOUR_OF_DAY, end_date);
}

static int64_t SubtractDay(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
	SetTime(calendar, start_date);
	return SubtractField(calendar, CAL_DATE, end_date);
}

static int64_t SubtractWeek(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
	calendar->SetFirstDayOfWeek(CAL_MONDAY);
	calendar->SetMinimalDaysInFirstWeek(4);
	SetTime(calendar, start_date);
	return SubtractField(calendar, CAL_WEEK_OF_YEAR, end_date);
}

static int64_t SubtractMonth(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
	SetTime(calendar, start_date);
	return SubtractField(calendar, CAL_MONTH, end_date);
}

static int64_t SubtractQuarter(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
	// No ICU part for this, so do it manually. This will not work for lunar calendars!
	return SubtractMonth(calendar, start_date, end_date) / 3;
}

static int64_t SubtractYear(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
	SetTime(calendar, start_date);
	return SubtractField(calendar, CAL_YEAR, end_date);
}

static int64_t SubtractISOYear(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
	calendar->SetFirstDayOfWeek(CAL_MONDAY);
	calendar->SetMinimalDaysInFirstWeek(4);
	SetTime(calendar, start_date);
	return SubtractField(calendar, CAL_YEAR_WOY, end_date);
}

static int64_t SubtractDecade(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
	return SubtractYear(calendar, start_date, end_date) / 10;
}

static int64_t SubtractCentury(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
	return SubtractYear(calendar, start_date, end_date) / 100;
}

static int64_t SubtractMillenium(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
	return SubtractYear(calendar, start_date, end_date) / 1000;
}

static int64_t SubtractEra(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
	SetTime(calendar, start_date);
	return SubtractField(calendar, CAL_ERA, end_date);
}

static part_sub_t SubtractFactory(DatePartSpecifier type) {
	switch (type) {
	case DatePartSpecifier::MILLENNIUM:
		return SubtractMillenium;
	case DatePartSpecifier::CENTURY:
		return SubtractCentury;
	case DatePartSpecifier::DECADE:
		return SubtractDecade;
	case DatePartSpecifier::YEAR:
		return SubtractYear;
	case DatePartSpecifier::QUARTER:
		return SubtractQuarter;
	case DatePartSpecifier::MONTH:
		return SubtractMonth;
	case DatePartSpecifier::WEEK:
	case DatePartSpecifier::YEARWEEK:
		return SubtractWeek;
	case DatePartSpecifier::ISOYEAR:
		return SubtractISOYear;
	case DatePartSpecifier::DAY:
	case DatePartSpecifier::DOW:
	case DatePartSpecifier::ISODOW:
	case DatePartSpecifier::DOY:
	case DatePartSpecifier::JULIAN_DAY:
		return SubtractDay;
	case DatePartSpecifier::HOUR:
		return SubtractHour;
	case DatePartSpecifier::MINUTE:
		return SubtractMinute;
	case DatePartSpecifier::SECOND:
	case DatePartSpecifier::EPOCH:
		return SubtractSecond;
	case DatePartSpecifier::MILLISECONDS:
		return SubtractMillisecond;
	case DatePartSpecifier::MICROSECONDS:
		return SubtractMicrosecond;
	case DatePartSpecifier::ERA:
		return SubtractEra;
	default:
		throw NotImplementedException("Specifier type not implemented for ICU subtraction");
	}
}

//----------------------------------------------------------------------------------------------------------------------
// Differences (MS-SQL style: truncate both arguments, then subtract)
//----------------------------------------------------------------------------------------------------------------------
static int64_t DifferenceEra(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
	SetTime(calendar, start_date);
	const auto start_era = ExtractField(calendar, CAL_ERA);
	SetTime(calendar, end_date);
	return int64_t(ExtractField(calendar, CAL_ERA)) - start_era;
}

static int64_t DifferenceEra(Calendar *calendar, timestamp_tz_ns_t start_date, timestamp_tz_ns_t end_date) {
	SetTimeNS(calendar, start_date);
	const auto start_era = ExtractField(calendar, CAL_ERA);
	SetTimeNS(calendar, end_date);
	return int64_t(ExtractField(calendar, CAL_ERA)) - start_era;
}

static timestamp_tz_t TruncateForDiff(Calendar *calendar, timestamp_tz_t date, part_trunc_t trunc_func) {
	auto micros = SetTime(calendar, date);
	trunc_func(calendar, micros);
	return GetTimeUnsafe(calendar, micros);
}

static timestamp_tz_t TruncateForDiff(Calendar *calendar, timestamp_tz_ns_t date, part_trunc_t trunc_func) {
	auto nanos = SetTimeNS(calendar, date);
	// Adapt TIMESTAMPTZ_NS to the existing microsecond-or-coarser date_diff path.
	uint64_t micros = nanos / Interval::NANOS_PER_MICRO;
	trunc_func(calendar, micros);
	return GetTimeUnsafe(calendar, micros);
}

static part_trunc_t DiffTruncationFactory(DatePartSpecifier type) {
	switch (type) {
	case DatePartSpecifier::WEEK:
		//	Weeks are computed without anchors
		return TruncationFactory(DatePartSpecifier::DAY);
	default:
		break;
	}
	return TruncationFactory(type);
}

template <typename T>
static int64_t Difference(Calendar *calendar, DatePartSpecifier part, T start_date, T end_date) {
	if (part == DatePartSpecifier::ERA) {
		return DifferenceEra(calendar, start_date, end_date);
	}
	auto trunc_func = DiffTruncationFactory(part);
	auto sub_func = SubtractFactory(part);
	// Truncate the two arguments. This is safe because we will stay in range
	auto start_micros = TruncateForDiff(calendar, start_date, trunc_func);
	auto end_micros = TruncateForDiff(calendar, end_date, trunc_func);
	return sub_func(calendar, start_micros, end_micros);
}

//----------------------------------------------------------------------------------------------------------------------
// Interval arithmetic
//----------------------------------------------------------------------------------------------------------------------
static inline void CalendarAddHour(Calendar *calendar, int64_t interval_hour) {
	if (interval_hour >= 0) {
		while (interval_hour > 0) {
			calendar->Add(CAL_HOUR, interval_hour > NumericLimits<int32_t>::Maximum()
			                            ? NumericLimits<int32_t>::Maximum()
			                            : static_cast<int32_t>(interval_hour));
			interval_hour -= NumericLimits<int32_t>::Maximum();
		}
	} else {
		while (interval_hour < 0) {
			calendar->Add(CAL_HOUR, interval_hour < NumericLimits<int32_t>::Minimum()
			                            ? NumericLimits<int32_t>::Minimum()
			                            : static_cast<int32_t>(interval_hour));
			interval_hour -= NumericLimits<int32_t>::Minimum();
		}
	}
}

static timestamp_tz_t CalendarAdd(Calendar *calendar, timestamp_tz_t timestamp, interval_t interval) {
	if (!timestamp.IsFinite()) {
		return timestamp;
	}

	int64_t millis = timestamp.value / Interval::MICROS_PER_MSEC;
	int64_t micros = timestamp.value % Interval::MICROS_PER_MSEC;

	// Manually move the µs
	micros += interval.micros % Interval::MICROS_PER_MSEC;
	if (micros >= Interval::MICROS_PER_MSEC) {
		micros -= Interval::MICROS_PER_MSEC;
		++millis;
	} else if (micros < 0) {
		micros += Interval::MICROS_PER_MSEC;
		--millis;
	}

	// Now use the calendar to add the other parts
	calendar->SetTime(millis);

	// Break units apart to avoid overflow
	auto interval_h = interval.micros / Interval::MICROS_PER_MSEC;

	const auto interval_ms = static_cast<int32_t>(interval_h % Interval::MSECS_PER_SEC);
	interval_h /= Interval::MSECS_PER_SEC;

	const auto interval_s = static_cast<int32_t>(interval_h % Interval::SECS_PER_MINUTE);
	interval_h /= Interval::SECS_PER_MINUTE;

	const auto interval_m = static_cast<int32_t>(interval_h % Interval::MINS_PER_HOUR);
	interval_h /= Interval::MINS_PER_HOUR;

	if (interval.months < 0 || interval.days < 0 || interval.micros < 0) {
		// Add interval fields from lowest to highest (non-ragged to ragged)
		calendar->Add(CAL_MILLISECOND, interval_ms);
		calendar->Add(CAL_SECOND, interval_s);
		calendar->Add(CAL_MINUTE, interval_m);
		CalendarAddHour(calendar, interval_h);

		// PG Adds months before days
		calendar->Add(CAL_MONTH, interval.months);
		calendar->Add(CAL_DATE, interval.days);
	} else {
		// PG Adds months before days
		calendar->Add(CAL_MONTH, interval.months);
		calendar->Add(CAL_DATE, interval.days);

		// Add interval fields from highest to lowest (ragged to non-ragged)
		CalendarAddHour(calendar, interval_h);
		calendar->Add(CAL_MINUTE, interval_m);
		calendar->Add(CAL_SECOND, interval_s);
		calendar->Add(CAL_MILLISECOND, interval_ms);
	}

	return GetTime(calendar, micros);
}

static interval_t CalendarSub(Calendar *calendar, timestamp_tz_t end_date, timestamp_tz_t start_date) {
	if (!end_date.IsFinite() || !start_date.IsFinite()) {
		throw InvalidInputException("Cannot subtract infinite timestamps");
	}
	if (start_date > end_date) {
		auto negated = CalendarSub(calendar, start_date, end_date);
		return {-negated.months, -negated.days, -negated.micros};
	}

	auto start_micros = SetTime(calendar, start_date);
	auto end_micros = (uint64_t)(end_date.value % Interval::MICROS_PER_MSEC);

	// Borrow 1ms from end_date if we wrap. This works because start_date <= end_date
	// and if the µs are out of order, then there must be an extra ms.
	if (start_micros > (idx_t)end_micros) {
		end_date.value -= Interval::MICROS_PER_MSEC;
		end_micros += Interval::MICROS_PER_MSEC;
	}

	//	Timestamp differences do not use months, so start with days
	interval_t result;
	result.months = 0;
	result.days = SubtractField(calendar, CAL_DATE, end_date);

	auto hour_diff = SubtractField(calendar, CAL_HOUR_OF_DAY, end_date);
	auto min_diff = SubtractField(calendar, CAL_MINUTE, end_date);
	auto sec_diff = SubtractField(calendar, CAL_SECOND, end_date);
	auto ms_diff = SubtractField(calendar, CAL_MILLISECOND, end_date);
	auto micros_diff = UnsafeNumericCast<int32_t>(ms_diff * Interval::MICROS_PER_MSEC + (end_micros - start_micros));
	result.micros = Time::FromTime(hour_diff, min_diff, sec_diff, micros_diff).value;

	return result;
}

static TimestampComponents GetComponents(timestamp_tz_t ts, Calendar *calendar) {
	// Get the parts in the given time zone
	uint64_t micros = SetTime(calendar, ts);

	TimestampComponents ts_data;
	ts_data.year = ExtractField(calendar, CAL_EXTENDED_YEAR);
	ts_data.month = ExtractField(calendar, CAL_MONTH) + 1;
	ts_data.day = ExtractField(calendar, CAL_DATE);

	ts_data.hour = ExtractField(calendar, CAL_HOUR_OF_DAY);
	ts_data.minute = ExtractField(calendar, CAL_MINUTE);
	ts_data.second = ExtractField(calendar, CAL_SECOND);
	ts_data.microsecond =
	    UnsafeNumericCast<int32_t>(ExtractField(calendar, CAL_MILLISECOND) * Interval::MICROS_PER_MSEC + micros);
	ts_data.nanosecond = 0;
	return ts_data;
}

static interval_t CalendarAge(Calendar *calendar, bool gregorian, timestamp_tz_t end_date, timestamp_tz_t start_date) {
	if (gregorian) {
		auto start_data = GetComponents(start_date, calendar);
		auto end_data = GetComponents(end_date, calendar);
		return Interval::GetAge(end_data, start_data, start_date > end_date);
	}
	// fallback for non-gregorian calendars, since Interval::GetAge does not handle
	if (start_date > end_date) {
		auto negated = CalendarSub(calendar, start_date, end_date);
		return {-negated.months, -negated.days, -negated.micros};
	}

	auto start_micros = SetTime(calendar, start_date);
	auto end_micros = (uint64_t)(end_date.value % Interval::MICROS_PER_MSEC);

	// Borrow 1ms from end_date if we wrap. This works because start_date <= end_date
	// and if the µs are out of order, then there must be an extra ms.
	if (start_micros > (idx_t)end_micros) {
		end_date.value -= Interval::MICROS_PER_MSEC;
		end_micros += Interval::MICROS_PER_MSEC;
	}

	//	Lunar calendars have uneven numbers of months, so we just diff months, not years
	interval_t result;
	result.months = SubtractField(calendar, CAL_MONTH, end_date);
	result.days = SubtractField(calendar, CAL_DATE, end_date);

	auto hour_diff = SubtractField(calendar, CAL_HOUR_OF_DAY, end_date);
	auto min_diff = SubtractField(calendar, CAL_MINUTE, end_date);
	auto sec_diff = SubtractField(calendar, CAL_SECOND, end_date);
	auto ms_diff = SubtractField(calendar, CAL_MILLISECOND, end_date);
	auto micros_diff = UnsafeNumericCast<int32_t>(ms_diff * Interval::MICROS_PER_MSEC + (end_micros - start_micros));
	result.micros = Time::FromTime(hour_diff, min_diff, sec_diff, micros_diff).value;

	return result;
}

} // namespace icu_arith
} // namespace duckdb

namespace icu_bridge {

namespace {

using duckdb::datetime::Calendar;

Calendar *Native(icu_bridge::Calendar &calendar) {
	return &CalendarAccess::Native(calendar);
}

ArithmeticErrorKind KindOf(duckdb::ExceptionType type) {
	switch (type) {
	case duckdb::ExceptionType::CONVERSION:
		return ArithmeticErrorKind::CONVERSION;
	case duckdb::ExceptionType::INVALID_INPUT:
		return ArithmeticErrorKind::INVALID_INPUT;
	case duckdb::ExceptionType::OUT_OF_RANGE:
		return ArithmeticErrorKind::OUT_OF_RANGE;
	case duckdb::ExceptionType::NOT_IMPLEMENTED:
		return ArithmeticErrorKind::NOT_IMPLEMENTED;
	default:
		return ArithmeticErrorKind::INTERNAL;
	}
}

//! Runs the kernel, translating engine exceptions
template <class F>
auto Guard(F &&function) -> decltype(function()) {
	try {
		return function();
	} catch (duckdb::Exception &ex) {
		duckdb::ErrorData data(ex);
		throw ArithmeticError(KindOf(data.Type()), data.RawMessage());
	}
}

IntervalValue ToValue(duckdb::interval_t interval) {
	return IntervalValue {interval.months, interval.days, interval.micros};
}

duckdb::interval_t FromValue(const IntervalValue &interval) {
	return duckdb::interval_t {interval.months, interval.days, interval.micros};
}

} // namespace

DiffPart ParseDiffPart(const std::string &specifier) {
	return Guard([&] { return DiffPart {static_cast<uint8_t>(duckdb::GetDatePartSpecifier(specifier))}; });
}

static duckdb::DatePartSpecifier ToSpecifier(DiffPart part) {
	return static_cast<duckdb::DatePartSpecifier>(part.id);
}

bool SupportsIntervals(icu_bridge::Calendar &calendar) {
	return Native(calendar)->GetMaximum(duckdb::CAL_MONTH) < 12; // 0-based
}

bool IsGregorianCalendar(const std::string &name) {
	return name.empty() || duckdb::StringUtil::CIEquals(name, "gregorian");
}

int64_t AddInterval(icu_bridge::Calendar &calendar, int64_t timestamp, const IntervalValue &interval) {
	return Guard([&] {
		return duckdb::icu_arith::CalendarAdd(Native(calendar), duckdb::timestamp_tz_t(timestamp), FromValue(interval))
		    .value;
	});
}

int64_t SubInterval(icu_bridge::Calendar &calendar, int64_t timestamp, const IntervalValue &interval) {
	return Guard([&] {
		const auto negated = duckdb::Interval::Invert(FromValue(interval));
		return duckdb::icu_arith::CalendarAdd(Native(calendar), duckdb::timestamp_tz_t(timestamp), negated).value;
	});
}

IntervalValue SubTimestamps(icu_bridge::Calendar &calendar, int64_t end, int64_t start) {
	return Guard([&] {
		return ToValue(duckdb::icu_arith::CalendarSub(Native(calendar), duckdb::timestamp_tz_t(end),
		                                              duckdb::timestamp_tz_t(start)));
	});
}

IntervalValue Age(icu_bridge::Calendar &calendar, bool gregorian, int64_t end, int64_t start) {
	return Guard([&] {
		return ToValue(duckdb::icu_arith::CalendarAge(Native(calendar), gregorian, duckdb::timestamp_tz_t(end),
		                                              duckdb::timestamp_tz_t(start)));
	});
}

int64_t MidnightOf(icu_bridge::Calendar &calendar, int64_t instant) {
	return Guard([&] {
		auto *native = Native(calendar);
		auto micros = duckdb::icu_arith::SetTime(native, duckdb::timestamp_tz_t(instant));
		duckdb::icu_arith::TruncDay(native, micros);
		return duckdb::icu_arith::GetTime(native).value;
	});
}

int64_t DateSub(icu_bridge::Calendar &calendar, DiffPart part, int64_t start, int64_t end) {
	return Guard([&] {
		auto sub_func = duckdb::icu_arith::SubtractFactory(ToSpecifier(part));
		return sub_func(Native(calendar), duckdb::timestamp_tz_t(start), duckdb::timestamp_tz_t(end));
	});
}

int64_t DateDiff(icu_bridge::Calendar &calendar, DiffPart part, int64_t start, int64_t end) {
	return Guard([&] {
		return duckdb::icu_arith::Difference(Native(calendar), ToSpecifier(part), duckdb::timestamp_tz_t(start),
		                                     duckdb::timestamp_tz_t(end));
	});
}

int64_t DateDiffNanos(icu_bridge::Calendar &calendar, DiffPart part, int64_t start, int64_t end) {
	return Guard([&] {
		return duckdb::icu_arith::Difference(Native(calendar), ToSpecifier(part), duckdb::timestamp_tz_ns_t(start),
		                                     duckdb::timestamp_tz_ns_t(end));
	});
}

} // namespace icu_bridge
