#include "icu-datetime.hpp"

namespace duckdb_icu {

const char *const Date::MONTH_NAMES[12] = {"January", "February", "March",     "April",   "May",      "June",
                                           "July",    "August",   "September", "October", "November", "December"};
const char *const Date::DAY_NAMES[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const int32_t Date::NORMAL_DAYS[13] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
const int32_t Date::LEAP_DAYS[13] = {0, 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

namespace {

//! Writes the decimal digits of `value` backwards, ending at `end`, and returns where they start
char *FormatUnsigned(uint64_t value, char *end) {
	do {
		*--end = char('0' + value % 10);
		value /= 10;
	} while (value);
	return end;
}

idx_t UnsignedLength(uint64_t value) {
	idx_t length = 1;
	while (value >= 10) {
		value /= 10;
		length++;
	}
	return length;
}

void FormatSignedNumber(int64_t value, char buffer[], idx_t &length) {
	const bool negative = value < 0;
	const auto unsigned_value = negative ? uint64_t(0) - uint64_t(value) : uint64_t(value);
	length += UnsignedLength(unsigned_value) + negative;
	auto end = FormatUnsigned(unsigned_value, buffer + length);
	if (negative) {
		*--end = '-';
	}
}

void FormatIntervalValue(int32_t value, char buffer[], idx_t &length, const char *name, idx_t name_len) {
	if (value == 0) {
		return;
	}
	if (length != 0) {
		buffer[length++] = ' ';
	}
	FormatSignedNumber(value, buffer, length);
	memcpy(buffer + length, name, name_len);
	length += name_len;
	if (value != 1 && value != -1) {
		buffer[length++] = 's';
	}
}

} // namespace

//===--------------------------------------------------------------------===//
// Text
//===--------------------------------------------------------------------===//
idx_t DateToStringCast::YearLength(int32_t &year, idx_t &year_length, bool &add_bc) {
	// format is YYYY-MM-DD with optional (BC) at the end
	idx_t length = 6;
	year_length = 4;
	add_bc = false;
	if (year <= 0) {
		length += 5;
		year = -year + 1;
		add_bc = true;
	}
	year_length += year >= 10000;
	year_length += year >= 100000;
	year_length += year >= 1000000;
	year_length += year >= 10000000;
	length += year_length;
	return length;
}

static void FormatComponent(char *&ptr, int32_t number) {
	ptr[0] = '-';
	ptr[1] = char('0' + number / 10);
	ptr[2] = char('0' + number % 10);
	ptr += 3;
}

void DateToStringCast::Format(char *data, int32_t year, int32_t month, int32_t day, idx_t year_length, bool add_bc) {
	auto endptr = data + year_length;
	endptr = FormatUnsigned(uint64_t(year), endptr);
	while (endptr > data) {
		*--endptr = '0';
	}
	auto ptr = data + year_length;
	FormatComponent(ptr, month);
	FormatComponent(ptr, day);
	if (add_bc) {
		memcpy(ptr, " (BC)", 5);
	}
}

int32_t TimeToStringCast::FormatMicros(int32_t microseconds, char micro_buffer[], int32_t nanos) {
	idx_t buf_len = 6;
	if (nanos) {
		microseconds *= 1000;
		microseconds += nanos;
		buf_len += 3;
	}
	char *endptr = micro_buffer + buf_len;
	endptr = FormatUnsigned(uint64_t(microseconds), endptr);
	while (endptr > micro_buffer) {
		*--endptr = '0';
	}
	int32_t trailing_zeros = 0;
	for (idx_t i = buf_len - 1; i > 0; i--) {
		if (micro_buffer[i] != '0') {
			break;
		}
		trailing_zeros++;
	}
	return trailing_zeros;
}

idx_t TimeToStringCast::MicrosLength(int32_t micros, char micro_buffer[], int32_t nanos) {
	// format is HH:MM:DD.MS, without the trailing zeros of the fraction
	if (micros == 0 && nanos == 0) {
		return 8;
	}
	idx_t length = 15;
	if (nanos) {
		length += 3;
	}
	length -= idx_t(FormatMicros(micros, micro_buffer, nanos));
	return length;
}

void TimeToStringCast::FormatTwoDigits(char *ptr, int32_t value) {
	if (value < 0 || value > 99) {
		throw InternalException("FormatTwoDigits called with out-of-range value %d", value);
	}
	ptr[0] = char('0' + value / 10);
	ptr[1] = char('0' + value % 10);
}

void TimeToStringCast::Format(char *data, idx_t length, int32_t hour, int32_t minute, int32_t second,
                              char micro_buffer[]) {
	FormatTwoDigits(data, hour);
	data[2] = ':';
	FormatTwoDigits(data + 3, minute);
	data[5] = ':';
	FormatTwoDigits(data + 6, second);
	if (length > 8) {
		data[8] = '.';
		memcpy(data + 9, micro_buffer, length - 9);
	}
}

string Interval::ToString(const interval_t &interval) {
	char buffer[70];
	idx_t length = 0;
	if (interval.months != 0) {
		int32_t years = interval.months / 12;
		int32_t months = interval.months - years * 12;
		FormatIntervalValue(years, buffer, length, " year", 5);
		FormatIntervalValue(months, buffer, length, " month", 6);
	}
	if (interval.days != 0) {
		FormatIntervalValue(interval.days, buffer, length, " day", 4);
	}
	if (interval.micros != 0) {
		if (length != 0) {
			buffer[length++] = ' ';
		}
		int64_t micros = interval.micros;
		if (micros < 0) {
			buffer[length++] = '-';
		} else {
			micros = -micros;
		}
		int64_t hour = -(micros / Interval::MICROS_PER_HOUR);
		micros += hour * Interval::MICROS_PER_HOUR;
		int64_t min = -(micros / Interval::MICROS_PER_MINUTE);
		micros += min * Interval::MICROS_PER_MINUTE;
		int64_t sec = -(micros / Interval::MICROS_PER_SEC);
		micros += sec * Interval::MICROS_PER_SEC;
		micros = -micros;

		if (hour < 10) {
			buffer[length++] = '0';
		}
		FormatSignedNumber(hour, buffer, length);
		buffer[length++] = ':';
		TimeToStringCast::FormatTwoDigits(buffer + length, int32_t(min));
		length += 2;
		buffer[length++] = ':';
		TimeToStringCast::FormatTwoDigits(buffer + length, int32_t(sec));
		length += 2;
		if (micros != 0) {
			buffer[length++] = '.';
			auto trailing_zeros = TimeToStringCast::FormatMicros(int32_t(micros), buffer + length);
			length += idx_t(6 - trailing_zeros);
		}
	} else if (length == 0) {
		memcpy(buffer, "00:00:00", 8);
		return string(buffer, 8);
	}
	return string(buffer, length);
}

string Date::ToString(date_t date) {
	if (!IsFinite(date)) {
		return date.days == DATE_INFINITY ? "infinity" : "-infinity";
	}
	int32_t year, month, day;
	Date::Convert(date, year, month, day);
	idx_t year_length;
	bool add_bc;
	auto length = DateToStringCast::YearLength(year, year_length, add_bc);
	string result(length, '\0');
	DateToStringCast::Format(&result[0], year, month, day, year_length, add_bc);
	return result;
}

string Timestamp::ToString(timestamp_t timestamp) {
	if (!IsFinite(timestamp)) {
		return ToInfinity(timestamp);
	}
	date_t date;
	dtime_t time;
	Convert(timestamp, date, time);
	int32_t hour, minute, second, micros;
	Time::Convert(time, hour, minute, second, micros);
	char micro_buffer[10];
	auto length = TimeToStringCast::MicrosLength(micros, micro_buffer);
	string time_text(length, '\0');
	TimeToStringCast::Format(&time_text[0], length, hour, minute, second, micro_buffer);
	return Date::ToString(date) + " " + time_text;
}

string Time::ToUTCOffset(int hour_offset, int minute_offset, int second_offset) {
	int64_t micros =
	    ((hour_offset * Interval::MINS_PER_HOUR + minute_offset) * Interval::SECS_PER_MINUTE + second_offset) *
	    Interval::MICROS_PER_SEC;
	char buffer[1 + 2 + 1 + 2 + 1 + 2];
	idx_t length = 0;
	buffer[length++] = (micros < 0 ? '-' : '+');
	micros = std::abs(micros);

	dtime_t time;
	time.micros = micros;
	int32_t time_units[4];
	Time::Convert(time, time_units[0], time_units[1], time_units[2], time_units[3]);

	TimeToStringCast::FormatTwoDigits(buffer + length, time_units[0]);
	length += 2;
	if (time_units[1] || time_units[2]) {
		buffer[length++] = ':';
		TimeToStringCast::FormatTwoDigits(buffer + length, time_units[1]);
		length += 2;
	}
	if (time_units[2]) {
		buffer[length++] = ':';
		TimeToStringCast::FormatTwoDigits(buffer + length, time_units[2]);
		length += 2;
	}
	return string(buffer, length);
}

//===--------------------------------------------------------------------===//
// Age
//===--------------------------------------------------------------------===//
interval_t GetAge(const TimestampComponents &ts1, const TimestampComponents &ts2, bool is_negative) {
	auto year_diff = ts1.year - ts2.year;
	auto month_diff = ts1.month - ts2.month;
	auto day_diff = ts1.day - ts2.day;

	auto hour_diff = ts1.hour - ts2.hour;
	auto min_diff = ts1.minute - ts2.minute;
	auto sec_diff = ts1.second - ts2.second;
	auto micros_diff = ts1.microsecond - ts2.microsecond;

	bool sign_flipped = false;
	if (is_negative) {
		year_diff = -year_diff;
		month_diff = -month_diff;
		day_diff = -day_diff;
		hour_diff = -hour_diff;
		min_diff = -min_diff;
		sec_diff = -sec_diff;
		micros_diff = -micros_diff;
		sign_flipped = true;
	}
	// propagate any negative field into the next higher field
	while (micros_diff < 0) {
		micros_diff += Interval::MICROS_PER_SEC;
		sec_diff--;
	}
	while (sec_diff < 0) {
		sec_diff += Interval::SECS_PER_MINUTE;
		min_diff--;
	}
	while (min_diff < 0) {
		min_diff += Interval::MINS_PER_HOUR;
		hour_diff--;
	}
	while (hour_diff < 0) {
		hour_diff += Interval::HOURS_PER_DAY;
		day_diff--;
	}
	while (day_diff < 0) {
		if (is_negative) {
			day_diff += Date::IsLeapYear(ts1.year) ? Date::LEAP_DAYS[ts1.month] : Date::NORMAL_DAYS[ts1.month];
			month_diff--;
		} else {
			day_diff += Date::IsLeapYear(ts2.year) ? Date::LEAP_DAYS[ts2.month] : Date::NORMAL_DAYS[ts2.month];
			month_diff--;
		}
	}
	while (month_diff < 0) {
		month_diff += Interval::MONTHS_PER_YEAR;
		year_diff--;
	}

	if (sign_flipped) {
		year_diff = -year_diff;
		month_diff = -month_diff;
		day_diff = -day_diff;
		hour_diff = -hour_diff;
		min_diff = -min_diff;
		sec_diff = -sec_diff;
		micros_diff = -micros_diff;
	}
	interval_t interval;
	interval.months = year_diff * Interval::MONTHS_PER_YEAR + month_diff;
	interval.days = day_diff;
	interval.micros = Time::FromTime(hour_diff, min_diff, sec_diff, micros_diff).micros;
	return interval;
}

//===--------------------------------------------------------------------===//
// Date parts
//===--------------------------------------------------------------------===//
namespace {

struct DatePartName {
	const char *name;
	DatePartSpecifier part;
};

const DatePartName DATE_PART_NAMES[] = {
    {"year", DatePartSpecifier::YEAR},
    {"y", DatePartSpecifier::YEAR},
    {"yr", DatePartSpecifier::YEAR},
    {"yrs", DatePartSpecifier::YEAR},
    {"years", DatePartSpecifier::YEAR},
    {"month", DatePartSpecifier::MONTH},
    {"mon", DatePartSpecifier::MONTH},
    {"mons", DatePartSpecifier::MONTH},
    {"months", DatePartSpecifier::MONTH},
    {"day", DatePartSpecifier::DAY},
    {"d", DatePartSpecifier::DAY},
    {"days", DatePartSpecifier::DAY},
    {"dayofmonth", DatePartSpecifier::DAY},
    {"decade", DatePartSpecifier::DECADE},
    {"dec", DatePartSpecifier::DECADE},
    {"decs", DatePartSpecifier::DECADE},
    {"decades", DatePartSpecifier::DECADE},
    {"century", DatePartSpecifier::CENTURY},
    {"c", DatePartSpecifier::CENTURY},
    {"cent", DatePartSpecifier::CENTURY},
    {"centuries", DatePartSpecifier::CENTURY},
    {"millennium", DatePartSpecifier::MILLENNIUM},
    {"mil", DatePartSpecifier::MILLENNIUM},
    {"mils", DatePartSpecifier::MILLENNIUM},
    {"millennia", DatePartSpecifier::MILLENNIUM},
    {"millenniums", DatePartSpecifier::MILLENNIUM},
    {"millenium", DatePartSpecifier::MILLENNIUM},
    {"microseconds", DatePartSpecifier::MICROSECONDS},
    {"microsecond", DatePartSpecifier::MICROSECONDS},
    {"us", DatePartSpecifier::MICROSECONDS},
    {"usec", DatePartSpecifier::MICROSECONDS},
    {"usecs", DatePartSpecifier::MICROSECONDS},
    {"usecond", DatePartSpecifier::MICROSECONDS},
    {"useconds", DatePartSpecifier::MICROSECONDS},
    {"milliseconds", DatePartSpecifier::MILLISECONDS},
    {"millisecond", DatePartSpecifier::MILLISECONDS},
    {"ms", DatePartSpecifier::MILLISECONDS},
    {"msec", DatePartSpecifier::MILLISECONDS},
    {"msecs", DatePartSpecifier::MILLISECONDS},
    {"msecond", DatePartSpecifier::MILLISECONDS},
    {"mseconds", DatePartSpecifier::MILLISECONDS},
    {"second", DatePartSpecifier::SECOND},
    {"s", DatePartSpecifier::SECOND},
    {"sec", DatePartSpecifier::SECOND},
    {"secs", DatePartSpecifier::SECOND},
    {"seconds", DatePartSpecifier::SECOND},
    {"minute", DatePartSpecifier::MINUTE},
    {"m", DatePartSpecifier::MINUTE},
    {"min", DatePartSpecifier::MINUTE},
    {"mins", DatePartSpecifier::MINUTE},
    {"minutes", DatePartSpecifier::MINUTE},
    {"hour", DatePartSpecifier::HOUR},
    {"h", DatePartSpecifier::HOUR},
    {"hr", DatePartSpecifier::HOUR},
    {"hrs", DatePartSpecifier::HOUR},
    {"hours", DatePartSpecifier::HOUR},
    {"dow", DatePartSpecifier::DOW},
    {"dayofweek", DatePartSpecifier::DOW},
    {"weekday", DatePartSpecifier::DOW},
    {"isodow", DatePartSpecifier::ISODOW},
    {"week", DatePartSpecifier::WEEK},
    {"w", DatePartSpecifier::WEEK},
    {"weeks", DatePartSpecifier::WEEK},
    {"weekofyear", DatePartSpecifier::WEEK},
    {"isoyear", DatePartSpecifier::ISOYEAR},
    {"quarter", DatePartSpecifier::QUARTER},
    {"quarters", DatePartSpecifier::QUARTER},
    {"doy", DatePartSpecifier::DOY},
    {"dayofyear", DatePartSpecifier::DOY},
    {"yearweek", DatePartSpecifier::YEARWEEK},
    {"era", DatePartSpecifier::ERA},
    {"timezone", DatePartSpecifier::TIMEZONE},
    {"timezone_hour", DatePartSpecifier::TIMEZONE_HOUR},
    {"timezone_minute", DatePartSpecifier::TIMEZONE_MINUTE},
    {"epoch", DatePartSpecifier::EPOCH},
    {"julian", DatePartSpecifier::JULIAN_DAY},
    {"jd", DatePartSpecifier::JULIAN_DAY},
};

bool EqualsIgnoreCase(std::string_view a, const char *b) {
	idx_t i = 0;
	for (; i < a.size(); i++) {
		if (b[i] == '\0' || StringUtil::CharacterToLower(a[i]) != b[i]) {
			return false;
		}
	}
	return b[i] == '\0';
}

} // namespace

bool TryGetDatePartSpecifier(std::string_view specifier, DatePartSpecifier &result) {
	for (auto &entry : DATE_PART_NAMES) {
		if (EqualsIgnoreCase(specifier, entry.name)) {
			result = entry.part;
			return true;
		}
	}
	return false;
}

DatePartSpecifier GetDatePartSpecifier(std::string_view specifier) {
	DatePartSpecifier result;
	if (!TryGetDatePartSpecifier(specifier, result)) {
		throw ConversionException("extract specifier \"%s\" not recognized", string(specifier));
	}
	return result;
}

string DatePartFunctionName(DatePartSpecifier part) {
	switch (part) {
	case DatePartSpecifier::YEAR:
		return "year";
	case DatePartSpecifier::MONTH:
		return "month";
	case DatePartSpecifier::DAY:
		return "day";
	case DatePartSpecifier::DECADE:
		return "decade";
	case DatePartSpecifier::CENTURY:
		return "century";
	case DatePartSpecifier::MILLENNIUM:
		return "millennium";
	case DatePartSpecifier::MICROSECONDS:
		return "microsecond";
	case DatePartSpecifier::MILLISECONDS:
		return "millisecond";
	case DatePartSpecifier::SECOND:
		return "second";
	case DatePartSpecifier::MINUTE:
		return "minute";
	case DatePartSpecifier::HOUR:
		return "hour";
	case DatePartSpecifier::DOW:
		return "dayofweek";
	case DatePartSpecifier::ISODOW:
		return "isodow";
	case DatePartSpecifier::WEEK:
		return "week";
	case DatePartSpecifier::ISOYEAR:
		return "isoyear";
	case DatePartSpecifier::QUARTER:
		return "quarter";
	case DatePartSpecifier::DOY:
		return "dayofyear";
	case DatePartSpecifier::YEARWEEK:
		return "yearweek";
	case DatePartSpecifier::ERA:
		return "era";
	case DatePartSpecifier::TIMEZONE:
		return "timezone";
	case DatePartSpecifier::TIMEZONE_HOUR:
		return "timezone_hour";
	case DatePartSpecifier::TIMEZONE_MINUTE:
		return "timezone_minute";
	case DatePartSpecifier::EPOCH:
		return "epoch";
	case DatePartSpecifier::JULIAN_DAY:
		return "julian";
	default:
		throw InternalException("Unknown date part");
	}
}

} // namespace duckdb_icu
