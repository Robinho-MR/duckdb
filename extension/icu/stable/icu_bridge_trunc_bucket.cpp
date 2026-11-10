#include "icu_bridge_trunc_bucket.hpp"

#include "icu_bridge_internal.hpp"

#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"

#include <cmath>

namespace icu_bridge {

namespace {

using duckdb::date_t;
using duckdb::DatePartSpecifier;
using duckdb::dtime_t;
using duckdb::interval_t;
using duckdb::timestamp_t;
using duckdb::datetime::CAL_DATE;
using duckdb::datetime::CAL_DST_OFFSET;
using duckdb::datetime::CAL_ERA;
using duckdb::datetime::CAL_HOUR;
using duckdb::datetime::CAL_HOUR_OF_DAY;
using duckdb::datetime::CAL_MILLISECOND;
using duckdb::datetime::CAL_MINUTE;
using duckdb::datetime::CAL_MONDAY;
using duckdb::datetime::CAL_MONTH;
using duckdb::datetime::CAL_SECOND;
using duckdb::datetime::CAL_WEEK_OF_YEAR;
using duckdb::datetime::CAL_YEAR;
using duckdb::datetime::CAL_ZONE_OFFSET;
using duckdb::datetime::CalendarField;
using Cal = duckdb::datetime::Calendar;

constexpr int32_t CAL_JANUARY = 0;
constexpr int64_t MICROS_PER_MSEC = duckdb::Interval::MICROS_PER_MSEC;

//! Runs the kernel and turns the engine exceptions into KernelErrors
template <class F>
auto Guard(F &&function) -> decltype(function()) {
	try {
		return function();
	} catch (duckdb::Exception &ex) {
		duckdb::ErrorData data(ex);
		ErrorKind kind;
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
		case duckdb::ExceptionType::INVALID_INPUT:
			kind = ErrorKind::INVALID_INPUT;
			break;
		default:
			kind = ErrorKind::OTHER;
			break;
		}
		throw KernelError(kind, data.RawMessage());
	}
}

Cal *Native(Calendar &calendar) {
	return &CalendarAccess::Native(calendar);
}

//----------------------------------------------------------------------------------------------------------------------
// Calendar helpers
//----------------------------------------------------------------------------------------------------------------------
//! Sets the calendar to the instant, returning the microseconds that do not fit in the milliseconds of the calendar
uint64_t SetTime(Cal *calendar, int64_t instant) {
	int64_t millis = instant / MICROS_PER_MSEC;
	int64_t micros = instant % MICROS_PER_MSEC;
	if (micros < 0) {
		--millis;
		micros += MICROS_PER_MSEC;
	}
	calendar->SetTime(millis);
	return uint64_t(micros);
}

bool TryGetTime(Cal *calendar, uint64_t micros, int64_t &result) {
	auto millis = calendar->GetTime();
	if (calendar->HasFailed()) {
		return false;
	}
	// The time is a whole number of milliseconds, so converting it back to microseconds can overflow
	if (!duckdb::Timestamp::TryCombineUnits(millis, int64_t(micros), MICROS_PER_MSEC, result)) {
		return false;
	}
	return duckdb::timestamp_t(result).IsFinite();
}

int64_t GetTime(Cal *calendar, uint64_t micros = 0) {
	int64_t result;
	if (!TryGetTime(calendar, micros, result)) {
		throw duckdb::ConversionException("ICU date overflows timestamp range");
	}
	return result;
}

int64_t GetTimeUnsafe(Cal *calendar, uint64_t micros) {
	const auto millis = calendar->GetTime();
	if (calendar->HasFailed()) {
		throw duckdb::InternalException("Unable to get calendar time.");
	}
	int64_t result;
	if (!duckdb::Timestamp::TryCombineUnits(millis, int64_t(micros), MICROS_PER_MSEC, result)) {
		throw duckdb::OutOfRangeException("Unable to get calendar time: timestamp out of range");
	}
	return result;
}

int32_t ExtractField(Cal *calendar, CalendarField field) {
	const auto result = calendar->Get(field);
	if (calendar->HasFailed()) {
		throw duckdb::ConversionException("Unable to extract calendar part");
	}
	return result;
}

int32_t SubtractField(Cal *calendar, CalendarField field, int64_t end_date) {
	const int64_t millis = end_date / MICROS_PER_MSEC;
	const auto sub = calendar->FieldDifference(millis, field);
	if (calendar->HasFailed()) {
		throw duckdb::ConversionException("Unable to subtract calendar part");
	}
	return sub;
}

bool TryTimeTZOffset(Cal *calendar, int32_t &offset) {
	offset = ExtractField(calendar, CAL_ZONE_OFFSET);
	offset += ExtractField(calendar, CAL_DST_OFFSET);
	offset /= duckdb::Interval::MSECS_PER_SEC;
	return offset >= duckdb::dtime_tz_t::MIN_OFFSET && offset <= duckdb::dtime_tz_t::MAX_OFFSET;
}

std::string TimeTZOffsetError(int32_t offset) {
	interval_t interval {0, 0, offset * duckdb::Interval::MICROS_PER_SEC};
	return duckdb::StringUtil::Format(
	    "Time zone offset %s is out of range, expected a value between -15:59:59 and +15:59:59",
	    duckdb::Interval::ToString(interval));
}

//! The wall clock of a naive timestamp, pushed into the calendar. Returns the microseconds beyond the milliseconds.
int64_t FromNaive(Cal *calendar, int64_t naive_micros) {
	const timestamp_t naive(naive_micros);
	if (!naive.IsFinite()) {
		return naive_micros;
	}
	date_t local_date;
	dtime_t local_time;
	duckdb::Timestamp::Convert(naive, local_date, local_time);

	int32_t year;
	int32_t mm;
	int32_t dd;
	duckdb::Date::Convert(local_date, year, mm, dd);

	int32_t hr;
	int32_t mn;
	int32_t secs;
	int32_t frac;
	duckdb::Time::Convert(local_time, hr, mn, secs, frac);
	const int32_t millis = frac / int32_t(MICROS_PER_MSEC);
	const uint64_t micros = frac % MICROS_PER_MSEC;

	calendar->Set(CAL_YEAR, year);
	calendar->Set(CAL_MONTH, int32_t(mm - 1));
	calendar->Set(CAL_DATE, dd);
	calendar->Set(CAL_HOUR_OF_DAY, hr);
	calendar->Set(CAL_MINUTE, mn);
	calendar->Set(CAL_SECOND, secs);
	calendar->Set(CAL_MILLISECOND, millis);
	return GetTime(calendar, micros);
}

//----------------------------------------------------------------------------------------------------------------------
// Truncation
//----------------------------------------------------------------------------------------------------------------------
using TruncFunction = void (*)(Cal *calendar, uint64_t &micros);

//! Restores the week settings of the calendar on scope exit, truncating to a week changes them
class WeekSettings {
public:
	explicit WeekSettings(Cal *calendar_p) : calendar(calendar_p) {
		auto field_calendar = dynamic_cast<duckdb::datetime::FieldCalendar *>(calendar);
		if (field_calendar) {
			first_day = field_calendar->GetFirstDayOfWeek();
			minimal_days = field_calendar->GetMinimalDaysInFirstWeek();
			active = true;
		}
	}
	~WeekSettings() {
		if (active) {
			calendar->SetFirstDayOfWeek(first_day);
			calendar->SetMinimalDaysInFirstWeek(minimal_days);
		}
	}

private:
	Cal *calendar;
	int32_t first_day = 0;
	int32_t minimal_days = 0;
	bool active = false;
};

void PreserveOffsets(Cal *calendar) {
	// Everything must be extracted before anything is set, otherwise the calendar clears its fields
	const auto zone_offset = ExtractField(calendar, CAL_ZONE_OFFSET);
	const auto dst_offset = ExtractField(calendar, CAL_DST_OFFSET);
	// Force reuse of the offsets when reassembling truncated sub-hour times
	calendar->Set(CAL_ZONE_OFFSET, zone_offset);
	calendar->Set(CAL_DST_OFFSET, dst_offset);
}

void TruncMicrosecondInternal(Cal *, uint64_t &) {
}

void TruncMicrosecond(Cal *calendar, uint64_t &micros) {
	PreserveOffsets(calendar);
	TruncMicrosecondInternal(calendar, micros);
}

void TruncMillisecondInternal(Cal *calendar, uint64_t &micros) {
	TruncMicrosecondInternal(calendar, micros);
	micros = 0;
}

void TruncMillisecond(Cal *calendar, uint64_t &micros) {
	PreserveOffsets(calendar);
	TruncMillisecondInternal(calendar, micros);
}

void TruncSecondInternal(Cal *calendar, uint64_t &micros) {
	TruncMillisecondInternal(calendar, micros);
	calendar->Set(CAL_MILLISECOND, 0);
}

void TruncSecond(Cal *calendar, uint64_t &micros) {
	PreserveOffsets(calendar);
	TruncSecondInternal(calendar, micros);
}

void TruncMinuteInternal(Cal *calendar, uint64_t &micros) {
	TruncSecondInternal(calendar, micros);
	calendar->Set(CAL_SECOND, 0);
}

void TruncMinute(Cal *calendar, uint64_t &micros) {
	PreserveOffsets(calendar);
	TruncMinuteInternal(calendar, micros);
}

void TruncHour(Cal *calendar, uint64_t &micros) {
	TruncMinuteInternal(calendar, micros);
	calendar->Set(CAL_MINUTE, 0);
}

void TruncDay(Cal *calendar, uint64_t &micros) {
	TruncHour(calendar, micros);
	calendar->Set(CAL_HOUR_OF_DAY, 0);
}

void TruncWeek(Cal *calendar, uint64_t &micros) {
	calendar->SetFirstDayOfWeek(CAL_MONDAY);
	calendar->SetMinimalDaysInFirstWeek(4);
	TruncDay(calendar, micros);
	calendar->Set(duckdb::datetime::CAL_DAY_OF_WEEK, CAL_MONDAY);
}

void TruncMonth(Cal *calendar, uint64_t &micros) {
	TruncDay(calendar, micros);
	calendar->Set(CAL_DATE, 1);
}

void TruncQuarter(Cal *calendar, uint64_t &micros) {
	TruncMonth(calendar, micros);
	auto mm = ExtractField(calendar, CAL_MONTH);
	calendar->Set(CAL_MONTH, (mm / 3) * 3);
}

void TruncYear(Cal *calendar, uint64_t &micros) {
	TruncMonth(calendar, micros);
	calendar->Set(CAL_MONTH, CAL_JANUARY);
}

void TruncISOYear(Cal *calendar, uint64_t &micros) {
	TruncWeek(calendar, micros);
	calendar->Set(CAL_WEEK_OF_YEAR, 1);
}

void TruncDecade(Cal *calendar, uint64_t &micros) {
	TruncYear(calendar, micros);
	auto yyyy = ExtractField(calendar, CAL_YEAR) / 10;
	calendar->Set(CAL_YEAR, yyyy * 10);
}

void TruncCentury(Cal *calendar, uint64_t &micros) {
	TruncYear(calendar, micros);
	auto yyyy = ExtractField(calendar, CAL_YEAR) / 100;
	calendar->Set(CAL_YEAR, yyyy * 100);
}

void TruncMillenium(Cal *calendar, uint64_t &micros) {
	TruncYear(calendar, micros);
	auto yyyy = ExtractField(calendar, CAL_YEAR) / 1000;
	calendar->Set(CAL_YEAR, yyyy * 1000);
}

void TruncEra(Cal *calendar, uint64_t &micros) {
	TruncYear(calendar, micros);
	auto era = ExtractField(calendar, CAL_ERA);
	calendar->Set(CAL_YEAR, 0);
	calendar->Set(CAL_ERA, era);
}

TruncFunction TruncationFactory(DatePartSpecifier type) {
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
		throw duckdb::NotImplementedException("Specifier type not implemented for ICU DATETRUNC");
	}
}

//----------------------------------------------------------------------------------------------------------------------
// Interval arithmetic
//----------------------------------------------------------------------------------------------------------------------
void CalendarAddHour(Cal *calendar, int64_t interval_hour) {
	constexpr int32_t INT32_MAX_VALUE = std::numeric_limits<int32_t>::max();
	constexpr int32_t INT32_MIN_VALUE = std::numeric_limits<int32_t>::min();
	if (interval_hour >= 0) {
		while (interval_hour > 0) {
			calendar->Add(CAL_HOUR,
			              interval_hour > INT32_MAX_VALUE ? INT32_MAX_VALUE : static_cast<int32_t>(interval_hour));
			interval_hour -= INT32_MAX_VALUE;
		}
	} else {
		while (interval_hour < 0) {
			calendar->Add(CAL_HOUR,
			              interval_hour < INT32_MIN_VALUE ? INT32_MIN_VALUE : static_cast<int32_t>(interval_hour));
			interval_hour -= INT32_MIN_VALUE;
		}
	}
}

int64_t Add(Cal *calendar, int64_t timestamp, interval_t interval) {
	if (!timestamp_t(timestamp).IsFinite()) {
		return timestamp;
	}
	int64_t millis = timestamp / MICROS_PER_MSEC;
	int64_t micros = timestamp % MICROS_PER_MSEC;

	// Manually move the microseconds
	micros += interval.micros % MICROS_PER_MSEC;
	if (micros >= MICROS_PER_MSEC) {
		micros -= MICROS_PER_MSEC;
		++millis;
	} else if (micros < 0) {
		micros += MICROS_PER_MSEC;
		--millis;
	}

	// Now use the calendar to add the other parts
	calendar->SetTime(millis);

	// Break units apart to avoid overflow
	auto interval_h = interval.micros / MICROS_PER_MSEC;

	const auto interval_ms = static_cast<int32_t>(interval_h % duckdb::Interval::MSECS_PER_SEC);
	interval_h /= duckdb::Interval::MSECS_PER_SEC;

	const auto interval_s = static_cast<int32_t>(interval_h % duckdb::Interval::SECS_PER_MINUTE);
	interval_h /= duckdb::Interval::SECS_PER_MINUTE;

	const auto interval_m = static_cast<int32_t>(interval_h % duckdb::Interval::MINS_PER_HOUR);
	interval_h /= duckdb::Interval::MINS_PER_HOUR;

	if (interval.months < 0 || interval.days < 0 || interval.micros < 0) {
		// Add interval fields from lowest to highest (non-ragged to ragged)
		calendar->Add(CAL_MILLISECOND, interval_ms);
		calendar->Add(CAL_SECOND, interval_s);
		calendar->Add(CAL_MINUTE, interval_m);
		CalendarAddHour(calendar, interval_h);

		// PG adds months before days
		calendar->Add(CAL_MONTH, interval.months);
		calendar->Add(CAL_DATE, interval.days);
	} else {
		// PG adds months before days
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

int64_t Sub(Cal *calendar, int64_t timestamp, interval_t interval) {
	return Add(calendar, timestamp, duckdb::Interval::Invert(interval));
}

int64_t SubtractDay(Cal *calendar, int64_t start_date, int64_t end_date) {
	SetTime(calendar, start_date);
	return SubtractField(calendar, CAL_DATE, end_date);
}

int64_t SubtractMonth(Cal *calendar, int64_t start_date, int64_t end_date) {
	SetTime(calendar, start_date);
	return SubtractField(calendar, CAL_MONTH, end_date);
}

int64_t TruncateTo(Cal *calendar, TruncFunction truncator, int64_t instant) {
	auto micros = SetTime(calendar, instant);
	truncator(calendar, micros);
	return GetTimeUnsafe(calendar, micros);
}

//----------------------------------------------------------------------------------------------------------------------
// Buckets
//----------------------------------------------------------------------------------------------------------------------
// 2000-01-03 00:00:00 (Monday) is the origin of buckets in days and shorter, 2000-01-01 of buckets in months and longer
constexpr int64_t DEFAULT_ORIGIN_MICROS_1 = 10959 * duckdb::Interval::MICROS_PER_DAY;
constexpr int64_t DEFAULT_ORIGIN_MICROS_2 = 10957 * duckdb::Interval::MICROS_PER_DAY;

enum class BucketWidthType { CONVERTIBLE_TO_MICROS, CONVERTIBLE_TO_DAYS, CONVERTIBLE_TO_MONTHS };

BucketWidthType ClassifyBucketWidth(const interval_t bucket_width, bool constant) {
	// A constant width is classified before it is validated, so zero is reported as a zero number of days
	if (constant && bucket_width.months == 0 && bucket_width.days == 0 && bucket_width.micros == 0) {
		return BucketWidthType::CONVERTIBLE_TO_DAYS;
	}
	if (bucket_width.months == 0 && bucket_width.days == 0) {
		if (bucket_width.micros <= 0) {
			throw duckdb::NotImplementedException("Period must be greater than 0");
		}
		return BucketWidthType::CONVERTIBLE_TO_MICROS;
	} else if (bucket_width.months == 0 && bucket_width.micros == 0) {
		if (bucket_width.days <= 0) {
			throw duckdb::NotImplementedException("Period must be greater than 0");
		}
		return BucketWidthType::CONVERTIBLE_TO_DAYS;
	} else if (bucket_width.days == 0 && bucket_width.micros == 0) {
		if (bucket_width.months <= 0) {
			throw duckdb::NotImplementedException("Period must be greater than 0");
		}
		return BucketWidthType::CONVERTIBLE_TO_MONTHS;
	} else if (bucket_width.months == 0) {
		throw duckdb::NotImplementedException("Day intervals cannot have time component");
	} else {
		throw duckdb::NotImplementedException("Month intervals cannot have day or time component");
	}
}

int64_t WidthConvertibleToMicrosCommon(int64_t bucket_width_micros, int64_t ts, int64_t origin, Cal *calendar) {
	if (!bucket_width_micros) {
		throw duckdb::OutOfRangeException("Can't bucket using zero microseconds");
	}
	int64_t ts_micros = duckdb::SubtractOperatorOverflowCheck::Operation<int64_t, int64_t, int64_t>(ts, origin);
	int64_t result_micros = (ts_micros / bucket_width_micros) * bucket_width_micros;
	if (ts_micros < 0 && ts_micros % bucket_width_micros != 0) {
		result_micros = duckdb::SubtractOperatorOverflowCheck::Operation<int64_t, int64_t, int64_t>(
		    result_micros, bucket_width_micros);
	}
	return Add(calendar, origin, interval_t {0, 0, result_micros});
}

//! Add resolves a local time that occurs twice (e.g. when the clocks are turned back) to its latter occurrence. If that
//! is after ts, ts lies between the two occurrences, and its bucket starts at the former occurrence.
int64_t FormerOccurrence(Cal *calendar, const int64_t bucket, const int64_t ts) {
	auto get_offset = [&](const int64_t instant) {
		SetTime(calendar, instant);
		return int64_t(ExtractField(calendar, CAL_ZONE_OFFSET)) + ExtractField(calendar, CAL_DST_OFFSET);
	};
	const auto ts_offset = get_offset(ts);
	const auto bucket_offset = get_offset(bucket);
	if (ts_offset <= bucket_offset) {
		return bucket;
	}
	const int64_t former = bucket - (ts_offset - bucket_offset) * MICROS_PER_MSEC;
	if (ts < former || get_offset(former) != ts_offset) {
		return bucket;
	}
	return former;
}

int64_t WidthConvertibleToDaysCommon(int32_t bucket_width_days, int64_t ts, int64_t origin, Cal *calendar) {
	if (!bucket_width_days) {
		throw duckdb::OutOfRangeException("Can't bucket using zero days");
	}
	int64_t ts_days = SubtractDay(calendar, origin, ts);
	int64_t result_days = (ts_days / bucket_width_days) * bucket_width_days;
	if (result_days < std::numeric_limits<int32_t>::min() || result_days > std::numeric_limits<int32_t>::max()) {
		throw duckdb::OutOfRangeException("Timestamp out of range");
	}
	auto bucket = Add(calendar, origin, interval_t {0, static_cast<int32_t>(result_days), 0});
	if (ts < bucket) {
		bucket = FormerOccurrence(calendar, bucket, ts);
	}
	while (ts < bucket) {
		bucket = Add(calendar, bucket, interval_t {0, -bucket_width_days, 0});
	}
	return bucket;
}

int64_t WidthConvertibleToMonthsCommon(int32_t bucket_width_months, int64_t ts, int64_t origin, Cal *calendar) {
	if (!bucket_width_months) {
		throw duckdb::OutOfRangeException("Can't bucket using zero months");
	}
	uint64_t tmp_micros = SetTime(calendar, ts);
	TruncMonth(calendar, tmp_micros);
	auto truncated_ts = GetTimeUnsafe(calendar, tmp_micros);

	tmp_micros = SetTime(calendar, origin);
	TruncMonth(calendar, tmp_micros);
	auto truncated_origin = GetTimeUnsafe(calendar, tmp_micros);

	int32_t ts_months = duckdb::NumericCast<int32_t, int64_t>(SubtractMonth(calendar, truncated_origin, truncated_ts));
	auto result_months = (ts_months / bucket_width_months) * bucket_width_months;
	if (ts_months < 0 && ts_months % bucket_width_months != 0) {
		result_months = duckdb::SubtractOperatorOverflowCheck::Operation<int32_t, int32_t, int32_t>(
		    result_months, bucket_width_months);
	}

	auto bucket = Add(calendar, truncated_origin, interval_t {static_cast<int32_t>(result_months), 0, 0});
	if (ts < bucket) {
		bucket = FormerOccurrence(calendar, bucket, ts);
	}
	while (ts < bucket) {
		bucket = Add(calendar, bucket, interval_t {-bucket_width_months, 0, 0});
	}
	return bucket;
}

int64_t BucketCommon(BucketWidthType type, const interval_t &width, int64_t ts, int64_t origin, Cal *calendar) {
	switch (type) {
	case BucketWidthType::CONVERTIBLE_TO_MICROS:
		return WidthConvertibleToMicrosCommon(width.micros, ts, origin, calendar);
	case BucketWidthType::CONVERTIBLE_TO_DAYS:
		return WidthConvertibleToDaysCommon(width.days, ts, origin, calendar);
	default:
		return WidthConvertibleToMonthsCommon(width.months, ts, origin, calendar);
	}
}

int64_t DefaultOrigin(BucketWidthType type) {
	return type == BucketWidthType::CONVERTIBLE_TO_MONTHS ? DEFAULT_ORIGIN_MICROS_2 : DEFAULT_ORIGIN_MICROS_1;
}

interval_t ToEngine(const IntervalValue &value) {
	return interval_t {value.months, value.days, value.micros};
}

} // namespace

//----------------------------------------------------------------------------------------------------------------------
// date_trunc
//----------------------------------------------------------------------------------------------------------------------
int ParseTruncPart(const std::string &part) {
	return Guard([&]() {
		const auto specifier = duckdb::GetDatePartSpecifier(part);
		// Rejects the specifiers that cannot be truncated to
		TruncationFactory(specifier);
		return static_cast<int>(specifier);
	});
}

int64_t DateTrunc(Calendar &calendar, int part, int64_t instant) {
	return Guard([&]() {
		auto cal = Native(calendar);
		const auto specifier = static_cast<DatePartSpecifier>(part);
		WeekSettings restore(cal);
		return TruncateTo(cal, TruncationFactory(specifier), instant);
	});
}

//----------------------------------------------------------------------------------------------------------------------
// make_timestamptz
//----------------------------------------------------------------------------------------------------------------------
int64_t MakeTimestampTZ(Calendar &calendar, int64_t yyyy, int64_t mm, int64_t dd, int64_t hr, int64_t mn, double ss) {
	return Guard([&]() {
		auto cal = Native(calendar);
		const auto year = duckdb::Cast::Operation<int64_t, int32_t>(
		    duckdb::AddOperator::Operation<int64_t, int64_t, int64_t>(yyyy, (yyyy < 0)));
		const auto month = duckdb::Cast::Operation<int64_t, int32_t>(
		    duckdb::SubtractOperatorOverflowCheck::Operation<int64_t, int64_t, int64_t>(mm, 1));
		const auto day = duckdb::Cast::Operation<int64_t, int32_t>(dd);
		const auto hour = duckdb::Cast::Operation<int64_t, int32_t>(hr);
		const auto min = duckdb::Cast::Operation<int64_t, int32_t>(mn);

		const auto secs = duckdb::Cast::Operation<double, int32_t>(ss);
		ss -= secs;
		ss *= duckdb::Interval::MSECS_PER_SEC;
		const auto millis = int32_t(ss);
		int64_t micros =
		    duckdb::LossyNumericCast<int64_t, double>(std::round((ss - millis) * duckdb::Interval::MICROS_PER_MSEC));

		// Start from the present, so a year is read in the current era of the calendar
		cal->SetTime(Cal::GetNow());
		cal->Set(CAL_YEAR, year);
		cal->Set(CAL_MONTH, month);
		cal->Set(CAL_DATE, day);
		cal->Set(CAL_HOUR_OF_DAY, hour);
		cal->Set(CAL_MINUTE, min);
		cal->Set(CAL_SECOND, secs);
		cal->Set(CAL_MILLISECOND, millis);

		return GetTime(cal, micros);
	});
}

//----------------------------------------------------------------------------------------------------------------------
// Casts
//----------------------------------------------------------------------------------------------------------------------
bool InstantToDate(Calendar &calendar, int64_t instant, int32_t &days, std::string &error) {
	try {
		return Guard([&]() {
			auto cal = Native(calendar);
			SetTime(cal, instant);
			const auto era = ExtractField(cal, CAL_ERA);
			const auto year = ExtractField(cal, CAL_YEAR);
			const auto mm = ExtractField(cal, CAL_MONTH) + 1;
			const auto dd = ExtractField(cal, CAL_DATE);

			const auto yyyy = era ? year : (-year + 1);
			date_t result;
			if (!duckdb::Date::TryFromDate(yyyy, mm, dd, result)) {
				error = "Unable to convert TIMESTAMPTZ to DATE";
				return false;
			}
			days = result.days;
			return true;
		});
	} catch (KernelError &ex) {
		error = ex.what();
		return false;
	}
}

bool InstantToTimeTZ(Calendar &calendar, int64_t instant, int64_t &time_micros, int32_t &offset, std::string &error) {
	try {
		return Guard([&]() {
			auto cal = Native(calendar);
			// Time in the time zone of the calendar
			auto micros = int32_t(SetTime(cal, instant));
			const auto hour = ExtractField(cal, CAL_HOUR_OF_DAY);
			const auto minute = ExtractField(cal, CAL_MINUTE);
			const auto second = ExtractField(cal, CAL_SECOND);
			const auto millis = ExtractField(cal, CAL_MILLISECOND);
			micros += millis * int32_t(MICROS_PER_MSEC);
			if (!duckdb::Time::IsValidTime(hour, minute, second, micros)) {
				error.clear();
				return false;
			}
			time_micros = duckdb::Time::FromTime(hour, minute, second, micros).value;

			// Offset in the time zone of the calendar
			if (!TryTimeTZOffset(cal, offset)) {
				error = TimeTZOffsetError(offset);
				return false;
			}
			return true;
		});
	} catch (KernelError &ex) {
		error = ex.what();
		return false;
	}
}

bool CurrentTimeTZOffset(Calendar &calendar, int32_t &offset, std::string &error) {
	try {
		return Guard([&]() {
			auto cal = Native(calendar);
			cal->SetTime(Cal::GetNow());
			if (!TryTimeTZOffset(cal, offset)) {
				error = TimeTZOffsetError(offset);
				return false;
			}
			return true;
		});
	} catch (KernelError &ex) {
		error = ex.what();
		return false;
	}
}

//----------------------------------------------------------------------------------------------------------------------
// time_bucket
//----------------------------------------------------------------------------------------------------------------------
int64_t TimeBucket(Calendar &calendar, const IntervalValue &width_p, int64_t ts) {
	return Guard([&]() {
		const auto width = ToEngine(width_p);
		const auto type = ClassifyBucketWidth(width, width_p.constant);
		if (!timestamp_t(ts).IsFinite()) {
			return ts;
		}
		return BucketCommon(type, width, ts, DefaultOrigin(type), Native(calendar));
	});
}

int64_t TimeBucketOffset(Calendar &calendar, const IntervalValue &width_p, int64_t ts, const IntervalValue &offset_p) {
	return Guard([&]() {
		const auto width = ToEngine(width_p);
		const auto offset = ToEngine(offset_p);
		const auto type = ClassifyBucketWidth(width, width_p.constant);
		if (!timestamp_t(ts).IsFinite()) {
			return ts;
		}
		auto cal = Native(calendar);
		const auto shifted = Sub(cal, ts, offset);
		return Add(cal, BucketCommon(type, width, shifted, DefaultOrigin(type), cal), offset);
	});
}

int64_t TimeBucketOrigin(Calendar &calendar, const IntervalValue &width_p, int64_t ts, int64_t origin) {
	return Guard([&]() {
		const auto width = ToEngine(width_p);
		const auto type = ClassifyBucketWidth(width, width_p.constant);
		if (!timestamp_t(ts).IsFinite()) {
			return ts;
		}
		return BucketCommon(type, width, ts, origin, Native(calendar));
	});
}

int64_t TimeBucketZone(Calendar &calendar, const IntervalValue &width_p, int64_t ts) {
	return Guard([&]() {
		const auto width = ToEngine(width_p);
		const auto type = ClassifyBucketWidth(width, width_p.constant);
		auto cal = Native(calendar);
		const auto origin = FromNaive(cal, DefaultOrigin(type));
		if (!timestamp_t(ts).IsFinite()) {
			return ts;
		}
		return BucketCommon(type, width, ts, origin, cal);
	});
}

} // namespace icu_bridge
