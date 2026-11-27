//===----------------------------------------------------------------------===//
//                         DuckDB
//
// icu-datefunc.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb_cpp.hpp"
#include "icu-datetime.hpp"
#include "tz_calendar.hpp"

#include <functional>

namespace duckdb_icu {

namespace cxx = duckdb::cxx;

//! What the ICU functions register through: the extension, and the factory of its database
struct ICURegistration {
	cxx::Extension &extension;
	cxx::Factory &factory;

	cxx::LogicalType Type(std::string_view name) const {
		if (name == "ANY") {
			return factory.CreateType(cxx::LogicalTypeId::ANY);
		}
		return factory.ParseType(name);
	}

	//! A scalar function with a name and a signature, for the caller to give its callbacks and register
	cxx::ScalarFunction Scalar(const string &name, const vector<pair<string, string>> &parameters,
	                           const string &return_type) const {
		auto function = cxx::ScalarFunction::Create(factory);
		function.SetName(name);
		auto signature = function.GetSignature();
		for (auto &parameter : parameters) {
			signature.AddParameter(parameter.first, Type(parameter.second));
		}
		signature.SetReturnType(Type(return_type));
		return function;
	}
};

struct ICUHelpers {
	//! Gets a time zone, normalizing its name - throws an error if the time zone is not found, unless `error_message`
	//! is given, in which case it receives the error and nullptr is returned
	static unique_ptr<TimeZone> GetTimeZone(string &tz_str, string *error_message = nullptr);
	static TimestampComponents GetComponents(timestamp_tz_t ts, Calendar *calendar);
	static TimestampComponents GetComponents(timestamp_tz_ns_t ts, Calendar *calendar);
};

struct ICUDateFunc {
	//! The time zone and calendar settings a call reads when it is bound, and the calendar they make
	struct BindData {
		explicit BindData(cxx::Context &context);
		BindData(const string &tz_setting, const string &cal_setting);
		BindData(const BindData &other);

		string tz_setting;
		string cal_setting;
		CalendarPtr calendar;

		bool operator==(const BindData &other) const {
			return calendar->Equals(*other.calendar);
		}

		void InitCalendar();
	};

	//! The settings of a context: the session's time zone and calendar
	static void ReadSettings(cxx::Context &context, string &tz_setting, string &cal_setting);

	//! Per-thread cache of resolved time zones. Resolving a time zone name looks the name up and
	//! then builds a zone and a calendar for it, so functions with per-row time zone arguments
	//! must resolve each distinct zone only once per thread.
	struct CalendarCacheState {
		struct CacheEntry {
			//! The resolved time zone (nullptr if the name is invalid)
			unique_ptr<TimeZone> tz;
			//! Copy of the base calendar with the time zone applied (nullptr if the name is invalid)
			CalendarPtr calendar;
			//! The resolution error for invalid time zone names
			string error;
		};

		explicit CalendarCacheState(Calendar &base) : base_calendar(base.Copy()) {
		}

		//! Returns a calendar for the time zone. Throws for unknown zones.
		Calendar *GetCalendar(std::string_view tz_id);
		//! Returns a calendar for the time zone, or nullptr for unknown zones (filling error_message if provided).
		Calendar *TryGetCalendar(std::string_view tz_id, string *error_message = nullptr);
		//! Sets the time zone for the calendar.
		//! For unknown zones, fills error_message if provided and throws otherwise.
		void SetTimeZone(Calendar *calendar, std::string_view tz_id, string *error_message = nullptr);
		//! Sets the time zone for the calendar and returns false if it is not valid.
		bool TrySetTimeZone(Calendar *calendar, std::string_view tz_id);

		Calendar *GetBaseCalendar() {
			return base_calendar.get();
		}

	private:
		const CacheEntry &GetEntry(std::string_view tz_id);

		CalendarPtr base_calendar;
		unordered_map<string, CacheEntry> cache;
		//! Exploit locality for timestamp rows
		const std::pair<const string, CacheEntry> *last = nullptr;
	};

	//! Binds a default calendar object for use by the function
	static void Bind(cxx::ScalarFunction::BindInput &input);
	//! The statistics of a monotone function, as the engine derives them from the bounds of the arguments, except
	//! when -infinity lies between the bounds of an argument: the result is NULL for it
	static void PropagateFiniteMonotoneBounds(cxx::ScalarFunction::StatsInput &input);
	//! Initializes a CalendarCacheState from the function's BindData
	static void InitCalendarCache(cxx::ScalarFunction::InitInput &input);

	//! Gets the timestamp from the calendar, returning false if it is not in range.
	static bool TryGetTime(Calendar *calendar, uint64_t micros, timestamp_tz_t &result);
	//! Gets the timestamp from the calendar, throwing if it is not in range.
	static timestamp_tz_t GetTime(Calendar *calendar, uint64_t micros = 0);
	//! Gets the timestamp from the calendar, returning false if it is not in range.
	static bool TryGetTimeNS(Calendar *calendar, uint64_t nanos, timestamp_tz_ns_t &result);
	//! Gets the timestamp from the calendar, throwing if it is not in range.
	static timestamp_tz_ns_t GetTimeNS(Calendar *calendar, uint64_t nanos = 0);
	//! Gets the timestamp from the calendar, assuming it is in range.
	static timestamp_tz_t GetTimeUnsafe(Calendar *calendar, uint64_t micros = 0);
	//! Sets the calendar to the timestamp, returning the unused µs part
	static uint64_t SetTime(Calendar *calendar, timestamp_tz_t date);
	//! Sets the calendar to the timestamp, returning the unused ns part
	static uint64_t SetTimeNS(Calendar *calendar, timestamp_tz_ns_t date);
	//! Extracts the field from the calendar
	static int32_t ExtractField(Calendar *calendar, CalendarField field);
	//! Extracts the UTC offset (in seconds) of the calendar, returns false if a TIMETZ cannot represent it
	static bool TryGetTimeTZOffset(Calendar *calendar, int32_t &offset);
	//! Extracts the UTC offset (in seconds) of the calendar, throws if a TIMETZ cannot represent it
	static int32_t GetTimeTZOffset(Calendar *calendar);
	static string TimeTZOffsetError(int32_t offset);
	//! Subtracts the field of the given date from the calendar
	static int32_t SubtractField(Calendar *calendar, CalendarField field, timestamp_tz_t end_date);
	//! Adds the timestamp and the interval using the calendar
	static timestamp_tz_t Add(TZCalendar &calendar, timestamp_tz_t timestamp, interval_t interval);
	//! Subtracts the interval from the timestamp using the calendar
	static timestamp_tz_t Sub(TZCalendar &calendar, timestamp_tz_t timestamp, interval_t interval);
	//! Subtracts the latter timestamp from the former timestamp using the calendar
	static interval_t Sub(TZCalendar &calendar, timestamp_tz_t end_date, timestamp_tz_t start_date);
	//! Pulls out the bin values from the timestamp assuming it is an instant,
	//! constructs an ICU timestamp, and then converts that back to a DuckDB instant
	//! Adding offset doesn't really work around DST because the bin values are ambiguous
	static timestamp_tz_t FromNaive(Calendar *calendar, timestamp_t naive);
	//! Non-throwing form of FromNaive - sets the error and returns false if the result is out of range
	static bool TryFromNaive(Calendar *calendar, timestamp_t naive, timestamp_tz_t &result, string &error);

	//! Truncates the calendar time to the given part precision
	typedef void (*part_trunc_t)(Calendar *calendar, uint64_t &micros);
	static part_trunc_t TruncationFactory(DatePartSpecifier part);
	//! Midnight of the start of the context's transaction, in the calendar
	static timestamp_tz_t CurrentMidnight(Calendar *calendar, cxx::Context &context);

	//! Subtracts the two times at the given part precision
	typedef int64_t (*part_sub_t)(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date);
	static part_sub_t SubtractFactory(DatePartSpecifier part);
};

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//
void RegisterICUCurrentFunctions(const ICURegistration &reg);
void RegisterICUDateAddFunctions(const ICURegistration &reg);
void RegisterICUDatePartFunctions(const ICURegistration &reg);
void RegisterICUDateSubFunctions(const ICURegistration &reg);
void RegisterICUDateTruncFunctions(const ICURegistration &reg);
void RegisterICUMakeDateFunctions(const ICURegistration &reg);
void RegisterICUTableRangeFunctions(const ICURegistration &reg);
void RegisterICUListRangeFunctions(const ICURegistration &reg);
void RegisterICUStrptimeFunctions(const ICURegistration &reg);
void RegisterICUTimeBucketFunctions(const ICURegistration &reg);
void RegisterICUTimeZoneFunctions(const ICURegistration &reg);

} // namespace duckdb_icu
