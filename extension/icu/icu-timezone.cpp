#include "icu-casts.hpp"
#include "icu-datefunc.hpp"
#include "icu-vector.hpp"

namespace duckdb_icu {

//===--------------------------------------------------------------------===//
// Cast helpers
//===--------------------------------------------------------------------===//
bool ICUCasts::TimestampTZCastsDisabled(cxx::Context &context) {
	auto option = context.GetConfig().TryGetOption("disable_timestamptz_casts");
	return option && !option->value.IsNull() && option->value.Get<bool>();
}

void ICUCasts::BindCastData(cxx::CastFunction::BindInput &input, const char *what, const string &disabled_message) {
	if (!input.HasContext()) {
		throw InternalException("Missing context for %s cast.", what);
	}
	auto &context = input.GetContext();
	if (!disabled_message.empty() && TimestampTZCastsDisabled(context)) {
		throw BinderException(disabled_message);
	}
	input.SetBindData<ICUDateFunc::BindData>(context);
}

void ICUCasts::InitCalendarCache(cxx::CastFunction::InitInput &input) {
	auto &info = input.GetBindData<ICUDateFunc::BindData>();
	input.SetInitData<ICUDateFunc::CalendarCacheState>(*info.calendar);
}

void ICUCasts::Register(const ICURegistration &reg, const string &source, const string &target, bool implicit,
                        cxx::CastFunction::BindCallback bind, cxx::CastFunction::ExecCallback exec,
                        cxx::CastFunction::InitCallback init) {
	auto source_type = reg.Type(source);
	auto target_type = reg.Type(target);
	auto cast = cxx::CastFunction::Create(reg.factory);
	cast.SetSourceType(source_type).SetTargetType(target_type);
	if (implicit) {
		cast.SetImplicitCastCost(cxx::CastFunction::GetImplicitCastCost(reg.factory, source_type, target_type));
	}
	// the result depends on the TimeZone and Calendar settings
	cast.SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
	cast.SetBindCallback(bind).SetExecCallback(exec);
	if (init) {
		cast.SetInitCallback(init);
	}
	reg.extension.Register(cast);
}

//! The message of a failed conversion between two timestamp precisions, as the engine's casts write it
template <class SRC>
static string CastExceptionText(const SRC &input) {
	return "Type INT64 with value " + Timestamp::ToString(timestamp_t {Raw(input)}) +
	       " can't be cast to the destination type INT64";
}

static string CastExceptionText(const date_t &input) {
	return "Type INT32 with value " + Date::ToString(input) + " can't be cast to the destination type INT64";
}

//! The naive timestamp of the precision of a time zone type
template <class DST>
using NaiveOf =
    typename std::conditional<std::is_same<DST, timestamp_tz_ns_t>::value, timestamp_ns_t, timestamp_t>::type;

template <class SRC, class DST>
static bool TryCastToNaive(const SRC &input, DST &result) {
	return TryCastTimestamp(input, result);
}

template <class DST>
static bool TryCastToNaive(const date_t &input, DST &result) {
	return TryCastDate(input, result);
}

//===--------------------------------------------------------------------===//
// pg_timezone_names
//===--------------------------------------------------------------------===//
struct ICUTimeZoneData {
	ICUTimeZoneData() : now(Calendar::GetNow()) {
	}

	idx_t offset = 0;
	int64_t now;
};

static void ICUTimeZoneBind(cxx::TableFunction::BindInput &input) {
	auto &factory = input.GetContext().GetFactory();
	input.AddResultColumn("name", factory.ParseType("VARCHAR"));
	input.AddResultColumn("abbrev", factory.ParseType("VARCHAR"));
	input.AddResultColumn("utc_offset", factory.ParseType("INTERVAL"));
	input.AddResultColumn("is_dst", factory.ParseType("BOOLEAN"));
}

static void ICUTimeZoneInit(cxx::TableFunction::InitGlobalInput &input) {
	input.SetGlobalState<ICUTimeZoneData>();
}

static void ICUTimeZoneFunction(cxx::TableFunction::ExecInput &input) {
	auto &data = input.GetGlobalState<ICUTimeZoneData>();
	const auto &names = TimeZone::GetAvailableIds();
	auto output = input.GetOutputChunk();
	const auto capacity = output.GetCapacity();

	auto name_col = output.GetVector(0);
	auto abbrev = output.GetVector(1);
	auto utc_offset = output.GetVector(2);
	auto is_dst = output.GetVector(3);
	auto utc_offset_data = utc_offset.GetDataMutable<interval_t>();
	auto is_dst_data = is_dst.GetDataMutable<bool>();

	idx_t index = 0;
	while (index < capacity && data.offset < names.size()) {
		//	The LONG name is the one we looked up
		const auto &long_id = names[data.offset++];

		//	We don't have the zone tree for determining abbreviated names,
		//	so the SHORT name is the shortest, lexicographically first equivalent TZ without a slash.
		auto short_id = long_id;
		for (const auto &eid : TimeZone::GetEquivalentIds(long_id)) {
			if (eid.find('/') != string::npos) {
				continue;
			}
			if (eid.size() < short_id.size() || (eid.size() == short_id.size() && eid < short_id)) {
				short_id = eid;
			}
		}

		auto tz = TimeZone::TryCreate(long_id);
		int32_t raw_offset_ms;
		int32_t dst_offset_ms;
		tz->GetOffset(data.now, raw_offset_ms, dst_offset_ms);

		name_col.AssignStringUnsafe(index, long_id);
		abbrev.AssignStringUnsafe(index, short_id);
		//	What PG reports is the total offset for today,
		//	which is the total offset (i.e., "raw") plus the DST offset.
		raw_offset_ms += dst_offset_ms;
		utc_offset_data[index] = Interval::FromMicro(int64_t(raw_offset_ms) * Interval::MICROS_PER_MSEC);
		is_dst_data[index] = dst_offset_ms != 0;
		++index;
	}
	for (idx_t col = 0; col < output.GetVectorCount(); col++) {
		output.GetVector(col).SetSize(index);
	}
}

//===--------------------------------------------------------------------===//
// From naive timestamps
//===--------------------------------------------------------------------===//
struct ICUFromNaiveTimestamp : public ICUDateFunc {
	//! Shared by both forms: push the naive parts into the calendar, and recover the sub-millisecond remainder.
	static uint64_t SetTimeParts(Calendar *calendar, timestamp_t naive) {
		date_t local_date;
		dtime_t local_time;
		Timestamp::Convert(naive, local_date, local_time);

		int32_t year, mm, dd;
		Date::Convert(local_date, year, mm, dd);

		int32_t hr, mn, secs, frac;
		Time::Convert(local_time, hr, mn, secs, frac);
		const int32_t millis = frac / int32_t(Interval::MICROS_PER_MSEC);

		calendar->Set(CAL_YEAR, year);
		calendar->Set(CAL_MONTH, int32_t(mm - 1));
		calendar->Set(CAL_DATE, dd);
		calendar->Set(CAL_HOUR_OF_DAY, hr);
		calendar->Set(CAL_MINUTE, mn);
		calendar->Set(CAL_SECOND, secs);
		calendar->Set(CAL_MILLISECOND, millis);
		return uint64_t(frac % Interval::MICROS_PER_MSEC);
	}

	//! Non-throwing form, so a TRY_CAST can turn the failure into NULL rather than an error.
	static bool TryOperation(Calendar *calendar, timestamp_t naive, timestamp_tz_t &result, string &error) {
		if (!IsFinite(naive)) {
			result = timestamp_tz_t {naive.micros};
			return true;
		}
		const auto micros = SetTimeParts(calendar, naive);
		if (!TryGetTime(calendar, micros, result)) {
			error = "ICU date overflows timestamp range";
			return false;
		}
		return true;
	}

	static bool TryOperation(Calendar *calendar, timestamp_ns_t naive, timestamp_tz_ns_t &result, string &error) {
		if (!IsFinite(naive)) {
			result = timestamp_tz_ns_t {naive.nanos};
			return true;
		}

		auto nanos = int32_t(naive.nanos % Interval::NANOS_PER_MICRO);
		timestamp_t micros {naive.nanos / Interval::NANOS_PER_MICRO};
		timestamp_tz_t cast;
		if (!TryOperation(calendar, micros, cast, error)) {
			return false;
		}

		timestamp_ns_t scaled;
		if (!Timestamp::TryFromTimestampNanos(timestamp_t {cast.micros}, nanos, scaled)) {
			error = "ICU date overflows timestamp_ns range";
			return false;
		}
		result = timestamp_tz_ns_t {scaled.nanos};
		return true;
	}

	static inline timestamp_tz_t Operation(Calendar *calendar, timestamp_t naive) {
		if (!IsFinite(naive)) {
			return timestamp_tz_t {naive.micros};
		}
		const auto micros = SetTimeParts(calendar, naive);
		return GetTime(calendar, micros);
	}

	static inline timestamp_tz_ns_t Operation(Calendar *calendar, timestamp_ns_t naive) {
		if (!IsFinite(naive)) {
			return timestamp_tz_ns_t {naive.nanos};
		}

		auto nanos = int32_t(naive.nanos % Interval::NANOS_PER_MICRO);
		timestamp_t micros {naive.nanos / Interval::NANOS_PER_MICRO};
		timestamp_t cast {Operation(calendar, micros).micros};

		timestamp_ns_t result;
		if (!Timestamp::TryFromTimestampNanos(cast, nanos, result)) {
			throw ConversionException("ICU date overflows timestamp_ns range");
		}
		return timestamp_tz_ns_t {result.nanos};
	}

	static inline timestamp_tz_t Operation(Calendar *calendar, date_t naive) {
		if (naive.days == DATE_INFINITY) {
			return timestamp_tz_t {TIMESTAMP_INFINITY};
		}
		if (naive.days == DATE_NINFINITY) {
			return timestamp_tz_t {TIMESTAMP_NINFINITY};
		}
		return Operation(calendar, Timestamp::FromDatetime(naive, dtime_t {}));
	}

	template <class SRC, class DST>
	static void CastFromNaive(cxx::CastFunction::ExecInput &input) {
		auto &info = input.GetBindData<BindData>();
		CalendarPtr calendar(info.calendar->Copy());

		ExecuteCast<SRC, DST>(input, [&](SRC value, DST &result, string &error) {
			using NAIVE = NaiveOf<DST>;
			NAIVE naive;
			if (!TryCastToNaive(value, naive)) {
				error = CastExceptionText(value);
				return false;
			}
			// the zone shift can overflow, and a TRY_CAST must see that as NULL rather than an exception
			return TryOperation(calendar.get(), naive, result, error);
		});
	}

	static void BindCastFromNaive(cxx::CastFunction::BindInput &input) {
		ICUCasts::BindCastData(input, "TIMESTAMP to TIMESTAMPTZ",
		                       "Casting from TIMESTAMP to TIMESTAMP WITH TIME ZONE without an explicit time zone "
		                       "has been disabled  - use \"AT TIME ZONE ...\"");
	}

	static void AddCasts(const ICURegistration &reg) {
		auto add = [&](const string &source, const string &target, cxx::CastFunction::ExecCallback exec) {
			ICUCasts::Register(reg, source, target, true, BindCastFromNaive, exec);
		};
		add("TIMESTAMP", "TIMESTAMPTZ", CastFromNaive<timestamp_t, timestamp_tz_t>);
		add("TIMESTAMP_MS", "TIMESTAMPTZ", CastFromNaive<timestamp_ms_t, timestamp_tz_t>);
		add("TIMESTAMP_NS", "TIMESTAMPTZ", CastFromNaive<timestamp_ns_t, timestamp_tz_t>);
		add("TIMESTAMP_NS", "TIMESTAMPTZ_NS", CastFromNaive<timestamp_ns_t, timestamp_tz_ns_t>);
		add("TIMESTAMP_S", "TIMESTAMPTZ", CastFromNaive<timestamp_s_t, timestamp_tz_t>);
		add("DATE", "TIMESTAMPTZ", CastFromNaive<date_t, timestamp_tz_t>);
	}
};

//===--------------------------------------------------------------------===//
// To naive timestamps
//===--------------------------------------------------------------------===//
struct ICUToNaiveTimestamp : public ICUDateFunc {
	//! Non-throwing form, so a TRY_CAST can turn the failure into NULL rather than an error. The error text is
	//! returned for the throwing form, keeping both spellings identical.
	static bool TryOperation(Calendar *calendar, timestamp_tz_t instant, timestamp_t &naive, string &error) {
		if (!IsFinite(instant)) {
			naive = timestamp_t {instant.micros};
			return true;
		}

		// Extract the time zone parts
		auto micros = int32_t(SetTime(calendar, instant));
		const auto era = ExtractField(calendar, CAL_ERA);
		const auto year = ExtractField(calendar, CAL_YEAR);
		const auto mm = ExtractField(calendar, CAL_MONTH) + 1;
		const auto dd = ExtractField(calendar, CAL_DATE);

		const auto yyyy = era ? year : (-year + 1);
		date_t local_date;
		if (!Date::TryFromDate(yyyy, mm, dd, local_date)) {
			error = "Unable to convert TIMESTAMPTZ to local date";
			return false;
		}

		const auto hr = ExtractField(calendar, CAL_HOUR_OF_DAY);
		const auto mn = ExtractField(calendar, CAL_MINUTE);
		const auto secs = ExtractField(calendar, CAL_SECOND);
		const auto millis = ExtractField(calendar, CAL_MILLISECOND);

		micros += millis * int32_t(Interval::MICROS_PER_MSEC);
		dtime_t local_time = Time::FromTime(hr, mn, secs, micros);

		if (!Timestamp::TryFromDatetime(local_date, local_time, naive)) {
			error = "Unable to convert TIMESTAMPTZ to local TIMESTAMP";
			return false;
		}

		return true;
	}

	static inline timestamp_t Operation(Calendar *calendar, timestamp_tz_t instant) {
		timestamp_t naive;
		string error;
		if (!TryOperation(calendar, instant, naive, error)) {
			throw ConversionException(error);
		}
		return naive;
	}

	static bool TryOperation(Calendar *calendar, timestamp_tz_ns_t instant, timestamp_ns_t &result, string &error) {
		if (!IsFinite(instant)) {
			result = timestamp_ns_t {instant.nanos};
			return true;
		}

		auto nanos = int32_t(instant.nanos % Interval::NANOS_PER_MICRO);
		timestamp_tz_t micros {instant.nanos / Interval::NANOS_PER_MICRO};
		timestamp_t cast;
		if (!TryOperation(calendar, micros, cast, error)) {
			return false;
		}

		if (!Timestamp::TryFromTimestampNanos(cast, nanos, result)) {
			error = "ICU date overflows timestamp_ns range";
			return false;
		}
		return true;
	}

	static inline timestamp_ns_t Operation(Calendar *calendar, timestamp_tz_ns_t instant) {
		timestamp_ns_t result;
		string error;
		if (!TryOperation(calendar, instant, result, error)) {
			throw ConversionException(error);
		}
		return result;
	}

	//! Rounds the local time micros * 1000 + nanos to microseconds, away from zero on ties like the other casts.
	//! The nanosecond value is not formed, as it can overflow int64 where the microsecond value does not.
	static timestamp_t RoundToMicros(timestamp_t micros, int32_t nanos) {
		auto value = micros.micros;
		if (value == 0) {
			value = nanos >= 500 ? 1 : (nanos <= -500 ? -1 : 0);
		} else if (value > 0) {
			value += nanos >= 0 ? (nanos >= 500 ? 1 : 0) : (nanos < -500 ? -1 : 0);
		} else {
			value += nanos <= 0 ? (nanos <= -500 ? -1 : 0) : (nanos > 500 ? 1 : 0);
		}
		return timestamp_t {value};
	}

	template <class SRC, class DST>
	static void CastToNaive(cxx::CastFunction::ExecInput &input) {
		auto &info = input.GetBindData<BindData>();
		CalendarPtr calendar(info.calendar->Copy());

		ExecuteCast<SRC, DST>(input, [&](SRC value, DST &result, string &error) {
			if constexpr (std::is_same<SRC, timestamp_tz_ns_t>::value && std::is_same<DST, timestamp_t>::value) {
				if (IsFinite(value)) {
					// shift in microseconds: the local time can lie outside the timestamp_ns range
					timestamp_t micros_naive;
					const auto nanos = int32_t(value.nanos % Interval::NANOS_PER_MICRO);
					const timestamp_tz_t micros {value.nanos / Interval::NANOS_PER_MICRO};
					if (!TryOperation(calendar.get(), micros, micros_naive, error)) {
						return false;
					}
					result = RoundToMicros(micros_naive, nanos);
					return true;
				}
			}
			// the zone shift itself can fail, and a TRY_CAST must see that as NULL rather than an exception
			NaiveOf<typename std::conditional<std::is_same<SRC, timestamp_tz_ns_t>::value, timestamp_tz_ns_t,
			                                  timestamp_tz_t>::type>
			    naive;
			if (!TryOperation(calendar.get(), value, naive, error)) {
				return false;
			}
			if (!TryCastTimestamp(naive, result)) {
				error = "Could not convert Timestamp to higher precision.";
				return false;
			}
			return true;
		});
	}

	static void BindCastToNaive(cxx::CastFunction::BindInput &input) {
		auto target = input.GetTargetType().ToText();
		ICUCasts::BindCastData(input, ("TIMESTAMPTZ to " + target).c_str(),
		                       "Casting from TIMESTAMP WITH TIME ZONE to " + target +
		                           " without an explicit time zone has been disabled  - use \"AT TIME ZONE ...\"");
	}

	static void AddCasts(const ICURegistration &reg) {
		auto add = [&](const string &source, const string &target, cxx::CastFunction::ExecCallback exec) {
			ICUCasts::Register(reg, source, target, true, BindCastToNaive, exec);
		};
		add("TIMESTAMPTZ", "TIMESTAMP", CastToNaive<timestamp_tz_t, timestamp_t>);
		add("TIMESTAMPTZ", "TIMESTAMP_MS", CastToNaive<timestamp_tz_t, timestamp_ms_t>);
		add("TIMESTAMPTZ", "TIMESTAMP_NS", CastToNaive<timestamp_tz_t, timestamp_ns_t>);
		add("TIMESTAMPTZ", "TIMESTAMP_S", CastToNaive<timestamp_tz_t, timestamp_s_t>);

		add("TIMESTAMPTZ_NS", "TIMESTAMP", CastToNaive<timestamp_tz_ns_t, timestamp_t>);
		add("TIMESTAMPTZ_NS", "TIMESTAMP_NS", CastToNaive<timestamp_tz_ns_t, timestamp_ns_t>);
	}
};

//===--------------------------------------------------------------------===//
// current_localtimestamp, current_localtime
//===--------------------------------------------------------------------===//
struct ICULocalTimestampFunc : public ICUDateFunc {
	struct BindDataNow : public BindData {
		explicit BindDataNow(cxx::Context &context) : BindData(context), now(context.GetTransactionStartTime()) {
		}

		bool operator==(const BindDataNow &other) const {
			return now.micros == other.now.micros && BindData::operator==(other);
		}

		timestamp_tz_t now;
	};

	static void BindNow(cxx::ScalarFunction::BindInput &input) {
		input.SetBindData<BindDataNow>(input.GetContext());
	}

	static timestamp_t GetLocalTimestamp(cxx::ScalarFunction::ExecInput &input) {
		auto &info = input.GetBindData<BindDataNow>();
		CalendarPtr calendar_ptr(info.calendar->Copy());
		return ICUToNaiveTimestamp::Operation(calendar_ptr.get(), info.now);
	}

	static void Execute(cxx::ScalarFunction::ExecInput &input) {
		const auto local = GetLocalTimestamp(input);
		ExecuteRows<timestamp_t>(input, [&]() -> std::optional<timestamp_t> { return local; });
	}

	static void ExecuteTime(cxx::ScalarFunction::ExecInput &input) {
		const auto local = Timestamp::GetTime(GetLocalTimestamp(input));
		ExecuteRows<dtime_t>(input, [&]() -> std::optional<dtime_t> { return local; });
	}

	static void AddFunction(const ICURegistration &reg, const string &name, const string &type,
	                        cxx::ScalarFunction::ExecCallback exec) {
		auto function = reg.Scalar(name, {}, type);
		function.SetBindCallback(BindNow).SetExecCallback(exec);
		// the bind reads the start of the transaction
		function.SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
		reg.extension.Register(function);
	}
};

//===--------------------------------------------------------------------===//
// TIME WITH TIME ZONE
//===--------------------------------------------------------------------===//
dtime_tz_t ICUToTimeTZ::Operation(Calendar *calendar, dtime_tz_t timetz) {
	// Normalise to +00:00, add TZ offset, then set offset to TZ
	auto time = Time::NormalizeTimeTZ(timetz);

	auto offset = GetTimeTZOffset(calendar);

	date_t date;
	time = Interval::Add(time, Interval::Make(0, 0, offset * Interval::MICROS_PER_SEC), date);
	return dtime_tz_t(time.micros, offset);
}

bool ICUToTimeTZ::ToTimeTZ(Calendar *calendar, timestamp_tz_t instant, dtime_tz_t &result, string *error_message) {
	if (!IsFinite(instant)) {
		if (error_message) {
			*error_message = "Can't get TIME of infinite TIMESTAMP";
		}
		return false;
	}

	//	Time in current TZ
	auto micros = int32_t(SetTime(calendar, instant));
	const auto hour = ExtractField(calendar, CAL_HOUR_OF_DAY);
	const auto minute = ExtractField(calendar, CAL_MINUTE);
	const auto second = ExtractField(calendar, CAL_SECOND);
	const auto millis = ExtractField(calendar, CAL_MILLISECOND);
	micros += millis * int32_t(Interval::MICROS_PER_MSEC);
	if (!Time::IsValidTime(hour, minute, second, micros)) {
		return false;
	}
	const auto time = Time::FromTime(hour, minute, second, micros);

	//	Offset in current TZ
	int32_t offset;
	if (!error_message) {
		offset = GetTimeTZOffset(calendar);
	} else if (!TryGetTimeTZOffset(calendar, offset)) {
		*error_message = TimeTZOffsetError(offset);
		return false;
	}

	result = dtime_tz_t(time.micros, offset);
	return true;
}

static void CastToTimeTZ(cxx::CastFunction::ExecInput &input) {
	auto &info = input.GetBindData<ICUDateFunc::BindData>();
	CalendarPtr calendar(info.calendar->Copy());
	ExecuteCast<timestamp_tz_t, dtime_tz_t>(input, [&](timestamp_tz_t value, dtime_tz_t &result, string &error) {
		return ICUToTimeTZ::ToTimeTZ(calendar.get(), value, result, &error);
	});
}

static void CastFromTime(cxx::CastFunction::ExecInput &input) {
	auto &info = input.GetBindData<ICUDateFunc::BindData>();
	CalendarPtr calendar(info.calendar->Copy());

	// Read the session UTC offset (with DST) from the calendar.
	// This mirrors the no-offset branch of the VARCHAR to TIMETZ cast so that
	// '00:00:00'::TIME::TIMETZ matches '00:00:00'::TIMETZ.
	int32_t offset;
	string offset_error;
	if (!ICUDateFunc::TryGetTimeTZOffset(calendar.get(), offset)) {
		offset_error = ICUDateFunc::TimeTZOffsetError(offset);
	}
	ExecuteCast<dtime_t, dtime_tz_t>(input, [&](dtime_t value, dtime_tz_t &result, string &error) {
		if (!offset_error.empty()) {
			error = offset_error;
			return false;
		}
		result = dtime_tz_t(value.micros, offset);
		return true;
	});
}

static void BindCastToTimeTZ(cxx::CastFunction::BindInput &input) {
	ICUCasts::BindCastData(input, "TIMESTAMPTZ to TIMETZ");
}

static void BindCastFromTime(cxx::CastFunction::BindInput &input) {
	ICUCasts::BindCastData(input, "TIME to TIMETZ");
}

void ICUToTimeTZ::AddCasts(const ICURegistration &reg) {
	ICUCasts::Register(reg, "TIMESTAMPTZ", "TIMETZ", true, BindCastToTimeTZ, CastToTimeTZ);
	ICUCasts::Register(reg, "TIME", "TIMETZ", true, BindCastFromTime, CastFromTime);
}

//===--------------------------------------------------------------------===//
// timezone(zone, ts)
//===--------------------------------------------------------------------===//
struct ICUTimeZoneFunc : public ICUDateFunc {
	template <typename OP, typename SRC, typename DST>
	static void Execute(cxx::ScalarFunction::ExecInput &input) {
		auto &cache = input.GetInitData<CalendarCacheState>();

		// Two cases: constant TZ, variable TZ
		auto tz_vec = input.GetArg(0);
		Calendar *constant_calendar = nullptr;
		if (IsConstant(tz_vec)) {
			StringArgReader tz(tz_vec);
			if (tz.IsValid(0)) {
				constant_calendar = cache.GetCalendar(tz.Get(0));
			}
		}
		ExecuteRows<DST, std::string_view, SRC>(input, [&](std::string_view tz_id, SRC ts) -> std::optional<DST> {
			if (constant_calendar) {
				return OP::Operation(constant_calendar, ts);
			}
			if (IsFinite(ts)) {
				return OP::Operation(cache.GetCalendar(tz_id), ts);
			}
			return Make<DST>(Raw(ts));
		});
	}

	static void ExecuteTimeTZ(cxx::ScalarFunction::ExecInput &input) {
		auto &cache = input.GetInitData<CalendarCacheState>();
		ExecuteRows<uint64_t, std::string_view, uint64_t>(
		    input, [&](std::string_view tz_id, uint64_t bits) -> std::optional<uint64_t> {
			    return ICUToTimeTZ::Operation(cache.GetCalendar(tz_id), dtime_tz_t(bits)).GetBits();
		    });
	}

	static void AddFunction(const ICURegistration &reg, const string &name) {
		auto add = [&](const string &param, const string &type, const string &result_type,
		               cxx::ScalarFunction::ExecCallback exec) {
			auto function = reg.Scalar(name, {{"timezone", "VARCHAR"}, {param, type}}, result_type);
			function.SetBindCallback(Bind).SetInitCallback(InitCalendarCache).SetExecCallback(exec);
			function.SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
			reg.extension.Register(function);
		};
		add("timestamp", "TIMESTAMP", "TIMESTAMPTZ", Execute<ICUFromNaiveTimestamp, timestamp_t, timestamp_tz_t>);
		add("timestamp", "TIMESTAMPTZ", "TIMESTAMP", Execute<ICUToNaiveTimestamp, timestamp_tz_t, timestamp_t>);
		add("timestamp", "TIMETZ", "TIMETZ", ExecuteTimeTZ);
		add("timestamp", "TIMESTAMP_NS", "TIMESTAMPTZ_NS",
		    Execute<ICUFromNaiveTimestamp, timestamp_ns_t, timestamp_tz_ns_t>);
		add("timestamp", "TIMESTAMPTZ_NS", "TIMESTAMP_NS",
		    Execute<ICUToNaiveTimestamp, timestamp_tz_ns_t, timestamp_ns_t>);
		add("date", "DATE", "TIMESTAMPTZ", ExecuteDate);
	}

	static void ExecuteDate(cxx::ScalarFunction::ExecInput &input) {
		auto &cache = input.GetInitData<CalendarCacheState>();
		ExecuteRows<timestamp_tz_t, std::string_view, date_t>(
		    input, [&](std::string_view tz_id, date_t date) -> std::optional<timestamp_tz_t> {
			    if (IsFinite(date)) {
				    return ICUFromNaiveTimestamp::Operation(cache.GetCalendar(tz_id), date);
			    }
			    return ICUFromNaiveTimestamp::Operation(nullptr, date);
		    });
	}
};

timestamp_tz_t ICUDateFunc::FromNaive(Calendar *calendar, timestamp_t naive) {
	return ICUFromNaiveTimestamp::Operation(calendar, naive);
}

bool ICUDateFunc::TryFromNaive(Calendar *calendar, timestamp_t naive, timestamp_tz_t &result, string &error) {
	return ICUFromNaiveTimestamp::TryOperation(calendar, naive, result, error);
}

void RegisterICUTimeZoneFunctions(const ICURegistration &reg) {
	//	Table functions
	auto tz_names = cxx::TableFunction::Create(reg.factory);
	tz_names.SetStability(cxx::FunctionStability::CONSISTENT);
	tz_names.SetName("pg_timezone_names")
	    .SetBindCallback(ICUTimeZoneBind)
	    .SetInitGlobalCallback(ICUTimeZoneInit)
	    .SetExecCallback(ICUTimeZoneFunction);
	reg.extension.Register(tz_names);

	//	Scalar functions
	ICUTimeZoneFunc::AddFunction(reg, "timezone");
	ICULocalTimestampFunc::AddFunction(reg, "current_localtimestamp", "TIMESTAMP", ICULocalTimestampFunc::Execute);
	ICULocalTimestampFunc::AddFunction(reg, "current_localtime", "TIME", ICULocalTimestampFunc::ExecuteTime);

	// 	Casts
	ICUFromNaiveTimestamp::AddCasts(reg);
	ICUToNaiveTimestamp::AddCasts(reg);
	ICUToTimeTZ::AddCasts(reg);
}

} // namespace duckdb_icu
