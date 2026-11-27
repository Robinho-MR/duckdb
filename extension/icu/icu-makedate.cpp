#include "icu-casts.hpp"
#include "icu-datefunc.hpp"
#include "icu-vector.hpp"

#include <cmath>

namespace duckdb_icu {

date_t ICUMakeDate::Operation(Calendar *calendar, timestamp_tz_t instant) {
	if (!IsFinite(instant)) {
		return Timestamp::GetDate(timestamp_t {instant.micros});
	}

	// Extract the time zone parts
	SetTime(calendar, instant);
	const auto era = ExtractField(calendar, CAL_ERA);
	const auto year = ExtractField(calendar, CAL_YEAR);
	const auto mm = ExtractField(calendar, CAL_MONTH) + 1;
	const auto dd = ExtractField(calendar, CAL_DATE);

	const auto yyyy = era ? year : (-year + 1);
	date_t result;
	if (!Date::TryFromDate(yyyy, mm, dd, result)) {
		throw ConversionException("Unable to convert TIMESTAMPTZ to DATE");
	}
	return result;
}

date_t ICUMakeDate::ToDate(cxx::Context &context, timestamp_tz_t instant) {
	ICUDateFunc::BindData data(context);
	return Operation(data.calendar.get(), instant);
}

static void CastToDate(cxx::CastFunction::ExecInput &input) {
	auto &info = input.GetBindData<ICUDateFunc::BindData>();
	CalendarPtr calendar(info.calendar->Copy());
	ExecuteCast<timestamp_tz_t, date_t>(input, [&](timestamp_tz_t value, date_t &result, string &error) {
		result = ICUMakeDate::Operation(calendar.get(), value);
		return true;
	});
}

static void CastNsToDate(cxx::CastFunction::ExecInput &input) {
	auto &info = input.GetBindData<ICUDateFunc::BindData>();
	CalendarPtr calendar(info.calendar->Copy());
	ExecuteCast<timestamp_tz_ns_t, date_t>(input, [&](timestamp_tz_ns_t value, date_t &result, string &error) {
		if (!IsFinite(value)) {
			result = Timestamp::GetDate(timestamp_t {value.nanos});
			return true;
		}
		// the date only needs the microsecond instant: floor the nanoseconds so no instant moves past midnight
		const auto nanos = value.nanos;
		auto micros = nanos / Interval::NANOS_PER_MICRO;
		if (nanos % Interval::NANOS_PER_MICRO < 0) {
			micros--;
		}
		result = ICUMakeDate::Operation(calendar.get(), timestamp_tz_t {micros});
		return true;
	});
}

static void BindCastToDate(cxx::CastFunction::BindInput &input) {
	ICUCasts::BindCastData(input, "TIMESTAMPTZ to DATE",
	                       "Casting from TIMESTAMP WITH TIME ZONE to DATE without an explicit time zone "
	                       "has been disabled  - use \"AT TIME ZONE ...\"");
}

void ICUMakeDate::AddCasts(const ICURegistration &reg) {
	ICUCasts::Register(reg, "TIMESTAMPTZ", "DATE", false, BindCastToDate, CastToDate);
	ICUCasts::Register(reg, "TIMESTAMPTZ_NS", "DATE", false, BindCastToDate, CastNsToDate);
}

struct ICUMakeTimestampTZFunc : public ICUDateFunc {
	//! The engine's cast of a BIGINT to an INTEGER
	static int32_t CastToInt32(int64_t value) {
		if (value < NumericLimits<int32_t>::Minimum() || value > NumericLimits<int32_t>::Maximum()) {
			throw InvalidInputException("Type INT64 with value " + std::to_string(value) +
			                            " can't be cast because the value is out of range for the destination type "
			                            "INT32");
		}
		return int32_t(value);
	}

	//! The engine's cast of a DOUBLE to an INTEGER: it rounds, and the rounded value must fit
	static int32_t CastToInt32(cxx::Context &context, double value) {
		auto rounded = std::nearbyint(value);
		if (!std::isfinite(value) || !(rounded >= -2147483648.0 && rounded < 2147483648.0)) {
			auto text = cxx::Value::Create(context.GetFactory(), value).ToText();
			throw InvalidInputException("Type DOUBLE with value " + text +
			                            " can't be cast because the value is out of range for the destination type "
			                            "INT32");
		}
		return int32_t(rounded);
	}

	static inline timestamp_tz_t Operation(cxx::Context &context, Calendar *calendar, int64_t yyyy, int64_t mm,
	                                       int64_t dd, int64_t hr, int64_t mn, double ss) {
		const auto year = CastToInt32(yyyy + (yyyy < 0));
		const auto month = CastToInt32(SubtractOverflowCheck(mm, int64_t(1)));
		const auto day = CastToInt32(dd);
		const auto hour = CastToInt32(hr);
		const auto min = CastToInt32(mn);

		const auto secs = CastToInt32(context, ss);
		ss -= secs;
		ss *= Interval::MSECS_PER_SEC;
		const auto millis = int32_t(ss);
		int64_t micros = int64_t(std::round((ss - millis) * Interval::MICROS_PER_MSEC));

		calendar->Set(CAL_YEAR, year);
		calendar->Set(CAL_MONTH, month);
		calendar->Set(CAL_DATE, day);
		calendar->Set(CAL_HOUR_OF_DAY, hour);
		calendar->Set(CAL_MINUTE, min);
		calendar->Set(CAL_SECOND, secs);
		calendar->Set(CAL_MILLISECOND, millis);

		return GetTime(calendar, uint64_t(micros));
	}

	static void FromMicros(cxx::ScalarFunction::ExecInput &input) {
		ExecuteRows<timestamp_tz_t, int64_t>(input, [&](int64_t micros) -> std::optional<timestamp_tz_t> {
			const timestamp_tz_t result {micros};
			if (!IsFinite(result)) {
				throw ConversionException("Timestamp microseconds out of range: %ld", micros);
			}
			return result;
		});
	}

	static void Execute(cxx::ScalarFunction::ExecInput &input) {
		auto &cache = input.GetInitData<CalendarCacheState>();
		auto &context = input.GetContext();
		auto calendar = cache.GetBaseCalendar();
		ExecuteRows<timestamp_tz_t, int64_t, int64_t, int64_t, int64_t, int64_t, double>(
		    input, [&](int64_t yyyy, int64_t mm, int64_t dd, int64_t hr, int64_t mn, double ss) {
			    return std::optional<timestamp_tz_t>(Operation(context, calendar, yyyy, mm, dd, hr, mn, ss));
		    });
	}

	static void ExecuteZone(cxx::ScalarFunction::ExecInput &input) {
		auto &cache = input.GetInitData<CalendarCacheState>();
		auto &context = input.GetContext();
		ExecuteRows<timestamp_tz_t, int64_t, int64_t, int64_t, int64_t, int64_t, double, std::string_view>(
		    input, [&](int64_t yyyy, int64_t mm, int64_t dd, int64_t hr, int64_t mn, double ss, std::string_view tz) {
			    return std::optional<timestamp_tz_t>(
			        Operation(context, cache.GetCalendar(tz), yyyy, mm, dd, hr, mn, ss));
		    });
	}

	static void AddFunction(const ICURegistration &reg, const string &name) {
		vector<pair<string, string>> parameters {{"year", "BIGINT"}, {"month", "BIGINT"},  {"day", "BIGINT"},
		                                         {"hour", "BIGINT"}, {"minute", "BIGINT"}, {"seconds", "DOUBLE"}};
		auto senary = reg.Scalar(name, parameters, "TIMESTAMPTZ");
		senary.SetBindCallback(Bind).SetInitCallback(InitCalendarCache).SetExecCallback(Execute);
		senary.SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
		reg.extension.Register(senary);

		parameters.emplace_back("timezone", "VARCHAR");
		auto septenary = reg.Scalar(name, parameters, "TIMESTAMPTZ");
		septenary.SetBindCallback(Bind).SetInitCallback(InitCalendarCache).SetExecCallback(ExecuteZone);
		septenary.SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
		reg.extension.Register(septenary);

		auto from_micros = reg.Scalar(name, {{"micros", "BIGINT"}}, "TIMESTAMPTZ");
		from_micros.SetExecCallback(FromMicros);
		reg.extension.Register(from_micros);
	}
};

void RegisterICUMakeDateFunctions(const ICURegistration &reg) {
	ICUMakeTimestampTZFunc::AddFunction(reg, "make_timestamptz");
	ICUMakeDate::AddCasts(reg);
}

//===--------------------------------------------------------------------===//
// get_current_time, current_date, today
//===--------------------------------------------------------------------===//
static void CurrentTimeFunction(cxx::ScalarFunction::ExecInput &input) {
	auto &context = input.GetContext();
	const auto instant = context.GetTransactionStartTime();
	ICUDateFunc::BindData data(context);
	dtime_tz_t result_time(0, 0);
	ICUToTimeTZ::ToTimeTZ(data.calendar.get(), instant, result_time);
	const auto bits = result_time.GetBits();
	ExecuteRows<uint64_t>(input, [&]() -> std::optional<uint64_t> { return bits; });
}

static void CurrentDateFunction(cxx::ScalarFunction::ExecInput &input) {
	auto &context = input.GetContext();
	const auto date = ICUMakeDate::ToDate(context, context.GetTransactionStartTime());
	ExecuteRows<date_t>(input, [&]() -> std::optional<date_t> { return date; });
}

void RegisterICUCurrentFunctions(const ICURegistration &reg) {
	auto current_time = reg.Scalar("get_current_time", {}, "TIMETZ");
	current_time.SetExecCallback(CurrentTimeFunction);
	current_time.SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
	reg.extension.Register(current_time);

	for (auto name : {"current_date", "today"}) {
		auto current_date = reg.Scalar(name, {}, "DATE");
		current_date.SetExecCallback(CurrentDateFunction);
		current_date.SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
		reg.extension.Register(current_date);
	}
}

} // namespace duckdb_icu
