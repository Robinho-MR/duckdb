#include "icu_bridge_range.hpp"

#include "icu_bridge_internal.hpp"

#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace icu_bridge {

using duckdb::Interval;
using duckdb::datetime::CAL_DATE;
using duckdb::datetime::CAL_HOUR;
using duckdb::datetime::CAL_MILLISECOND;
using duckdb::datetime::CAL_MINUTE;
using duckdb::datetime::CAL_MONTH;
using duckdb::datetime::CAL_SECOND;

static void CalendarAddHour(duckdb::datetime::Calendar &calendar, int64_t hours) {
	constexpr int64_t MAX = std::numeric_limits<int32_t>::max();
	constexpr int64_t MIN = std::numeric_limits<int32_t>::min();
	if (hours >= 0) {
		while (hours > 0) {
			calendar.Add(CAL_HOUR, hours > MAX ? static_cast<int32_t>(MAX) : static_cast<int32_t>(hours));
			hours -= MAX;
		}
	} else {
		while (hours < 0) {
			calendar.Add(CAL_HOUR, hours < MIN ? static_cast<int32_t>(MIN) : static_cast<int32_t>(hours));
			hours -= MIN;
		}
	}
}

bool RangeAddInterval(Calendar &bridge_calendar, int64_t instant, int32_t months, int32_t days, int64_t interval_micros,
                      int64_t &result) {
	if (instant == std::numeric_limits<int64_t>::max() || instant == -std::numeric_limits<int64_t>::max()) {
		result = instant;
		return true;
	}
	auto &calendar = CalendarAccess::Native(bridge_calendar);

	int64_t millis = instant / Interval::MICROS_PER_MSEC;
	int64_t micros = instant % Interval::MICROS_PER_MSEC;

	// Move the microseconds manually
	micros += interval_micros % Interval::MICROS_PER_MSEC;
	if (micros >= Interval::MICROS_PER_MSEC) {
		micros -= Interval::MICROS_PER_MSEC;
		++millis;
	} else if (micros < 0) {
		micros += Interval::MICROS_PER_MSEC;
		--millis;
	}

	// The calendar adds the other parts
	calendar.SetTime(millis);

	// Break the units apart to avoid overflow
	auto interval_h = interval_micros / Interval::MICROS_PER_MSEC;

	const auto interval_ms = static_cast<int32_t>(interval_h % Interval::MSECS_PER_SEC);
	interval_h /= Interval::MSECS_PER_SEC;

	const auto interval_s = static_cast<int32_t>(interval_h % Interval::SECS_PER_MINUTE);
	interval_h /= Interval::SECS_PER_MINUTE;

	const auto interval_m = static_cast<int32_t>(interval_h % Interval::MINS_PER_HOUR);
	interval_h /= Interval::MINS_PER_HOUR;

	if (months < 0 || days < 0 || interval_micros < 0) {
		// Negative: add the fields from lowest to highest
		calendar.Add(CAL_MILLISECOND, interval_ms);
		calendar.Add(CAL_SECOND, interval_s);
		calendar.Add(CAL_MINUTE, interval_m);
		CalendarAddHour(calendar, interval_h);

		// Months are added before days
		calendar.Add(CAL_MONTH, months);
		calendar.Add(CAL_DATE, days);
	} else {
		calendar.Add(CAL_MONTH, months);
		calendar.Add(CAL_DATE, days);

		CalendarAddHour(calendar, interval_h);
		calendar.Add(CAL_MINUTE, interval_m);
		calendar.Add(CAL_SECOND, interval_s);
		calendar.Add(CAL_MILLISECOND, interval_ms);
	}

	const auto new_millis = calendar.GetTime();
	if (calendar.HasFailed()) {
		return false;
	}
	if (!duckdb::Timestamp::TryCombineUnits(new_millis, micros, Interval::MICROS_PER_MSEC, result)) {
		return false;
	}
	return result != std::numeric_limits<int64_t>::max() && result != -std::numeric_limits<int64_t>::max();
}

} // namespace icu_bridge
