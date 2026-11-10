#include "icu_bridge.hpp"

#include "calendar.hpp"
#include "collation_collator.hpp"
#include "duckdb/common/string_util.hpp"
#include "timezone.hpp"

#include <cstdio>
#include <stdexcept>

namespace icu_bridge {

using duckdb::datetime::CAL_DATE;
using duckdb::datetime::CAL_DAY_OF_WEEK;
using duckdb::datetime::CAL_DAY_OF_YEAR;
using duckdb::datetime::CAL_DST_OFFSET;
using duckdb::datetime::CAL_ERA;
using duckdb::datetime::CAL_HOUR_OF_DAY;
using duckdb::datetime::CAL_MILLISECOND;
using duckdb::datetime::CAL_MINUTE;
using duckdb::datetime::CAL_MONTH;
using duckdb::datetime::CAL_SECOND;
using duckdb::datetime::CAL_YEAR;
using duckdb::datetime::CAL_ZONE_OFFSET;
using duckdb::datetime::CalendarField;

static constexpr int64_t MICROS_PER_MILLI = 1000;
static constexpr int64_t MILLIS_PER_SECOND = 1000;
static constexpr int64_t MICROS_PER_DAY = 86400LL * 1000000LL;

//----------------------------------------------------------------------------------------------------------------------
// Names
//----------------------------------------------------------------------------------------------------------------------
static std::unique_ptr<duckdb::datetime::TimeZone> NormalizedTimeZone(std::string &tz_str) {
	auto tz = duckdb::datetime::TimeZone::TryCreate(tz_str);
	if (tz) {
		return tz;
	}

	//	Map UTC±NN00 to Etc/UTC±N
	do {
		if (tz_str.size() <= 4) {
			break;
		}
		if (tz_str.compare(0, 3, "UTC")) {
			break;
		}

		size_t pos = 3;
		const auto utc = tz_str[pos++];
		// The sign is inverted (UTC and Etc use opposite sign conventions)
		char sign;
		if (utc == '+') {
			sign = '-';
		} else if (utc == '-') {
			sign = '+';
		} else {
			break;
		}

		std::string remainder;
		for (; pos < tz_str.size(); ++pos) {
			const auto ch = tz_str[pos];
			if (ch != ':' && !(ch >= '0' && ch <= '9')) {
				break;
			}
			remainder += ch;
		}
		if (pos < tz_str.size()) {
			break;
		}

		size_t start = 0;
		while (start < remainder.size() && remainder[start] == '0') {
			++start;
		}
		remainder = remainder.substr(start);

		std::string hours_str;
		auto colon_idx = remainder.find(':');
		if (colon_idx != std::string::npos) {
			hours_str = remainder.substr(0, colon_idx);
		} else if (remainder.size() <= 2) {
			hours_str = remainder;
		} else {
			// HHMM: the last two digits are minutes
			hours_str = remainder.substr(0, remainder.size() - 2);
		}

		std::string mapped = "Etc/GMT";
		if (hours_str.empty()) {
			mapped += "+0";
		} else {
			mapped += sign;
			mapped += hours_str;
		}
		tz = duckdb::datetime::TimeZone::TryCreate(mapped);
		if (tz) {
			tz_str = mapped;
			return tz;
		}
	} while (false);

	return nullptr;
}

bool ResolveTimeZone(std::string &name, std::string *error) {
	if (NormalizedTimeZone(name)) {
		return true;
	}
	// Be friendlier: a case insensitive match wins, otherwise suggest candidates
	duckdb::vector<std::string> candidates;
	for (const auto &candidate : duckdb::datetime::TimeZone::GetAvailableIds()) {
		if (duckdb::StringUtil::CIEquals(candidate, name)) {
			name = candidate;
			return true;
		}
		candidates.emplace_back(candidate);
	}
	if (error) {
		*error = "Unknown TimeZone '" + name + "'!\n" +
		         duckdb::StringUtil::CandidatesMessage(duckdb::StringUtil::TopNJaroWinkler(candidates, name),
		                                               "Candidate time zones");
	}
	return false;
}

bool ResolveCalendar(std::string &name, std::string *error) {
	const auto &types = duckdb::datetime::Calendar::GetAvailableTypes();
	for (const auto &type : types) {
		if (type == name) {
			return true;
		}
	}
	for (const auto &candidate : types) {
		if (duckdb::StringUtil::CIEquals(candidate, name)) {
			name = candidate;
			return true;
		}
	}
	if (error) {
		*error = "Unknown Calendar '" + name + "'!\n" +
		         duckdb::StringUtil::CandidatesMessage(duckdb::StringUtil::TopNJaroWinkler(types, name),
		                                               "Candidate calendars");
	}
	return false;
}

std::string HostTimeZone() {
	auto tz = duckdb::datetime::TimeZone::TryCreateDefault();
	return tz ? tz->GetId() : "UTC";
}

std::vector<std::string> CalendarNames() {
	return duckdb::datetime::Calendar::GetAvailableTypes();
}

//----------------------------------------------------------------------------------------------------------------------
// Calendar
//----------------------------------------------------------------------------------------------------------------------
struct Calendar::Impl {
	std::unique_ptr<duckdb::datetime::Calendar> calendar;
};

Calendar::Calendar() : impl(new Impl()) {
}

Calendar::~Calendar() = default;

std::unique_ptr<Calendar> Calendar::Create(const std::string &time_zone, const std::string &calendar) {
	auto tz = duckdb::datetime::TimeZone::TryCreate(time_zone);
	if (!tz) {
		tz = duckdb::datetime::TimeZone::TryCreate("UTC");
	}
	std::unique_ptr<Calendar> result(new Calendar());
	result->impl->calendar = duckdb::datetime::Calendar::TryCreate(calendar, std::move(tz));
	if (!result->impl->calendar) {
		throw std::runtime_error("Unable to create calendar.");
	}
	return result;
}

void Calendar::SetTimeZone(const std::string &time_zone) {
	auto tz = duckdb::datetime::TimeZone::TryCreate(time_zone);
	if (tz) {
		impl->calendar->SetTimeZone(std::move(tz));
	}
}

static int64_t FloorDiv(int64_t value, int64_t divisor) {
	auto quotient = value / divisor;
	if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) {
		--quotient;
	}
	return quotient;
}

static int64_t FloorMod(int64_t value, int64_t divisor) {
	return value - FloorDiv(value, divisor) * divisor;
}

bool Calendar::ToLocal(int64_t instant, int64_t &local) {
	auto &cal = *impl->calendar;
	cal.SetTime(FloorDiv(instant, MICROS_PER_MILLI));
	const auto offset = int64_t(cal.Get(CAL_ZONE_OFFSET)) + int64_t(cal.Get(CAL_DST_OFFSET));
	if (cal.HasFailed()) {
		return false;
	}
	return !__builtin_add_overflow(instant, offset * MICROS_PER_MILLI, &local);
}

//! The civil date of a day number (days since 1970-01-01), proleptic Gregorian
static void CivilFromDays(int64_t days, int64_t &year, int32_t &month, int32_t &day) {
	days += 719468;
	const int64_t era = FloorDiv(days, 146097);
	const auto doe = static_cast<int32_t>(days - era * 146097);
	const int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const int32_t mp = (5 * doy + 2) / 153;
	day = doy - (153 * mp + 2) / 5 + 1;
	month = mp < 10 ? mp + 3 : mp - 9;
	year = yoe + era * 400 + (month <= 2 ? 1 : 0);
}

bool Calendar::FromLocal(int64_t local, int64_t &instant) {
	auto &cal = *impl->calendar;
	const auto days = FloorDiv(local, MICROS_PER_DAY);
	const auto time_of_day = FloorMod(local, MICROS_PER_DAY);
	int64_t year;
	int32_t month;
	int32_t day;
	CivilFromDays(days, year, month, day);
	if (year > INT32_MAX || year < INT32_MIN) {
		return false;
	}
	const auto micros_of_second = time_of_day % 1000000;
	const auto seconds = time_of_day / 1000000;
	cal.Set(CAL_YEAR, static_cast<int32_t>(year));
	cal.Set(CAL_MONTH, month - 1);
	cal.Set(CAL_DATE, day);
	cal.Set(CAL_HOUR_OF_DAY, static_cast<int32_t>(seconds / 3600));
	cal.Set(CAL_MINUTE, static_cast<int32_t>(seconds / 60 % 60));
	cal.Set(CAL_SECOND, static_cast<int32_t>(seconds % 60));
	cal.Set(CAL_MILLISECOND, static_cast<int32_t>(micros_of_second / MICROS_PER_MILLI));
	const auto millis = cal.GetTime();
	if (cal.HasFailed()) {
		return false;
	}
	int64_t scaled;
	if (__builtin_mul_overflow(millis, MICROS_PER_MILLI, &scaled)) {
		return false;
	}
	return !__builtin_add_overflow(scaled, micros_of_second % MICROS_PER_MILLI, &instant);
}

bool Calendar::GetPart(int64_t instant, Part part, int64_t &result) {
	auto &cal = *impl->calendar;
	cal.SetTime(FloorDiv(instant, MICROS_PER_MILLI));
	auto get = [&](CalendarField field) {
		return static_cast<int64_t>(cal.Get(field));
	};
	switch (part) {
	case Part::ERA:
		result = get(CAL_ERA);
		break;
	case Part::YEAR:
		result = get(CAL_YEAR);
		break;
	case Part::MONTH:
		result = get(CAL_MONTH) + 1;
		break;
	case Part::DAY:
		result = get(CAL_DATE);
		break;
	case Part::DAY_OF_WEEK:
		// [Sun(0), Sat(6)]
		result = get(CAL_DAY_OF_WEEK) - 1;
		break;
	case Part::DAY_OF_YEAR:
		result = get(CAL_DAY_OF_YEAR);
		break;
	case Part::HOUR:
		result = get(CAL_HOUR_OF_DAY);
		break;
	case Part::MINUTE:
		result = get(CAL_MINUTE);
		break;
	case Part::SECOND:
		result = get(CAL_SECOND);
		break;
	case Part::MILLISECOND:
		result = get(CAL_SECOND) * MILLIS_PER_SECOND + get(CAL_MILLISECOND);
		break;
	case Part::MICROSECOND:
		result = (get(CAL_SECOND) * MILLIS_PER_SECOND + get(CAL_MILLISECOND)) * MICROS_PER_MILLI +
		         FloorMod(instant, MICROS_PER_MILLI);
		break;
	case Part::ZONE_OFFSET:
		result = (get(CAL_ZONE_OFFSET) + get(CAL_DST_OFFSET)) / MILLIS_PER_SECOND;
		break;
	}
	return !cal.HasFailed();
}

bool Calendar::Format(int64_t instant, std::string &result) {
	int64_t local;
	int64_t offset;
	if (!ToLocal(instant, local) || !GetPart(instant, Part::ZONE_OFFSET, offset)) {
		return false;
	}
	int64_t year;
	int32_t month;
	int32_t day;
	CivilFromDays(FloorDiv(local, MICROS_PER_DAY), year, month, day);
	if (year < 1 || year > 9999) {
		return false;
	}
	const auto time_of_day = FloorMod(local, MICROS_PER_DAY);
	const auto seconds = time_of_day / 1000000;
	const auto micros = time_of_day % 1000000;
	char buffer[64];
	int length =
	    snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d", static_cast<int>(year), month, day,
	             static_cast<int>(seconds / 3600), static_cast<int>(seconds / 60 % 60), static_cast<int>(seconds % 60));
	result.assign(buffer, length);
	if (micros != 0) {
		length = snprintf(buffer, sizeof(buffer), ".%06d", static_cast<int>(micros));
		while (buffer[length - 1] == '0') {
			length--;
		}
		result.append(buffer, length);
	}
	const auto abs_offset = offset < 0 ? -offset : offset;
	result += offset < 0 ? '-' : '+';
	length = snprintf(buffer, sizeof(buffer), "%02d", static_cast<int>(abs_offset / 3600));
	result.append(buffer, length);
	if (abs_offset % 3600 != 0) {
		length = snprintf(buffer, sizeof(buffer), ":%02d", static_cast<int>(abs_offset / 60 % 60));
		result.append(buffer, length);
		if (abs_offset % 60 != 0) {
			length = snprintf(buffer, sizeof(buffer), ":%02d", static_cast<int>(abs_offset % 60));
			result.append(buffer, length);
		}
	}
	return true;
}

std::vector<ZoneRow> ListTimeZones() {
	const auto now = duckdb::datetime::Calendar::GetNow();
	std::vector<ZoneRow> rows;
	for (const auto &long_id : duckdb::datetime::TimeZone::GetAvailableIds()) {
		//	We don't have the zone tree for determining abbreviated names,
		//	so the SHORT name is the shortest, lexicographically first equivalent TZ without a slash.
		auto short_id = long_id;
		for (const auto &eid : duckdb::datetime::TimeZone::GetEquivalentIds(long_id)) {
			if (eid.find('/') != std::string::npos) {
				continue;
			}
			if (eid.size() < short_id.size() || (eid.size() == short_id.size() && eid < short_id)) {
				short_id = eid;
			}
		}
		auto tz = duckdb::datetime::TimeZone::TryCreate(long_id);
		int32_t raw_offset_ms;
		int32_t dst_offset_ms;
		tz->GetOffset(now, raw_offset_ms, dst_offset_ms);

		ZoneRow row;
		row.name = long_id;
		row.abbrev = short_id;
		row.utc_offset_seconds = (int64_t(raw_offset_ms) + dst_offset_ms) / MILLIS_PER_SECOND;
		row.is_dst = dst_offset_ms != 0;
		rows.push_back(std::move(row));
	}
	return rows;
}

//----------------------------------------------------------------------------------------------------------------------
// Collation
//----------------------------------------------------------------------------------------------------------------------
struct Collation::Impl {
	explicit Impl(const std::string &name) : collator(name) {
	}
	explicit Impl(duckdb::collation::CollationSettings settings) : collator(settings) {
	}

	duckdb::collation::Collator collator;
};

struct Collation::Buffer::Impl {
	duckdb::collation::CollationBuffer buffer;
};

Collation::Buffer::Buffer() : impl(new Impl()) {
}

Collation::Buffer::~Buffer() = default;

Collation::Collation() = default;

Collation::~Collation() = default;

std::unique_ptr<Collation> Collation::Create(const std::string &name, const std::string &tag) {
	std::unique_ptr<Collation> result(new Collation());
	if (tag.empty()) {
		result->impl.reset(new Impl(name));
		return result;
	}
	// "und-u-ks-level1-kc-true": compare at the primary level, but keep case differences
	if (tag != "und-u-ks-level1-kc-true") {
		throw std::runtime_error("Unknown collation tag " + tag);
	}
	duckdb::collation::CollationSettings settings;
	settings.strength = duckdb::collation::CollationStrength::PRIMARY;
	settings.case_level = true;
	result->impl.reset(new Impl(settings));
	return result;
}

std::vector<std::string> Collation::Names() {
	return duckdb::collation::Collator::GetCollations();
}

void Collation::SortKey(const char *data, size_t size, Buffer &buffer, const char *&key, size_t &key_size) const {
	impl->collator.GetSortKey(data, size, buffer.impl->buffer);
	auto &result = buffer.impl->buffer.key;
	key = reinterpret_cast<const char *>(result.data());
	// the terminating null byte is not part of the key
	key_size = result.size() - 1;
}

} // namespace icu_bridge
