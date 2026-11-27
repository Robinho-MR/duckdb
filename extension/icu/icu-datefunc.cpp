#include "icu-datefunc.hpp"

namespace duckdb_icu {

//===--------------------------------------------------------------------===//
// Settings
//===--------------------------------------------------------------------===//
static bool TryReadSetting(cxx::Context &context, std::string_view name, string &result) {
	auto option = context.GetConfig().TryGetOption(name);
	if (!option || option->value.IsNull()) {
		return false;
	}
	result = string(option->value.Get<cxx::varchar_t>().view());
	return true;
}

void ICUDateFunc::ReadSettings(cxx::Context &context, string &tz_setting, string &cal_setting) {
	tz_setting.clear();
	cal_setting = "gregorian";
	TryReadSetting(context, "TimeZone", tz_setting);
	TryReadSetting(context, "Calendar", cal_setting);
}

ICUDateFunc::BindData::BindData(const BindData &other)
    : tz_setting(other.tz_setting), cal_setting(other.cal_setting), calendar(other.calendar->Copy()) {
}

ICUDateFunc::BindData::BindData(const string &tz_setting_p, const string &cal_setting_p)
    : tz_setting(tz_setting_p), cal_setting(cal_setting_p) {
	InitCalendar();
}

ICUDateFunc::BindData::BindData(cxx::Context &context) {
	ReadSettings(context, tz_setting, cal_setting);
	InitCalendar();
}

void ICUDateFunc::BindData::InitCalendar() {
	//	Postgres always assumes times are given in the proleptic Gregorian calendar,
	//	which is what our Gregorian calendar implements.
	auto tz = TimeZone::TryCreate(tz_setting);
	calendar = Calendar::TryCreate(cal_setting, tz ? std::move(tz) : TimeZone::TryCreate("UTC"));
	if (!calendar) {
		throw InternalException("Unable to create calendar.");
	}
}

void ICUDateFunc::Bind(cxx::ScalarFunction::BindInput &input) {
	input.SetBindData<BindData>(input.GetContext());
}

//! Whether -infinity lies between the bounds of the statistics of a timestamp
static bool HoldsNegativeInfinity(const cxx::Stats &stats) {
	auto value = stats.GetValue();
	auto type = value.GetLogicalType();
	std::optional<cxx::Value> min;
	std::optional<cxx::Value> max;
	for (idx_t i = 0; i < value.GetChildCount(); i++) {
		auto name = type.GetStructChildName(i);
		if (name == "min") {
			min = value.GetChild(i);
		} else if (name == "max") {
			max = value.GetChild(i);
		}
	}
	if (!min || !max || min->IsNull() || max->IsNull()) {
		return false;
	}
	int64_t lo;
	int64_t hi;
	switch (min->GetLogicalType().GetTypeId()) {
	case cxx::LogicalTypeId::TIMESTAMP_TZ:
		lo = min->Get<timestamp_tz_t>().micros;
		hi = max->Get<timestamp_tz_t>().micros;
		break;
	case cxx::LogicalTypeId::TIMESTAMP_TZ_NS:
		lo = min->Get<timestamp_tz_ns_t>().nanos;
		hi = max->Get<timestamp_tz_ns_t>().nanos;
		break;
	default:
		return false;
	}
	return lo < hi && lo <= TIMESTAMP_NINFINITY && TIMESTAMP_NINFINITY <= hi;
}

void ICUDateFunc::PropagateFiniteMonotoneBounds(cxx::ScalarFunction::StatsInput &input) {
	// -infinity gives NULL, and it can lie between the bounds of an argument
	for (idx_t i = 0; i < input.GetArgCount(); i++) {
		if (HoldsNegativeInfinity(input.GetArgStats(i))) {
			return;
		}
	}
	input.PropagateMonotoneBounds();
}

void ICUDateFunc::InitCalendarCache(cxx::ScalarFunction::InitInput &input) {
	auto &info = input.GetBindData<BindData>();
	input.SetInitData<CalendarCacheState>(*info.calendar);
}

//===--------------------------------------------------------------------===//
// Time zones
//===--------------------------------------------------------------------===//
static unique_ptr<TimeZone> GetKnownTimeZone(const string &tz_str) {
	return TimeZone::TryCreate(tz_str);
}

static unique_ptr<TimeZone> GetNormalizedTimeZone(string &tz_str) {
	auto tz = GetKnownTimeZone(tz_str);
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

		idx_t pos = 3;
		const auto utc = tz_str[pos++];
		// Invert the sign (UTC and Etc use opposite sign conventions)
		// https://en.wikipedia.org/wiki/Tz_database#Area
		auto sign = utc;
		if (utc == '+') {
			sign = '-';
		} else if (utc == '-') {
			sign = '+';
		} else {
			break;
		}

		// Collect remaining characters (digits and colons)
		string remainder;
		for (; pos < tz_str.size(); ++pos) {
			const auto ch = tz_str[pos];
			if (ch != ':' && !StringUtil::CharacterIsDigit(ch)) {
				break;
			}
			remainder += ch;
		}
		if (pos < tz_str.size()) {
			break;
		}

		// Step 1: Strip leading zeros
		idx_t start = 0;
		while (start < remainder.size() && remainder[start] == '0') {
			++start;
		}
		remainder = remainder.substr(start);

		// Step 2: Parse hours based on whether colon is present
		string hours_str;
		auto colon_idx = remainder.find(':');
		if (colon_idx != string::npos) {
			// Has colon: split by colon, part before colon is hours
			hours_str = remainder.substr(0, colon_idx);
		} else if (remainder.size() <= 2) {
			// 1-2 digits: entire string is hours
			hours_str = remainder;
		} else {
			// No colon, 3+ digits: HHMM format, last 2 are minutes, rest are hours
			hours_str = remainder.substr(0, remainder.size() - 2);
		}

		// Build the mapped timezone string
		string mapped = "Etc/GMT";
		if (hours_str.empty()) {
			// Zero offset
			mapped += "+0";
		} else {
			mapped += sign;
			mapped += hours_str;
		}
		// Final sanity check
		tz = GetKnownTimeZone(mapped);
		if (tz) {
			tz_str = mapped;
			return tz;
		}
	} while (false);

	return nullptr;
}

static unique_ptr<TimeZone> GetTimeZoneInternal(string &tz_str, vector<string> &candidates) {
	auto tz = GetNormalizedTimeZone(tz_str);
	if (tz) {
		return tz;
	}

	// Try to be friendlier
	// Go through all the zone names and look for a case insensitive match
	// If we don't find one, make a suggestion
	for (const auto &candidate : TimeZone::GetAvailableIds()) {
		if (StringUtil::CIEquals(candidate, tz_str)) {
			// case insensitive match - return this timezone instead
			tz_str = candidate;
			return TimeZone::TryCreate(tz_str);
		}
		candidates.emplace_back(candidate);
	}
	return nullptr;
}

unique_ptr<TimeZone> ICUHelpers::GetTimeZone(string &tz_str, string *error_message) {
	vector<string> candidates;
	auto tz = GetTimeZoneInternal(tz_str, candidates);
	if (tz) {
		return tz;
	}
	string candidate_str =
	    StringUtil::CandidatesMessage(StringUtil::TopNJaroWinkler(candidates, tz_str), "Candidate time zones");
	if (error_message) {
		*error_message = "Unknown TimeZone '" + tz_str + "'!\n" + candidate_str;
		return nullptr;
	}
	throw NotImplementedException("Unknown TimeZone '%s'!\n%s", tz_str, candidate_str);
}

TimestampComponents ICUHelpers::GetComponents(timestamp_tz_t ts, Calendar *calendar) {
	// Get the parts in the given time zone
	uint64_t micros = ICUDateFunc::SetTime(calendar, ts);

	TimestampComponents ts_data;
	ts_data.year = ICUDateFunc::ExtractField(calendar, CAL_EXTENDED_YEAR);
	ts_data.month = ICUDateFunc::ExtractField(calendar, CAL_MONTH) + 1;
	ts_data.day = ICUDateFunc::ExtractField(calendar, CAL_DATE);

	ts_data.hour = ICUDateFunc::ExtractField(calendar, CAL_HOUR_OF_DAY);
	ts_data.minute = ICUDateFunc::ExtractField(calendar, CAL_MINUTE);
	ts_data.second = ICUDateFunc::ExtractField(calendar, CAL_SECOND);
	ts_data.microsecond =
	    int32_t(ICUDateFunc::ExtractField(calendar, CAL_MILLISECOND) * Interval::MICROS_PER_MSEC + int64_t(micros));
	ts_data.nanosecond = 0;
	return ts_data;
}

TimestampComponents ICUHelpers::GetComponents(timestamp_tz_ns_t tsns, Calendar *calendar) {
	// Get the parts in the given time zone
	int64_t micros;
	int64_t nanos;
	Timestamp::SplitTime(tsns.nanos, Interval::NANOS_PER_MICRO, micros, nanos);

	auto ts_data = GetComponents(timestamp_tz_t {micros}, calendar);
	ts_data.nanosecond = int32_t(nanos);
	return ts_data;
}

//===--------------------------------------------------------------------===//
// Calendar cache
//===--------------------------------------------------------------------===//
const ICUDateFunc::CalendarCacheState::CacheEntry &ICUDateFunc::CalendarCacheState::GetEntry(std::string_view tz_id) {
	if (last && last->first == tz_id) {
		return last->second;
	}
	string tz_str(tz_id);
	auto entry = cache.find(tz_str);
	if (entry == cache.end()) {
		CacheEntry new_entry;
		//	GetTimeZone may normalize the name - keep the original as the cache key
		auto resolved = tz_str;
		new_entry.tz = ICUHelpers::GetTimeZone(resolved, &new_entry.error);
		if (new_entry.tz) {
			new_entry.calendar = base_calendar->Copy();
			new_entry.calendar->SetTimeZone(new_entry.tz->Copy());
		}
		entry = cache.emplace(std::move(tz_str), std::move(new_entry)).first;
	}
	last = &*entry;
	return entry->second;
}

Calendar *ICUDateFunc::CalendarCacheState::GetCalendar(std::string_view tz_id) {
	auto &entry = GetEntry(tz_id);
	if (!entry.calendar) {
		throw NotImplementedException(entry.error);
	}
	return entry.calendar.get();
}

Calendar *ICUDateFunc::CalendarCacheState::TryGetCalendar(std::string_view tz_id, string *error_message) {
	auto &entry = GetEntry(tz_id);
	if (!entry.calendar) {
		if (error_message) {
			*error_message = entry.error;
		}
		return nullptr;
	}
	return entry.calendar.get();
}

void ICUDateFunc::CalendarCacheState::SetTimeZone(Calendar *calendar, std::string_view tz_id, string *error_message) {
	auto &entry = GetEntry(tz_id);
	if (!entry.tz) {
		if (error_message) {
			*error_message = entry.error;
			return;
		}
		throw NotImplementedException(entry.error);
	}
	calendar->SetTimeZone(entry.tz->Copy());
}

bool ICUDateFunc::CalendarCacheState::TrySetTimeZone(Calendar *calendar, std::string_view tz_id) {
	auto &entry = GetEntry(tz_id);
	if (!entry.tz) {
		return false;
	}
	calendar->SetTimeZone(entry.tz->Copy());
	return true;
}

//===--------------------------------------------------------------------===//
// Calendar time
//===--------------------------------------------------------------------===//
timestamp_tz_t ICUDateFunc::GetTimeUnsafe(Calendar *calendar, uint64_t micros) {
	// Extract the new time
	const auto millis = calendar->GetTime();
	if (calendar->HasFailed()) {
		throw InternalException("Unable to get calendar time.");
	}
	// The time is a whole number of milliseconds, so converting it back to microseconds can overflow.
	int64_t result;
	if (!Timestamp::TryCombineUnits(millis, int64_t(micros), Interval::MICROS_PER_MSEC, result)) {
		throw OutOfRangeException("Unable to get calendar time: timestamp out of range");
	}
	return timestamp_tz_t {result};
}

bool ICUDateFunc::TryGetTime(Calendar *calendar, uint64_t micros, timestamp_tz_t &result) {
	// Extract the new time
	auto millis = calendar->GetTime();
	if (calendar->HasFailed()) {
		return false;
	}

	// The time is a whole number of milliseconds, so converting it back to microseconds can overflow.
	if (!Timestamp::TryCombineUnits(millis, int64_t(micros), Interval::MICROS_PER_MSEC, result.micros)) {
		return false;
	}
	return IsFinite(result);
}

timestamp_tz_t ICUDateFunc::GetTime(Calendar *calendar, uint64_t micros) {
	timestamp_tz_t result;
	if (!TryGetTime(calendar, micros, result)) {
		throw ConversionException("ICU date overflows timestamp range");
	}
	return result;
}

bool ICUDateFunc::TryGetTimeNS(Calendar *calendar, uint64_t nanos, timestamp_tz_ns_t &result) {
	timestamp_tz_t tstz_micros;
	if (!TryGetTime(calendar, nanos / Interval::NANOS_PER_MICRO, tstz_micros)) {
		return false;
	}

	nanos %= Interval::NANOS_PER_MICRO;
	timestamp_ns_t ns;
	if (!Timestamp::TryFromTimestampNanos(timestamp_t {tstz_micros.micros}, int32_t(nanos), ns)) {
		return false;
	}
	result.nanos = ns.nanos;
	return true;
}

timestamp_tz_ns_t ICUDateFunc::GetTimeNS(Calendar *calendar, uint64_t nanos) {
	timestamp_tz_ns_t result;
	if (!TryGetTimeNS(calendar, nanos, result)) {
		throw ConversionException("ICU date overflows timestamp_ns range");
	}
	return result;
}

uint64_t ICUDateFunc::SetTime(Calendar *calendar, timestamp_tz_t date) {
	int64_t millis = date.micros / Interval::MICROS_PER_MSEC;
	int64_t micros = date.micros % Interval::MICROS_PER_MSEC;
	if (micros < 0) {
		--millis;
		micros += Interval::MICROS_PER_MSEC;
	}

	calendar->SetTime(millis);
	return uint64_t(micros);
}

uint64_t ICUDateFunc::SetTimeNS(Calendar *calendar, timestamp_tz_ns_t date) {
	int64_t millis = date.nanos / Interval::NANOS_PER_MSEC;
	int64_t nanos = date.nanos % Interval::NANOS_PER_MSEC;
	if (nanos < 0) {
		--millis;
		nanos += Interval::NANOS_PER_MSEC;
	}

	calendar->SetTime(millis);
	return uint64_t(nanos);
}

int32_t ICUDateFunc::ExtractField(Calendar *calendar, CalendarField field) {
	const auto result = calendar->Get(field);
	// a date the calendar cannot represent is something the query asked for, not a defect
	if (calendar->HasFailed()) {
		throw ConversionException("Unable to extract calendar part");
	}
	return result;
}

bool ICUDateFunc::TryGetTimeTZOffset(Calendar *calendar, int32_t &offset) {
	offset = ExtractField(calendar, CAL_ZONE_OFFSET);
	offset += ExtractField(calendar, CAL_DST_OFFSET);
	offset /= Interval::MSECS_PER_SEC;
	return offset >= -dtime_tz_t::MAX_OFFSET && offset <= dtime_tz_t::MAX_OFFSET;
}

int32_t ICUDateFunc::GetTimeTZOffset(Calendar *calendar) {
	int32_t offset;
	if (!TryGetTimeTZOffset(calendar, offset)) {
		throw OutOfRangeException(TimeTZOffsetError(offset));
	}
	return offset;
}

string ICUDateFunc::TimeTZOffsetError(int32_t offset) {
	auto interval = Interval::Make(0, 0, offset * Interval::MICROS_PER_SEC);
	return StringUtil::Format("Time zone offset %s is out of range, expected a value between -15:59:59 and +15:59:59",
	                          Interval::ToString(interval));
}

int32_t ICUDateFunc::SubtractField(Calendar *calendar, CalendarField field, timestamp_tz_t end_date) {
	const int64_t millis = end_date.micros / Interval::MICROS_PER_MSEC;
	const auto sub = calendar->FieldDifference(millis, field);
	if (calendar->HasFailed()) {
		throw ConversionException("Unable to subtract calendar part");
	}
	return sub;
}

} // namespace duckdb_icu
