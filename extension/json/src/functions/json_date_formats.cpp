#include "json_date_formats.hpp"

#include <cstdint>
#include <limits>

namespace duckdb {
namespace json {

//===--------------------------------------------------------------------===//
// Calendar helpers
//===--------------------------------------------------------------------===//
static const char *const MONTH_NAMES[] = {"January", "February", "March",     "April",   "May",      "June",
                                          "July",    "August",   "September", "October", "November", "December"};
static const char *const MONTH_NAMES_ABBREVIATED[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const DAY_NAMES[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const DAY_NAMES_ABBREVIATED[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static constexpr int64_t MICROS_PER_SEC = 1000000;
static constexpr int64_t MICROS_PER_DAY = 86400LL * MICROS_PER_SEC;
static constexpr int32_t DATE_MIN_YEAR = -290307;
static constexpr int32_t DATE_MAX_YEAR = 294247;

static bool IsLeapYear(int32_t year) {
	return (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
}

static int32_t DaysInMonth(int32_t year, int32_t month) {
	static const int32_t DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	return month == 2 && IsLeapYear(year) ? 29 : DAYS[month - 1];
}

//! Days since 1970-01-01 of a civil date (no validation)
static int32_t DaysFromCivil(int32_t y, int32_t m, int32_t d) {
	y -= m <= 2;
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	const int64_t yoe = y - era * 400;
	const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return static_cast<int32_t>(era * 146097 + doe - 719468);
}

static void CivilFromDays(int32_t z_in, int32_t &year, int32_t &month, int32_t &day) {
	const int64_t z = static_cast<int64_t>(z_in) + 719468;
	const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	const int64_t doe = z - era * 146097;
	const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const int64_t y = yoe + era * 400;
	const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const int64_t mp = (5 * doy + 2) / 153;
	day = static_cast<int32_t>(doy - (153 * mp + 2) / 5 + 1);
	month = static_cast<int32_t>(mp < 10 ? mp + 3 : mp - 9);
	year = static_cast<int32_t>(y + (month <= 2));
}

static bool TryFromDate(int32_t year, int32_t month, int32_t day, int32_t &result) {
	if (year < DATE_MIN_YEAR || year > DATE_MAX_YEAR || month < 1 || month > 12 || day < 1 ||
	    day > DaysInMonth(year, month)) {
		return false;
	}
	result = DaysFromCivil(year, month, day);
	return true;
}

//! The Monday of the week the date is in
static int32_t MondayOfCurrentWeek(int32_t days) {
	const int32_t weekday = ((days + 3) % 7 + 7) % 7; // 0 = Monday
	return days - weekday;
}

static bool IsSpace(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static bool IsDigit(char c) {
	return c >= '0' && c <= '9';
}

static bool IsAlpha(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static char ToLower(char c) {
	return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

static bool IsTimeZoneCharacter(char c) {
	return IsAlpha(c) || IsDigit(c) || c == '_' || c == '/' || c == '+' || c == '-' || c == ':';
}

//! Matches one of the special values (e.g. "infinity", whose abbreviation "inf" is also accepted)
static bool TryConvertDateSpecial(const char *buf, idx_t len, idx_t &pos, const char *special, idx_t abbreviation) {
	auto p = pos;
	auto cursor = special;
	for (; p < len && *cursor; ++p) {
		const auto s = *cursor++;
		if (ToLower(buf[p]) != s) {
			return false;
		}
	}
	if (*cursor && (p - pos) != abbreviation) {
		return false;
	}
	pos = p;
	return true;
}

static bool TryParseUTCOffset(const char *str, idx_t &pos, idx_t len, int &hh, int &mm, int &ss) {
	hh = 0;
	mm = 0;
	ss = 0;
	idx_t curpos = pos;
	const idx_t minlen = 1;
	if (curpos + minlen + 1 > len) {
		return true;
	}
	const char sign_char = str[curpos];
	if (sign_char != '+' && sign_char != '-') {
		return false;
	}
	curpos++;
	if (!IsDigit(str[curpos])) {
		return false;
	}
	hh = (str[curpos++] - '0');
	if (IsDigit(str[curpos])) {
		hh = hh * 10 + (str[curpos++] - '0');
	}
	if (sign_char == '-') {
		hh = -hh;
	}
	if (curpos >= len) {
		pos = curpos;
		return true;
	}
	const bool colons_used = (str[curpos] == ':');
	if (colons_used) {
		curpos++;
	}
	if (curpos + minlen > len || !IsDigit(str[curpos])) {
		pos = curpos;
		return !colons_used;
	}
	mm = (str[curpos++] - '0');
	if (IsDigit(str[curpos])) {
		mm = mm * 10 + (str[curpos++] - '0');
	}
	if (sign_char == '-') {
		mm = -mm;
	}
	if (curpos >= len || !colons_used || (str[curpos] != ':')) {
		pos = curpos;
		return true;
	}
	curpos++;
	if (curpos + minlen > len || !IsDigit(str[curpos])) {
		pos = curpos;
		return !colons_used;
	}
	ss = (str[curpos++] - '0');
	if (IsDigit(str[curpos])) {
		ss = ss * 10 + (str[curpos++] - '0');
	}
	if (sign_char == '-') {
		ss = -ss;
	}
	pos = curpos;
	return true;
}

static int NumericSpecifierWidth(JSONStrpTimeFormat::Specifier specifier) {
	using S = JSONStrpTimeFormat::Specifier;
	switch (specifier) {
	case S::WEEKDAY_DECIMAL:
	case S::WEEKDAY_ISO:
		return 1;
	case S::DAY_OF_MONTH_PADDED:
	case S::DAY_OF_MONTH:
	case S::MONTH_DECIMAL_PADDED:
	case S::MONTH_DECIMAL:
	case S::YEAR_WITHOUT_CENTURY_PADDED:
	case S::YEAR_WITHOUT_CENTURY:
	case S::HOUR_24_PADDED:
	case S::HOUR_24_DECIMAL:
	case S::HOUR_12_PADDED:
	case S::HOUR_12_DECIMAL:
	case S::MINUTE_PADDED:
	case S::MINUTE_DECIMAL:
	case S::SECOND_PADDED:
	case S::SECOND_DECIMAL:
	case S::WEEK_NUMBER_PADDED_SUN_FIRST:
	case S::WEEK_NUMBER_PADDED_MON_FIRST:
	case S::WEEK_NUMBER_ISO:
		return 2;
	case S::MILLISECOND_PADDED:
	case S::DAY_OF_YEAR_PADDED:
	case S::DAY_OF_YEAR_DECIMAL:
		return 3;
	case S::YEAR_DECIMAL:
	case S::YEAR_ISO:
		return 4;
	case S::MICROSECOND_PADDED:
		return 6;
	case S::NANOSECOND_PADDED:
		return 9;
	default:
		return -1;
	}
}

//===--------------------------------------------------------------------===//
// Format string
//===--------------------------------------------------------------------===//
void JSONStrpTimeFormat::AddSpecifier(string literal, Specifier specifier) {
	literals.push_back(std::move(literal));
	specifiers.push_back(specifier);
	numeric_width.push_back(NumericSpecifierWidth(specifier));
}

string JSONStrpTimeFormat::ParseFormatSpecifier(const string &format_string, JSONStrpTimeFormat &format) {
	using S = Specifier;
	if (format_string.empty()) {
		return "Empty format string";
	}
	format.format_specifier = format_string;
	format.specifiers.clear();
	format.literals.clear();
	format.numeric_width.clear();
	idx_t pos = 0;
	string current_literal;
	for (idx_t i = 0; i < format_string.size(); i++) {
		if (format_string[i] != '%') {
			continue;
		}
		if (i + 1 == format_string.size()) {
			return "Trailing format character %";
		}
		if (i > pos) {
			current_literal += format_string.substr(pos, i - pos);
		}
		char format_char = format_string[++i];
		if (format_char == '%') {
			// %%: a literal percent sign starting at the second one
			pos = i;
			continue;
		}
		S specifier;
		if (format_char == '-' && i + 1 < format_string.size()) {
			format_char = format_string[++i];
			switch (format_char) {
			case 'd':
				specifier = S::DAY_OF_MONTH;
				break;
			case 'm':
				specifier = S::MONTH_DECIMAL;
				break;
			case 'y':
				specifier = S::YEAR_WITHOUT_CENTURY;
				break;
			case 'H':
				specifier = S::HOUR_24_DECIMAL;
				break;
			case 'I':
				specifier = S::HOUR_12_DECIMAL;
				break;
			case 'M':
				specifier = S::MINUTE_DECIMAL;
				break;
			case 'S':
				specifier = S::SECOND_DECIMAL;
				break;
			case 'j':
				specifier = S::DAY_OF_YEAR_DECIMAL;
				break;
			default:
				return "Unrecognized format for strftime/strptime: %-" + string(1, format_char);
			}
		} else {
			switch (format_char) {
			case 'a':
				specifier = S::ABBREVIATED_WEEKDAY_NAME;
				break;
			case 'A':
				specifier = S::FULL_WEEKDAY_NAME;
				break;
			case 'w':
				specifier = S::WEEKDAY_DECIMAL;
				break;
			case 'u':
				specifier = S::WEEKDAY_ISO;
				break;
			case 'd':
				specifier = S::DAY_OF_MONTH_PADDED;
				break;
			case 'h':
			case 'b':
				specifier = S::ABBREVIATED_MONTH_NAME;
				break;
			case 'B':
				specifier = S::FULL_MONTH_NAME;
				break;
			case 'm':
				specifier = S::MONTH_DECIMAL_PADDED;
				break;
			case 'y':
				specifier = S::YEAR_WITHOUT_CENTURY_PADDED;
				break;
			case 'Y':
				specifier = S::YEAR_DECIMAL;
				break;
			case 'G':
				specifier = S::YEAR_ISO;
				break;
			case 'H':
				specifier = S::HOUR_24_PADDED;
				break;
			case 'I':
				specifier = S::HOUR_12_PADDED;
				break;
			case 'p':
				specifier = S::AM_PM;
				break;
			case 'M':
				specifier = S::MINUTE_PADDED;
				break;
			case 'S':
				specifier = S::SECOND_PADDED;
				break;
			case 'n':
				specifier = S::NANOSECOND_PADDED;
				break;
			case 'f':
				specifier = S::MICROSECOND_PADDED;
				break;
			case 'g':
				specifier = S::MILLISECOND_PADDED;
				break;
			case 'z':
				specifier = S::UTC_OFFSET;
				break;
			case 'Z':
				specifier = S::TZ_NAME;
				break;
			case 'j':
				specifier = S::DAY_OF_YEAR_PADDED;
				break;
			case 'U':
				specifier = S::WEEK_NUMBER_PADDED_SUN_FIRST;
				break;
			case 'W':
				specifier = S::WEEK_NUMBER_PADDED_MON_FIRST;
				break;
			case 'V':
				specifier = S::WEEK_NUMBER_ISO;
				break;
			case 'c':
			case 'x':
			case 'X':
			case 'T': {
				// The locale formats are the ISO ones
				const char *subformat = format_char == 'c'   ? "%Y-%m-%d %H:%M:%S"
				                        : format_char == 'x' ? "%Y-%m-%d"
				                                             : "%H:%M:%S";
				JSONStrpTimeFormat locale_format;
				auto error = ParseFormatSpecifier(subformat, locale_format);
				if (!error.empty()) {
					throw InternalException("Failed to bind sub-format specifier \"%s\": %s", subformat, error);
				}
				locale_format.literals[0] = std::move(current_literal) + locale_format.literals[0];
				current_literal = "";
				for (idx_t s = 0; s < locale_format.specifiers.size(); s++) {
					format.AddSpecifier(std::move(locale_format.literals[s]), locale_format.specifiers[s]);
				}
				pos = i + 1;
				continue;
			}
			default:
				return "Unrecognized format for strftime/strptime: %" + string(1, format_char);
			}
		}
		format.AddSpecifier(std::move(current_literal), specifier);
		current_literal = "";
		pos = i + 1;
	}
	if (pos < format_string.size()) {
		current_literal += format_string.substr(pos, format_string.size() - pos);
	}
	format.literals.push_back(std::move(current_literal));
	return string();
}

//===--------------------------------------------------------------------===//
// Parsing
//===--------------------------------------------------------------------===//
struct JSONStrpTimeFormat::ParseResult {
	//! year, month, day, hour, minute, second, nanosecond, UTC offset (seconds)
	int32_t data[8];
	string error_message;
	idx_t error_position = static_cast<idx_t>(-1);
	bool is_special = false;
	//! days (DATE) of the special value; INT32_MAX / -INT32_MAX are infinities
	int32_t special = 0;
};

static int32_t TryParseCollection(const char *data, idx_t &pos, idx_t size, const char *const collection[],
                                  idx_t collection_count) {
	for (idx_t c = 0; c < collection_count; c++) {
		const char *entry = collection[c];
		idx_t entry_size = 0;
		while (entry[entry_size]) {
			entry_size++;
		}
		if (pos + entry_size > size) {
			continue;
		}
		idx_t i;
		for (i = 0; i < entry_size; i++) {
			if (ToLower(entry[i]) != ToLower(data[pos + i])) {
				break;
			}
		}
		if (i == entry_size) {
			pos += entry_size;
			return static_cast<int32_t>(c);
		}
	}
	return -1;
}

enum class AmPm : uint8_t { NONE, AM, PM };

bool JSONStrpTimeFormat::Parse(string_view input, ParseResult &result) const {
	using S = Specifier;
	auto &result_data = result.data;
	auto &error_message = result.error_message;
	auto &error_position = result.error_position;

	result_data[0] = 1900;
	result_data[1] = 1;
	result_data[2] = 1;
	for (idx_t i = 3; i < 8; i++) {
		result_data[i] = 0;
	}
	auto data = input.data();
	idx_t size = input.size();
	// skip leading spaces
	while (size > 0 && IsSpace(*data)) {
		data++;
		size--;
	}
	// the special values
	idx_t pos = 0;
	result.is_special = false;
	if (size > 4) {
		if (IsAlpha(*data)) {
			if (TryConvertDateSpecial(data, size, pos, "infinity", 3)) {
				result.is_special = true;
				result.special = std::numeric_limits<int32_t>::max();
			} else if (TryConvertDateSpecial(data, size, pos, "epoch", 5)) {
				result.is_special = true;
				result.special = 0;
			}
		} else if (*data == '-' && TryConvertDateSpecial(data, size, pos, "-infinity", 4)) {
			result.is_special = true;
			result.special = -std::numeric_limits<int32_t>::max();
		}
	}
	if (result.is_special) {
		while (pos < size && IsSpace(data[pos])) {
			pos++;
		}
		if (pos != size) {
			error_message = "Special timestamp did not match: trailing characters";
			error_position = pos;
			return false;
		}
		return true;
	}

	AmPm ampm = AmPm::NONE;
	// Year offset state (Year+W/j)
	auto offset_specifier = S::WEEKDAY_DECIMAL;
	uint64_t weekno = 0;
	uint64_t weekday = 0;
	uint64_t yearday = 0;
	bool has_weekday = false;
	// ISO state (%G/%V/%u): out of range values detect multiple specifications
	uint64_t iso_year = 10000;
	uint64_t iso_week = 54;
	uint64_t iso_weekday = 8;

	for (idx_t i = 0;; i++) {
		const auto &literal = literals[i];
		for (size_t l = 0; l < literal.size();) {
			// runs of spaces match runs of spaces
			if (IsSpace(literal[l])) {
				if (pos >= size || !IsSpace(data[pos])) {
					error_message = "Space does not match, expected " + literals[i];
					error_position = pos;
					return false;
				}
				for (++pos; pos < size && IsSpace(data[pos]); ++pos) {
				}
				for (++l; l < literal.size() && IsSpace(literal[l]); ++l) {
				}
				continue;
			}
			if (pos >= size || data[pos] != literal[l]) {
				error_message = "Literal does not match, expected " + literal;
				error_position = pos;
				return false;
			}
			pos++;
			l++;
		}
		if (i == specifiers.size()) {
			break;
		}
		if (numeric_width[i] > 0) {
			uint64_t number = 0;
			int digits = 0;
			size_t start_pos = pos;
			size_t end_pos = start_pos + static_cast<size_t>(numeric_width[i]);
			while (pos < size && pos < end_pos && IsDigit(data[pos])) {
				number = number * 10 + static_cast<uint64_t>(data[pos]) - '0';
				pos++;
				++digits;
			}
			if (pos == start_pos) {
				error_message = "Expected a number";
				error_position = start_pos;
				return false;
			}
			switch (specifiers[i]) {
			case S::DAY_OF_MONTH_PADDED:
			case S::DAY_OF_MONTH:
				if (number < 1 || number > 31) {
					error_message = "Day out of range, expected a value between 1 and 31";
					error_position = start_pos;
					return false;
				}
				result_data[2] = static_cast<int32_t>(number);
				offset_specifier = specifiers[i];
				break;
			case S::MONTH_DECIMAL_PADDED:
			case S::MONTH_DECIMAL:
				if (number < 1 || number > 12) {
					error_message = "Month out of range, expected a value between 1 and 12";
					error_position = start_pos;
					return false;
				}
				result_data[1] = static_cast<int32_t>(number);
				offset_specifier = specifiers[i];
				break;
			case S::YEAR_WITHOUT_CENTURY_PADDED:
			case S::YEAR_WITHOUT_CENTURY:
				switch (offset_specifier) {
				case S::YEAR_ISO:
				case S::WEEK_NUMBER_ISO:
				case S::WEEKDAY_DECIMAL:
					offset_specifier = specifiers[i];
					break;
				default:
					break;
				}
				if (number >= 100) {
					error_message = "Year without century out of range, expected a value between 0 and 99";
					error_position = start_pos;
					return false;
				}
				// 69 is the crossover point (>= 69 is 19.., < 69 is 20..)
				result_data[0] =
				    number >= 69 ? static_cast<int32_t>(1900 + number) : static_cast<int32_t>(2000 + number);
				break;
			case S::YEAR_DECIMAL:
				switch (offset_specifier) {
				case S::YEAR_ISO:
				case S::WEEK_NUMBER_ISO:
				case S::WEEKDAY_DECIMAL:
					offset_specifier = specifiers[i];
					break;
				default:
					break;
				}
				result_data[0] = static_cast<int32_t>(number);
				break;
			case S::YEAR_ISO:
				switch (offset_specifier) {
				case S::DAY_OF_MONTH_PADDED:
				case S::DAY_OF_MONTH:
				case S::MONTH_DECIMAL_PADDED:
				case S::MONTH_DECIMAL:
				case S::YEAR_WITHOUT_CENTURY_PADDED:
				case S::YEAR_WITHOUT_CENTURY:
				case S::YEAR_DECIMAL:
					// y/m/d overrides G/V/u but does not conflict
					break;
				case S::WEEKDAY_DECIMAL:
					offset_specifier = specifiers[i];
					break;
				case S::YEAR_ISO:
				case S::WEEK_NUMBER_ISO:
					if (iso_year <= 9999) {
						error_message = "Multiple ISO year offsets specified";
						error_position = start_pos;
						return false;
					}
					break;
				default:
					error_message = "Incompatible ISO year offset specified";
					error_position = start_pos;
					return false;
				}
				if (number > 9999) {
					error_message = "ISO Year out of range, expected a value between 0000 and 9999";
					error_position = start_pos;
					return false;
				}
				iso_year = number;
				break;
			case S::HOUR_24_PADDED:
			case S::HOUR_24_DECIMAL:
				if (number >= 24) {
					error_message = "Hour out of range, expected a value between 0 and 23";
					error_position = start_pos;
					return false;
				}
				result_data[3] = static_cast<int32_t>(number);
				break;
			case S::HOUR_12_PADDED:
			case S::HOUR_12_DECIMAL:
				if (number < 1 || number > 12) {
					error_message = "Hour12 out of range, expected a value between 1 and 12";
					error_position = start_pos;
					return false;
				}
				result_data[3] = static_cast<int32_t>(number);
				break;
			case S::MINUTE_PADDED:
			case S::MINUTE_DECIMAL:
				if (number >= 60) {
					error_message = "Minutes out of range, expected a value between 0 and 59";
					error_position = start_pos;
					return false;
				}
				result_data[4] = static_cast<int32_t>(number);
				break;
			case S::SECOND_PADDED:
			case S::SECOND_DECIMAL:
				if (number >= 60) {
					error_message = "Seconds out of range, expected a value between 0 and 59";
					error_position = start_pos;
					return false;
				}
				result_data[5] = static_cast<int32_t>(number);
				break;
			case S::NANOSECOND_PADDED:
				for (; digits < numeric_width[i]; ++digits) {
					number *= 10;
				}
				result_data[6] = static_cast<int32_t>(number);
				break;
			case S::MICROSECOND_PADDED:
				for (; digits < numeric_width[i]; ++digits) {
					number *= 10;
				}
				result_data[6] = static_cast<int32_t>(number * 1000);
				break;
			case S::MILLISECOND_PADDED:
				for (; digits < numeric_width[i]; ++digits) {
					number *= 10;
				}
				result_data[6] = static_cast<int32_t>(number * 1000000);
				break;
			case S::WEEK_NUMBER_PADDED_SUN_FIRST:
			case S::WEEK_NUMBER_PADDED_MON_FIRST:
				switch (offset_specifier) {
				case S::DAY_OF_MONTH_PADDED:
				case S::DAY_OF_MONTH:
				case S::MONTH_DECIMAL_PADDED:
				case S::MONTH_DECIMAL:
					// m/d overrides WU/w but does not conflict
					break;
				case S::YEAR_WITHOUT_CENTURY_PADDED:
				case S::YEAR_WITHOUT_CENTURY:
				case S::YEAR_DECIMAL:
				case S::WEEKDAY_DECIMAL:
					offset_specifier = specifiers[i];
					break;
				default:
					error_message = "Multiple week offsets specified";
					error_position = start_pos;
					return false;
				}
				if (number > 53) {
					error_message = "Week out of range, expected a value between 0 and 53";
					error_position = start_pos;
					return false;
				}
				weekno = number;
				break;
			case S::WEEKDAY_DECIMAL:
				if (number > 6) {
					error_message = "Weekday out of range, expected a value between 0 and 6";
					error_position = start_pos;
					return false;
				}
				has_weekday = true;
				weekday = number;
				break;
			case S::WEEK_NUMBER_ISO:
				switch (offset_specifier) {
				case S::YEAR_WITHOUT_CENTURY_PADDED:
				case S::YEAR_WITHOUT_CENTURY:
				case S::YEAR_DECIMAL:
					error_message = "ISO week offsets are incompatible with non-ISO year specifiers. Use '%G' instead";
					error_position = start_pos;
					return false;
				case S::DAY_OF_MONTH_PADDED:
				case S::DAY_OF_MONTH:
				case S::MONTH_DECIMAL_PADDED:
				case S::MONTH_DECIMAL:
					break;
				case S::WEEKDAY_DECIMAL:
					offset_specifier = specifiers[i];
					break;
				case S::WEEK_NUMBER_ISO:
				case S::YEAR_ISO:
					if (iso_week <= 53) {
						error_message = "Multiple ISO week offsets specified";
						error_position = start_pos;
						return false;
					}
					break;
				default:
					error_message = "Incompatible ISO week offset specified";
					error_position = start_pos;
					return false;
				}
				if (number < 1 || number > 53) {
					error_message = "ISO week offset out of range, expected a value between 1 and 53";
					error_position = start_pos;
					return false;
				}
				iso_week = number;
				break;
			case S::WEEKDAY_ISO:
				if (iso_weekday <= 7) {
					error_message = "Multiple ISO weekday offsets specified";
					error_position = start_pos;
					return false;
				}
				if (number < 1 || number > 7) {
					error_message = "ISO weekday offset out of range, expected a value between 1 and 7";
					error_position = start_pos;
					return false;
				}
				iso_weekday = number;
				break;
			case S::DAY_OF_YEAR_PADDED:
			case S::DAY_OF_YEAR_DECIMAL:
				switch (offset_specifier) {
				case S::DAY_OF_MONTH_PADDED:
				case S::DAY_OF_MONTH:
				case S::MONTH_DECIMAL_PADDED:
				case S::MONTH_DECIMAL:
					// m/d overrides j but does not conflict
					break;
				case S::YEAR_WITHOUT_CENTURY_PADDED:
				case S::YEAR_WITHOUT_CENTURY:
				case S::YEAR_DECIMAL:
				case S::WEEKDAY_DECIMAL:
					offset_specifier = specifiers[i];
					break;
				default:
					error_message = "Multiple year offsets specified";
					error_position = start_pos;
					return false;
				}
				if (number < 1 || number > 366) {
					error_message = "Year day out of range, expected a value between 1 and 366";
					error_position = start_pos;
					return false;
				}
				yearday = number;
				break;
			default:
				throw NotImplementedException("Unsupported specifier for strptime");
			}
		} else {
			switch (specifiers[i]) {
			case S::AM_PM: {
				if (pos + 2 > size) {
					error_message = "Expected AM/PM";
					error_position = pos;
					return false;
				}
				const char pa_char = ToLower(data[pos]);
				const char m_char = ToLower(data[pos + 1]);
				if (m_char != 'm') {
					error_message = "Expected AM/PM";
					error_position = pos;
					return false;
				}
				if (pa_char == 'p') {
					ampm = AmPm::PM;
				} else if (pa_char == 'a') {
					ampm = AmPm::AM;
				} else {
					error_message = "Expected AM/PM";
					error_position = pos;
					return false;
				}
				pos += 2;
				break;
			}
			// weekday names are parsed but not used
			case S::ABBREVIATED_WEEKDAY_NAME:
				if (TryParseCollection(data, pos, size, DAY_NAMES_ABBREVIATED, 7) < 0) {
					error_message = "Expected an abbreviated day name (Mon, Tue, Wed, Thu, Fri, Sat, Sun)";
					error_position = pos;
					return false;
				}
				break;
			case S::FULL_WEEKDAY_NAME:
				if (TryParseCollection(data, pos, size, DAY_NAMES, 7) < 0) {
					error_message = "Expected a full day name (Monday, Tuesday, etc...)";
					error_position = pos;
					return false;
				}
				break;
			case S::ABBREVIATED_MONTH_NAME: {
				const auto month = TryParseCollection(data, pos, size, MONTH_NAMES_ABBREVIATED, 12);
				if (month < 0) {
					error_message = "Expected an abbreviated month name (Jan, Feb, Mar, etc..)";
					error_position = pos;
					return false;
				}
				result_data[1] = month + 1;
				break;
			}
			case S::FULL_MONTH_NAME: {
				const auto month = TryParseCollection(data, pos, size, MONTH_NAMES, 12);
				if (month < 0) {
					error_message = "Expected a full month name (January, February, etc...)";
					error_position = pos;
					return false;
				}
				result_data[1] = month + 1;
				break;
			}
			case S::UTC_OFFSET: {
				int hh, mm, ss;
				if (!TryParseUTCOffset(data, pos, size, hh, mm, ss)) {
					error_message = "Expected \xC2\xB1HH[MM] or -HH[:MM[:SS]]";
					error_position = pos;
					return false;
				}
				result_data[7] = (hh * 60 + mm) * 60 + ss;
				break;
			}
			case S::TZ_NAME: {
				while (pos < size && IsSpace(data[pos])) {
					pos++;
				}
				const auto tz_begin = pos;
				while (pos < size && IsTimeZoneCharacter(data[pos])) {
					pos++;
				}
				if (pos == tz_begin) {
					error_message = "Empty Time Zone name";
					error_position = tz_begin;
					return false;
				}
				break;
			}
			default:
				throw NotImplementedException("Unsupported specifier for strptime");
			}
		}
	}
	// skip trailing spaces
	while (pos < size && IsSpace(data[pos])) {
		pos++;
	}
	if (pos != size) {
		error_message = "Full specifier did not match: trailing characters";
		error_position = pos;
		return false;
	}
	if (ampm != AmPm::NONE) {
		if (result_data[3] > 12) {
			error_message =
			    "Invalid hour: " + std::to_string(result_data[3]) + " AM/PM, expected an hour within the range [0..12]";
			return false;
		}
		if (ampm == AmPm::AM) {
			// 12AM = 0
			if (result_data[3] == 12) {
				result_data[3] = 0;
			}
		} else if (result_data[3] != 12) {
			result_data[3] += 12;
		}
	}
	switch (offset_specifier) {
	case S::YEAR_ISO:
	case S::WEEK_NUMBER_ISO: {
		iso_year = (iso_year > 9999) ? 1900 : iso_year;
		iso_week = (iso_week > 53) ? 1 : iso_week;
		iso_weekday = (iso_weekday > 7) ? 1 : iso_weekday;
		// Gregorian and ISO agree on the year of January 4, and ISO week 1 starts on the previous Monday
		const auto jan4 = DaysFromCivil(static_cast<int32_t>(iso_year), 1, 4);
		const auto week1 = MondayOfCurrentWeek(jan4);
		const auto iso_date = week1 + static_cast<int32_t>((iso_week - 1) * 7 + (iso_weekday - 1));
		CivilFromDays(iso_date, result_data[0], result_data[1], result_data[2]);
		break;
	}
	case S::WEEK_NUMBER_PADDED_SUN_FIRST:
	case S::WEEK_NUMBER_PADDED_MON_FIRST: {
		if (has_weekday) {
			weekday = (weekday + 7 - static_cast<uint64_t>(offset_specifier == S::WEEK_NUMBER_PADDED_MON_FIRST)) % 7;
		}
		// the start of week 1, move back 7 days and then weekno * 7 + weekday gives the date
		const auto jan1 = DaysFromCivil(result_data[0], 1, 1);
		auto yeardate = MondayOfCurrentWeek(jan1);
		yeardate -= int(offset_specifier == S::WEEK_NUMBER_PADDED_SUN_FIRST);
		yeardate -= 7 * int(yeardate >= jan1);
		yeardate += static_cast<int32_t>(weekno * 7 + weekday);
		CivilFromDays(yeardate, result_data[0], result_data[1], result_data[2]);
		break;
	}
	case S::DAY_OF_YEAR_PADDED:
	case S::DAY_OF_YEAR_DECIMAL: {
		const auto year = result_data[0];
		auto yeardate = DaysFromCivil(year, 1, 1);
		yeardate += static_cast<int32_t>(yearday - 1);
		CivilFromDays(yeardate, result_data[0], result_data[1], result_data[2]);
		if (result_data[0] != year) {
			// day 366 of a year that is not a leap year
			error_message = "Year day out of range for the year";
			error_position = 0;
			return false;
		}
		break;
	}
	default:
		// m/d overrides UWVwu/j
		break;
	}
	return true;
}

string JSONStrpTimeFormat::FormatError(string_view input, const ParseResult &result) const {
	string position_line;
	if (result.error_position != static_cast<idx_t>(-1)) {
		position_line = string(input) + "\n" + string(result.error_position, ' ') + "^";
	}
	return Format("Could not parse string \"%s\" according to format specifier \"%s\"\n%s\nError: %s", string(input),
	              format_specifier, position_line, result.error_message);
}

bool JSONStrpTimeFormat::TryParseDate(string_view input, int32_t &days, string &error) const {
	ParseResult result;
	if (!Parse(input, result)) {
		error = FormatError(input, result);
		return false;
	}
	if (result.is_special) {
		days = result.special;
		return true;
	}
	return TryFromDate(result.data[0], result.data[1], result.data[2], days);
}

bool JSONStrpTimeFormat::TryParseTimestamp(string_view input, int64_t &micros, string &error) const {
	ParseResult result;
	if (!Parse(input, result)) {
		error = FormatError(input, result);
		return false;
	}
	if (result.is_special) {
		if (result.special == std::numeric_limits<int32_t>::max()) {
			micros = std::numeric_limits<int64_t>::max();
		} else if (result.special == -std::numeric_limits<int32_t>::max()) {
			micros = -std::numeric_limits<int64_t>::max();
		} else {
			micros = static_cast<int64_t>(result.special) * MICROS_PER_DAY;
		}
		return true;
	}
	int32_t days;
	if (!TryFromDate(result.data[0], result.data[1], result.data[2], days)) {
		return false;
	}
	// the UTC offset is subtracted from the time of day
	const int32_t offset = result.data[7];
	const int32_t hh = offset / 3600;
	const int32_t mm = (offset % 3600) / 60;
	const int32_t ss = offset % 60;
	const int32_t nanos = result.data[6];
	const int32_t sub_micros = (nanos + 500) / 1000;
	const int64_t time =
	    ((static_cast<int64_t>(result.data[3] - hh) * 60 + (result.data[4] - mm)) * 60 + (result.data[5] - ss)) *
	        MICROS_PER_SEC +
	    sub_micros;
	int64_t day_part;
	if (__builtin_mul_overflow(static_cast<int64_t>(days), MICROS_PER_DAY, &day_part) ||
	    __builtin_add_overflow(day_part, time, &micros)) {
		return false;
	}
	return micros != std::numeric_limits<int64_t>::max() && micros != -std::numeric_limits<int64_t>::max();
}

//===--------------------------------------------------------------------===//
// JSONDateFormatMap
//===--------------------------------------------------------------------===//
void JSONDateFormatMap::AddFormat(LogicalTypeId type, const string &format_string) {
	auto &list = formats[static_cast<uint32_t>(type)];
	list.emplace_back();
	const auto error = JSONStrpTimeFormat::ParseFormatSpecifier(format_string, list.back());
	if (!error.empty()) {
		list.pop_back();
		throw InvalidInputException(error);
	}
}

void JSONDateFormatMap::AddDetectionFormats() {
	static const char *const DATE_FORMATS[] = {"%m-%d-%Y", "%m-%d-%y", "%d-%m-%Y", "%d-%m-%y", "%Y-%m-%d", "%y-%m-%d"};
	static const char *const TIMESTAMP_FORMATS[] = {
	    "%Y-%m-%d %H:%M:%S.%f", "%m-%d-%Y %I:%M:%S %p",  "%m-%d-%y %I:%M:%S %p", "%d-%m-%Y %H:%M:%S",
	    "%d-%m-%y %H:%M:%S",    "%Y-%m-%d %H:%M:%S",     "%y-%m-%d %H:%M:%S",    "%Y-%m-%dT%H:%M:%S",
	    "%Y-%m-%dT%H:%M:%SZ",   "%Y-%m-%dT%H:%M:%S.%fZ", "%Y-%m-%dT%H:%M:%S%z",  "%Y-%m-%dT%H:%M:%S.%f%z"};
	// a type with a user-specified format keeps just that
	if (!HasFormats(LogicalTypeId::DATE)) {
		for (auto format : DATE_FORMATS) {
			AddFormat(LogicalTypeId::DATE, format);
		}
	}
	if (!HasFormats(LogicalTypeId::TIMESTAMP)) {
		for (auto format : TIMESTAMP_FORMATS) {
			AddFormat(LogicalTypeId::TIMESTAMP, format);
		}
	}
}

bool JSONDateFormatMap::HasFormats(LogicalTypeId type) const {
	return formats.find(static_cast<uint32_t>(type)) != formats.end();
}

idx_t JSONDateFormatMap::FormatCount(LogicalTypeId type) const {
	auto entry = formats.find(static_cast<uint32_t>(type));
	return entry == formats.end() ? 0 : entry->second.size();
}

bool JSONDateFormatMap::TryParse(LogicalTypeId type, idx_t format_idx, string_view input, int64_t &result,
                                 string &error) const {
	auto entry = formats.find(static_cast<uint32_t>(type));
	if (entry == formats.end() || format_idx >= entry->second.size()) {
		return false;
	}
	auto &format = entry->second[format_idx];
	if (type == LogicalTypeId::DATE) {
		int32_t days;
		if (!format.TryParseDate(input, days, error)) {
			return false;
		}
		result = days;
		return true;
	}
	return format.TryParseTimestamp(input, result, error);
}

bool JSONDateFormatMap::MatchesAnyFormat(LogicalTypeId type, const Vector &strings, idx_t count) {
	ValueView<varchar_t> values(strings);
	string error;
	for (idx_t f = FormatCount(type); f > 0; f--) {
		bool success = true;
		for (idx_t i = 0; i < count && success; i++) {
			if (!values.IsValid(i)) {
				continue;
			}
			int64_t parsed;
			success = TryParse(type, f - 1, values.Get(i).view(), parsed, error);
		}
		if (success) {
			return true;
		}
	}
	return false;
}

} // namespace json
} // namespace duckdb
