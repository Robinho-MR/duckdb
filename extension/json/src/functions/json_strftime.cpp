// strftime for the date/timestamp formats of COPY ... TO ... (FORMAT JSON). See json_strftime.hpp.

#include "json_strftime.hpp"

namespace duckdb {
namespace json {

static const char *const MONTH_NAMES[] = {"January", "February", "March",     "April",   "May",      "June",
                                          "July",    "August",   "September", "October", "November", "December"};
static const char *const MONTH_NAMES_ABBREVIATED[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const DAY_NAMES[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const DAY_NAMES_ABBREVIATED[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static constexpr int64_t NANOS_PER_DAY = 86400LL * 1000000000LL;

static int64_t FloorDiv(int64_t a, int64_t b) {
	auto q = a / b;
	if ((a % b != 0) && ((a < 0) != (b < 0))) {
		q--;
	}
	return q;
}

static int64_t FloorMod(int64_t a, int64_t b) {
	return a - FloorDiv(a, b) * b;
}

//! Days since 1970-01-01 of a civil date (proleptic Gregorian, astronomical year numbering)
static int64_t DaysFromCivil(int64_t y, int64_t m, int64_t d) {
	y -= m <= 2;
	const auto era = FloorDiv(y, 400);
	const auto yoe = y - era * 400;
	const auto doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const auto doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

static void CivilFromDays(int64_t z, int64_t &y, int64_t &m, int64_t &d) {
	z += 719468;
	const auto era = FloorDiv(z, 146097);
	const auto doe = z - era * 146097;
	const auto yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	y = yoe + era * 400;
	const auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const auto mp = (5 * doy + 2) / 153;
	d = doy - (153 * mp + 2) / 5 + 1;
	m = mp < 10 ? mp + 3 : mp - 9;
	y += m <= 2;
}

//! 1 (Monday) to 7 (Sunday)
static int64_t ISODayOfTheWeek(int64_t days) {
	return FloorMod(days + 3, 7) + 1;
}

static int64_t WeeksInISOYear(int64_t year) {
	auto p = [](int64_t y) {
		return FloorMod(y + FloorDiv(y, 4) - FloorDiv(y, 100) + FloorDiv(y, 400), 7);
	};
	return (p(year) == 4 || p(year - 1) == 3) ? 53 : 52;
}

static void ISOYearWeek(int64_t days, int64_t year, int64_t doy, int64_t &iso_year, int64_t &iso_week) {
	auto week = (doy - ISODayOfTheWeek(days) + 10) / 7;
	iso_year = year;
	if (week < 1) {
		iso_year = year - 1;
		week = WeeksInISOYear(iso_year);
	} else if (week > WeeksInISOYear(year)) {
		iso_year = year + 1;
		week = 1;
	}
	iso_week = week;
}

static int64_t WeekNumberRegular(int64_t days, int64_t year, int64_t day_of_year_zero_based, bool monday_first) {
	const auto day_of_jan_first = ISODayOfTheWeek(DaysFromCivil(year, 1, 1));
	int64_t first_week_start;
	if (monday_first) {
		first_week_start = day_of_jan_first == 1 ? 0 : 8 - day_of_jan_first;
	} else {
		first_week_start = 7 - day_of_jan_first;
	}
	if (day_of_year_zero_based < first_week_start) {
		return 0;
	}
	return ((day_of_year_zero_based - first_week_start) / 7) + 1;
}

static void AppendPadded(string &target, int64_t value, int width) {
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%0*lld", width, static_cast<long long>(value));
	target += buffer;
}

static void AppendNumber(string &target, int64_t value) {
	target += std::to_string(value);
}

static void AppendYear(string &target, int64_t year) {
	if (year >= 0 && year <= 9999) {
		AppendPadded(target, year, 4);
	} else {
		AppendNumber(target, year);
	}
}

void JSONTimeFormat::AddSpecifier(string literal, Specifier specifier) {
	literals.push_back(std::move(literal));
	specifiers.push_back(specifier);
}

string JSONTimeFormat::TryParse(const string &format, JSONTimeFormat &result) {
	if (format.empty()) {
		return "Empty format string";
	}
	result.format_string = format;
	result.specifiers.clear();
	result.literals.clear();
	size_t pos = 0;
	string current_literal;
	for (size_t i = 0; i < format.size(); i++) {
		if (format[i] != '%') {
			continue;
		}
		if (i + 1 == format.size()) {
			return "Trailing format character %";
		}
		if (i > pos) {
			current_literal += format.substr(pos, i - pos);
		}
		char c = format[++i];
		if (c == '%') {
			pos = i;
			continue;
		}
		Specifier specifier;
		if (c == '-' && i + 1 < format.size()) {
			c = format[++i];
			switch (c) {
			case 'd':
				specifier = Specifier::DAY_OF_MONTH;
				break;
			case 'm':
				specifier = Specifier::MONTH_DECIMAL;
				break;
			case 'y':
				specifier = Specifier::YEAR_WITHOUT_CENTURY;
				break;
			case 'H':
				specifier = Specifier::HOUR_24_DECIMAL;
				break;
			case 'I':
				specifier = Specifier::HOUR_12_DECIMAL;
				break;
			case 'M':
				specifier = Specifier::MINUTE_DECIMAL;
				break;
			case 'S':
				specifier = Specifier::SECOND_DECIMAL;
				break;
			case 'n':
				specifier = Specifier::NANOSECOND_PADDED;
				break;
			case 'f':
				specifier = Specifier::MICROSECOND_PADDED;
				break;
			case 'g':
				specifier = Specifier::MILLISECOND_PADDED;
				break;
			case 'z':
				specifier = Specifier::UTC_OFFSET;
				break;
			case 'Z':
				specifier = Specifier::TZ_NAME;
				break;
			case 'j':
				specifier = Specifier::DAY_OF_YEAR_DECIMAL;
				break;
			case 'U':
				specifier = Specifier::WEEK_NUMBER_PADDED_SUN_FIRST;
				break;
			case 'W':
				specifier = Specifier::WEEK_NUMBER_PADDED_MON_FIRST;
				break;
			case 'V':
				specifier = Specifier::WEEK_NUMBER_ISO;
				break;
			case 'c':
			case 'x':
			case 'X':
			case 'T': {
				const char *sub = c == 'c' ? "%Y-%m-%d %H:%M:%S" : (c == 'x' ? "%Y-%m-%d" : "%H:%M:%S");
				JSONTimeFormat locale;
				auto error = TryParse(sub, locale);
				if (!error.empty()) {
					throw InternalException("Failed to bind sub-format specifier \"%s\": %s", sub, error);
				}
				locale.literals[0] = std::move(current_literal) + locale.literals[0];
				current_literal = "";
				for (size_t k = 0; k < locale.specifiers.size(); k++) {
					result.AddSpecifier(std::move(locale.literals[k]), locale.specifiers[k]);
				}
				// the literal after the sub-format carries on as the current literal
				current_literal = locale.literals.back();
				pos = i + 1;
				continue;
			}
			default:
				return "Unrecognized format for strftime/strptime: %-" + string(1, c);
			}
		} else {
			switch (c) {
			case 'a':
				specifier = Specifier::ABBREVIATED_WEEKDAY_NAME;
				break;
			case 'A':
				specifier = Specifier::FULL_WEEKDAY_NAME;
				break;
			case 'w':
				specifier = Specifier::WEEKDAY_DECIMAL;
				break;
			case 'u':
				specifier = Specifier::WEEKDAY_ISO;
				break;
			case 'd':
				specifier = Specifier::DAY_OF_MONTH_PADDED;
				break;
			case 'h':
			case 'b':
				specifier = Specifier::ABBREVIATED_MONTH_NAME;
				break;
			case 'B':
				specifier = Specifier::FULL_MONTH_NAME;
				break;
			case 'm':
				specifier = Specifier::MONTH_DECIMAL_PADDED;
				break;
			case 'y':
				specifier = Specifier::YEAR_WITHOUT_CENTURY_PADDED;
				break;
			case 'Y':
				specifier = Specifier::YEAR_DECIMAL;
				break;
			case 'G':
				specifier = Specifier::YEAR_ISO;
				break;
			case 'H':
				specifier = Specifier::HOUR_24_PADDED;
				break;
			case 'I':
				specifier = Specifier::HOUR_12_PADDED;
				break;
			case 'p':
				specifier = Specifier::AM_PM;
				break;
			case 'M':
				specifier = Specifier::MINUTE_PADDED;
				break;
			case 'S':
				specifier = Specifier::SECOND_PADDED;
				break;
			case 'n':
				specifier = Specifier::NANOSECOND_PADDED;
				break;
			case 'f':
				specifier = Specifier::MICROSECOND_PADDED;
				break;
			case 'g':
				specifier = Specifier::MILLISECOND_PADDED;
				break;
			case 'z':
				specifier = Specifier::UTC_OFFSET;
				break;
			case 'Z':
				specifier = Specifier::TZ_NAME;
				break;
			case 'j':
				specifier = Specifier::DAY_OF_YEAR_PADDED;
				break;
			case 'U':
				specifier = Specifier::WEEK_NUMBER_PADDED_SUN_FIRST;
				break;
			case 'W':
				specifier = Specifier::WEEK_NUMBER_PADDED_MON_FIRST;
				break;
			case 'V':
				specifier = Specifier::WEEK_NUMBER_ISO;
				break;
			case 'c':
			case 'x':
			case 'X':
			case 'T': {
				const char *sub = c == 'c' ? "%Y-%m-%d %H:%M:%S" : (c == 'x' ? "%Y-%m-%d" : "%H:%M:%S");
				JSONTimeFormat locale;
				auto error = TryParse(sub, locale);
				if (!error.empty()) {
					throw InternalException("Failed to bind sub-format specifier \"%s\": %s", sub, error);
				}
				locale.literals[0] = std::move(current_literal) + locale.literals[0];
				for (size_t k = 0; k < locale.specifiers.size(); k++) {
					result.AddSpecifier(std::move(locale.literals[k]), locale.specifiers[k]);
				}
				current_literal = locale.literals.back();
				pos = i + 1;
				continue;
			}
			default:
				return "Unrecognized format for strftime/strptime: %" + string(1, c);
			}
		}
		result.AddSpecifier(std::move(current_literal), specifier);
		current_literal = "";
		pos = i + 1;
	}
	if (pos < format.size()) {
		current_literal += format.substr(pos);
	}
	result.literals.push_back(std::move(current_literal));
	return string();
}

JSONTimeFormat JSONTimeFormat::Parse(const string &format, const string &name) {
	JSONTimeFormat result;
	auto error = TryParse(format, result);
	if (!error.empty()) {
		throw InvalidInputException("Failed to parse %s format specifier %s: %s", name, format, error);
	}
	return result;
}

string JSONTimeFormat::Format(int32_t days_p, int64_t nanos_of_day) const {
	const int64_t days = days_p;
	int64_t year, month, day;
	CivilFromDays(days, year, month, day);
	const auto hour = nanos_of_day / (3600LL * 1000000000LL);
	const auto minute = (nanos_of_day / (60LL * 1000000000LL)) % 60;
	const auto second = (nanos_of_day / 1000000000LL) % 60;
	const auto nanos = nanos_of_day % 1000000000LL;
	const auto doy0 = days - DaysFromCivil(year, 1, 1);

	string result;
	for (size_t i = 0; i < specifiers.size(); i++) {
		result += literals[i];
		switch (specifiers[i]) {
		case Specifier::ABBREVIATED_WEEKDAY_NAME:
			result += DAY_NAMES_ABBREVIATED[ISODayOfTheWeek(days) % 7];
			break;
		case Specifier::FULL_WEEKDAY_NAME:
			result += DAY_NAMES[ISODayOfTheWeek(days) % 7];
			break;
		case Specifier::WEEKDAY_DECIMAL:
			AppendNumber(result, ISODayOfTheWeek(days) % 7);
			break;
		case Specifier::WEEKDAY_ISO:
			AppendNumber(result, ISODayOfTheWeek(days));
			break;
		case Specifier::DAY_OF_MONTH_PADDED:
			AppendPadded(result, day, 2);
			break;
		case Specifier::DAY_OF_MONTH:
			AppendNumber(result, day);
			break;
		case Specifier::ABBREVIATED_MONTH_NAME:
			result += MONTH_NAMES_ABBREVIATED[month - 1];
			break;
		case Specifier::FULL_MONTH_NAME:
			result += MONTH_NAMES[month - 1];
			break;
		case Specifier::MONTH_DECIMAL_PADDED:
			AppendPadded(result, month, 2);
			break;
		case Specifier::MONTH_DECIMAL:
			AppendNumber(result, month);
			break;
		case Specifier::YEAR_WITHOUT_CENTURY_PADDED:
			AppendPadded(result, (year < 0 ? -year : year) % 100, 2);
			break;
		case Specifier::YEAR_WITHOUT_CENTURY:
			AppendNumber(result, (year < 0 ? -year : year) % 100);
			break;
		case Specifier::YEAR_DECIMAL:
			AppendYear(result, year);
			break;
		case Specifier::YEAR_ISO: {
			int64_t iso_year, iso_week;
			ISOYearWeek(days, year, doy0 + 1, iso_year, iso_week);
			AppendYear(result, iso_year);
			break;
		}
		case Specifier::HOUR_24_PADDED:
			AppendPadded(result, hour, 2);
			break;
		case Specifier::HOUR_24_DECIMAL:
			AppendNumber(result, hour);
			break;
		case Specifier::HOUR_12_PADDED:
			AppendPadded(result, hour % 12 == 0 ? 12 : hour % 12, 2);
			break;
		case Specifier::HOUR_12_DECIMAL:
			AppendNumber(result, hour % 12 == 0 ? 12 : hour % 12);
			break;
		case Specifier::AM_PM:
			result += hour >= 12 ? "PM" : "AM";
			break;
		case Specifier::MINUTE_PADDED:
			AppendPadded(result, minute, 2);
			break;
		case Specifier::MINUTE_DECIMAL:
			AppendNumber(result, minute);
			break;
		case Specifier::SECOND_PADDED:
			AppendPadded(result, second, 2);
			break;
		case Specifier::SECOND_DECIMAL:
			AppendNumber(result, second);
			break;
		case Specifier::NANOSECOND_PADDED:
			AppendPadded(result, nanos, 9);
			break;
		case Specifier::MICROSECOND_PADDED:
			AppendPadded(result, nanos / 1000, 6);
			break;
		case Specifier::MILLISECOND_PADDED:
			AppendPadded(result, nanos / 1000000, 3);
			break;
		case Specifier::UTC_OFFSET:
			result += "+00";
			break;
		case Specifier::TZ_NAME:
			break;
		case Specifier::DAY_OF_YEAR_PADDED:
			AppendPadded(result, doy0 + 1, 3);
			break;
		case Specifier::DAY_OF_YEAR_DECIMAL:
			AppendNumber(result, doy0 + 1);
			break;
		case Specifier::WEEK_NUMBER_PADDED_SUN_FIRST:
			AppendPadded(result, WeekNumberRegular(days, year, doy0, false), 2);
			break;
		case Specifier::WEEK_NUMBER_PADDED_MON_FIRST:
			AppendPadded(result, WeekNumberRegular(days, year, doy0, true), 2);
			break;
		case Specifier::WEEK_NUMBER_ISO: {
			int64_t iso_year, iso_week;
			ISOYearWeek(days, year, doy0 + 1, iso_year, iso_week);
			AppendPadded(result, iso_week, 2);
			break;
		}
		}
	}
	result += literals.back();
	return result;
}

string JSONTimeFormat::FormatDate(int32_t days) const {
	return Format(days, 0);
}

string JSONTimeFormat::FormatTimestampMicros(int64_t micros) const {
	const auto days = FloorDiv(micros, 86400LL * 1000000LL);
	const auto micros_of_day = micros - days * 86400LL * 1000000LL;
	return Format(static_cast<int32_t>(days), micros_of_day * 1000);
}

string JSONTimeFormat::FormatTimestampNanos(int64_t nanos) const {
	const auto days = FloorDiv(nanos, NANOS_PER_DAY);
	return Format(static_cast<int32_t>(days), nanos - days * NANOS_PER_DAY);
}

} // namespace json
} // namespace duckdb
