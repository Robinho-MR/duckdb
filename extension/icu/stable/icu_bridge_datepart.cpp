#include "icu_bridge_datepart.hpp"
#include "icu_bridge_internal.hpp"

#include "calendar.hpp"
#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/common/types/date.hpp"

namespace icu_bridge {

using namespace duckdb::datetime;

namespace {

constexpr int64_t MICROS_PER_MILLI = 1000;
constexpr int64_t MILLIS_PER_SECOND = 1000;
constexpr int64_t MICROS_PER_MINUTE = 60LL * 1000000LL;
constexpr int64_t MICROS_PER_DAY = 86400LL * 1000000LL;
constexpr int64_t SECONDS_PER_HOUR = 3600;
constexpr int64_t SECONDS_PER_MINUTE = 60;
constexpr int64_t MONTHS_PER_QUARTER = 3;

//! Thrown inside the kernels when the calendar cannot represent the instant
struct ExtractFailure {};

//! A calendar positioned at an instant
class Extractor {
public:
	Extractor(Calendar &calendar, int64_t instant) : cal(CalendarAccess::Native(calendar)) {
		auto millis = instant / MICROS_PER_MILLI;
		micros = instant % MICROS_PER_MILLI;
		if (micros < 0) {
			--millis;
			micros += MICROS_PER_MILLI;
		}
		cal.SetTime(millis);
	}

	int64_t Field(CalendarField field) {
		const auto result = cal.Get(field);
		if (cal.HasFailed()) {
			throw ExtractFailure();
		}
		return result;
	}

	void UseIsoWeeks() {
		cal.SetFirstDayOfWeek(CAL_MONDAY);
		cal.SetMinimalDaysInFirstWeek(4);
	}

	int64_t Era() {
		return Field(CAL_ERA);
	}
	int64_t Year() {
		return Field(CAL_YEAR);
	}
	int64_t Month() {
		return Field(CAL_MONTH) + 1;
	}
	int64_t Second() {
		return Field(CAL_SECOND);
	}
	int64_t Millisecond() {
		return Second() * MILLIS_PER_SECOND + Field(CAL_MILLISECOND);
	}
	int64_t Microsecond() {
		return Millisecond() * MICROS_PER_MILLI + micros;
	}
	int64_t DayOfWeek() {
		// [Sun(0), Sat(6)]
		return Field(CAL_DAY_OF_WEEK) - CAL_SUNDAY;
	}
	int64_t ZoneOffsetSeconds() {
		return (Field(CAL_ZONE_OFFSET) + Field(CAL_DST_OFFSET)) / MILLIS_PER_SECOND;
	}

	int64_t Bigint(DatePart part) {
		switch (part) {
		case DatePart::YEAR:
			return Year();
		case DatePart::MONTH:
			return Month();
		case DatePart::DAY:
			return Field(CAL_DATE);
		case DatePart::DECADE:
			return Year() / 10;
		case DatePart::CENTURY: {
			const auto cccc = ((Year() - 1) / 100) + 1;
			return Era() > 0 ? cccc : -cccc;
		}
		case DatePart::MILLENNIUM: {
			const auto mmmm = ((Year() - 1) / 1000) + 1;
			return Era() > 0 ? mmmm : -mmmm;
		}
		case DatePart::MICROSECONDS:
			return Microsecond();
		case DatePart::MILLISECONDS:
			return Millisecond();
		case DatePart::SECOND:
			return Second();
		case DatePart::MINUTE:
			return Field(CAL_MINUTE);
		case DatePart::HOUR:
			return Field(CAL_HOUR_OF_DAY);
		case DatePart::DOW:
			return DayOfWeek();
		case DatePart::ISODOW:
			// [Mon(1), Sun(7)]
			return 1 + (Field(CAL_DAY_OF_WEEK) + 7 - CAL_MONDAY) % 7;
		case DatePart::WEEK:
			UseIsoWeeks();
			return Field(CAL_WEEK_OF_YEAR);
		case DatePart::ISOYEAR:
			UseIsoWeeks();
			return Field(CAL_YEAR_WOY);
		case DatePart::QUARTER:
			return Field(CAL_MONTH) / MONTHS_PER_QUARTER + 1;
		case DatePart::DOY:
			return Field(CAL_DAY_OF_YEAR);
		case DatePart::YEARWEEK: {
			UseIsoWeeks();
			const auto iyyy = Field(CAL_YEAR_WOY);
			const auto ww = Field(CAL_WEEK_OF_YEAR);
			return iyyy * 100 + ((iyyy > 0) ? ww : -ww);
		}
		case DatePart::ERA:
			return Era();
		case DatePart::TIMEZONE:
			return ZoneOffsetSeconds();
		case DatePart::TIMEZONE_HOUR:
			return ZoneOffsetSeconds() / SECONDS_PER_HOUR;
		case DatePart::TIMEZONE_MINUTE:
			return (ZoneOffsetSeconds() % SECONDS_PER_HOUR) / SECONDS_PER_MINUTE;
		default:
			throw ExtractFailure();
		}
	}

	double Double(DatePart part) {
		switch (part) {
		case DatePart::EPOCH: {
			// the milliseconds carry the fraction of a second, so the division has to keep it
			auto result = double(cal.GetTime()) / double(MILLIS_PER_SECOND);
			result += static_cast<double>(micros) / 1000000.0;
			return result;
		}
		case DatePart::JULIAN_DAY: {
			// PG uses doubles for JDs: days + fraction
			const auto days = Field(CAL_JULIAN_DAY);
			auto frac = Field(CAL_HOUR_OF_DAY);
			frac *= 60;
			frac += Field(CAL_MINUTE);
			frac *= MICROS_PER_MINUTE;
			frac += Microsecond();
			return static_cast<double>(frac) / double(MICROS_PER_DAY) + double(days);
		}
		default:
			throw ExtractFailure();
		}
	}

	int32_t LastDay() {
		// Set the calendar to midnight on the last day of the month
		cal.Set(CAL_MILLISECOND, 0);
		cal.Set(CAL_SECOND, 0);
		cal.Set(CAL_MINUTE, 0);
		cal.Set(CAL_HOUR_OF_DAY, 0);

		const auto dd = cal.GetActualMaximum(CAL_DATE);
		if (cal.HasFailed()) {
			throw ExtractFailure();
		}
		cal.Set(CAL_DATE, dd);

		// Offset to UTC
		auto millis = cal.GetTime();
		millis += Field(CAL_ZONE_OFFSET);
		millis += Field(CAL_DST_OFFSET);
		return duckdb::Date::EpochToDate(millis / MILLIS_PER_SECOND).days;
	}

	std::string_view MonthName() {
		const auto mm = Month() - 1;
		if (mm == 12) {
			return "Undecimber";
		}
		const auto &name = duckdb::Date::MONTH_NAMES[mm];
		return std::string_view(name.GetData(), name.GetSize());
	}

	std::string_view DayName() {
		const auto &name = duckdb::Date::DAY_NAMES[DayOfWeek()];
		return std::string_view(name.GetData(), name.GetSize());
	}

private:
	duckdb::datetime::Calendar &cal;
	int64_t micros;
};

} // namespace

bool TryParseDatePart(const std::string &name, DatePart &part) {
	duckdb::DatePartSpecifier specifier;
	if (!duckdb::TryGetDatePartSpecifier(name, specifier) || specifier == duckdb::DatePartSpecifier::INVALID) {
		return false;
	}
	// the enums list the same parts in the same order
	part = static_cast<DatePart>(static_cast<uint8_t>(specifier));
	return true;
}

bool ExtractBigintPart(Calendar &calendar, DatePart part, int64_t instant, int64_t &result) {
	try {
		result = Extractor(calendar, instant).Bigint(part);
		return true;
	} catch (const ExtractFailure &) {
		return false;
	}
}

bool ExtractDoublePart(Calendar &calendar, DatePart part, int64_t instant, double &result) {
	try {
		result = Extractor(calendar, instant).Double(part);
		return true;
	} catch (const ExtractFailure &) {
		return false;
	}
}

bool ExtractLastDay(Calendar &calendar, int64_t instant, int32_t &days) {
	try {
		days = Extractor(calendar, instant).LastDay();
		return true;
	} catch (const ExtractFailure &) {
		return false;
	}
}

bool ExtractMonthName(Calendar &calendar, int64_t instant, std::string_view &name) {
	try {
		name = Extractor(calendar, instant).MonthName();
		return true;
	} catch (const ExtractFailure &) {
		return false;
	}
}

bool ExtractDayName(Calendar &calendar, int64_t instant, std::string_view &name) {
	try {
		name = Extractor(calendar, instant).DayName();
		return true;
	} catch (const ExtractFailure &) {
		return false;
	}
}

} // namespace icu_bridge
