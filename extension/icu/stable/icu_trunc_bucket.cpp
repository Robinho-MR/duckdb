#include "icu_stable_areas.hpp"
#include "icu_stable_common.hpp"

#include "icu_bridge_trunc_bucket.hpp"

#include <string>

namespace icu_stable {

namespace {

//! The error a kernel raised, as the exception the engine reports
cxx::Exception ToException(const icu_bridge::KernelError &error) {
	int code;
	switch (error.kind) {
	case icu_bridge::ErrorKind::CONVERSION:
		code = DUCKDB_V2_ERROR_TYPE_CONVERSION;
		break;
	case icu_bridge::ErrorKind::OUT_OF_RANGE:
		code = DUCKDB_V2_ERROR_INPUT_OUT_OF_RANGE;
		break;
	case icu_bridge::ErrorKind::NOT_IMPLEMENTED:
		code = DUCKDB_V2_ERROR_QUERY_NOT_IMPLEMENTED;
		break;
	case icu_bridge::ErrorKind::INVALID_INPUT:
		code = DUCKDB_V2_ERROR_INPUT_INVALID;
		break;
	default:
		code = DUCKDB_V2_ERROR_QUERY_EXECUTOR;
		break;
	}
	return cxx::Exception(code, error.what());
}

//! Calls the kernel, reporting its errors as engine errors
template <class F>
auto Run(F &&function) -> decltype(function()) {
	try {
		return function();
	} catch (icu_bridge::KernelError &error) {
		throw ToException(error);
	}
}

int64_t FloorDivide(int64_t value, int64_t divisor) {
	auto quotient = value / divisor;
	if (value % divisor < 0) {
		quotient--;
	}
	return quotient;
}

//! The instant in microseconds, nanosecond instants are floored. The infinities are kept.
int64_t ToMicros(cxx::timestamp_tz_t value) {
	return value.micros;
}

int64_t ToMicros(cxx::timestamp_tz_ns_t value) {
	if (!IsFinite(value.nanos)) {
		return value.nanos;
	}
	return FloorDivide(value.nanos, 1000);
}

std::string ToString(cxx::varchar_t value) {
	return std::string(value.view());
}

icu_bridge::IntervalValue ToValue(const cxx::interval_t &interval, bool constant) {
	icu_bridge::IntervalValue result;
	result.constant = constant;
	result.months = interval.months;
	result.days = interval.days;
	result.micros = interval.micros;
	return result;
}

//! A calendar of the zone the buckets are computed in, kept by the thread
icu_bridge::Calendar &RequireZoneCalendar(const Settings &settings, const std::string &zone) {
	std::string error;
	auto *calendar = ZoneCalendar(settings, zone, error);
	if (!calendar) {
		throw cxx::Exception(DUCKDB_V2_ERROR_QUERY_NOT_IMPLEMENTED, error);
	}
	return *calendar;
}

//----------------------------------------------------------------------------------------------------------------------
// date_trunc
//----------------------------------------------------------------------------------------------------------------------
template <class TS>
void DateTruncExec(cxx::ScalarFunction::ExecInput &input) {
	auto &calendar = SettingsCalendar(ReadSettings(input.GetContext()));
	const auto part = input.GetArg(0).GetView();
	const auto in = input.GetArg(1).GetView();
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	auto *out = result.GetDataMutable<cxx::timestamp_tz_t>();
	std::string last_part;
	int last_id = -1;
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!part.IsValid(i) || !in.IsValid(i)) {
			result.SetNull(i);
			continue;
		}
		const auto instant = ToMicros(in.Data<TS>()[in.SelAt(i)]);
		if (!IsFinite(instant)) {
			out[i].micros = instant;
			continue;
		}
		Run([&]() {
			const auto specifier = part.Data<cxx::varchar_t>()[part.SelAt(i)].view();
			if (last_id < 0 || specifier != last_part) {
				last_id = icu_bridge::ParseTruncPart(std::string(specifier));
				last_part = std::string(specifier);
			}
			out[i].micros = icu_bridge::DateTrunc(calendar, last_id, instant);
		});
	}
}

template <class TS>
void RegisterDateTrunc(cxx::Extension &extension, cxx::Factory &factory, const char *name,
                       const cxx::LogicalType &type) {
	auto function = cxx::ScalarFunction::Create(factory);
	function.SetName(name)
	    .SetExecCallback(DateTruncExec<TS>)
	    .SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
	function.GetSignature()
	    .AddParameter("part", factory.ParseType("VARCHAR"))
	    .AddParameter("timestamp", type)
	    .SetReturnType(factory.ParseType("TIMESTAMP WITH TIME ZONE"));
	extension.Register(function);
}

//----------------------------------------------------------------------------------------------------------------------
// make_timestamptz
//----------------------------------------------------------------------------------------------------------------------
template <bool HAS_ZONE>
void MakeTimestampTZExec(cxx::ScalarFunction::ExecInput &input) {
	const auto settings = ReadSettings(input.GetContext());
	icu_bridge::Calendar *calendar = nullptr;
	if (!HAS_ZONE) {
		calendar = &SettingsCalendar(settings);
	}
	const auto year = input.GetArg(0).GetView();
	const auto month = input.GetArg(1).GetView();
	const auto day = input.GetArg(2).GetView();
	const auto hour = input.GetArg(3).GetView();
	const auto minute = input.GetArg(4).GetView();
	const auto seconds = input.GetArg(5).GetView();
	cxx::VectorView zone = input.GetArg(HAS_ZONE ? 6 : 5).GetView();
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	auto *out = result.GetDataMutable<cxx::timestamp_tz_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!year.IsValid(i) || !month.IsValid(i) || !day.IsValid(i) || !hour.IsValid(i) || !minute.IsValid(i) ||
		    !seconds.IsValid(i) || (HAS_ZONE && !zone.IsValid(i))) {
			result.SetNull(i);
			continue;
		}
		if (HAS_ZONE) {
			calendar = &RequireZoneCalendar(settings, ToString(zone.Data<cxx::varchar_t>()[zone.SelAt(i)]));
		}
		out[i].micros = Run([&]() {
			return icu_bridge::MakeTimestampTZ(
			    *calendar, year.Data<int64_t>()[year.SelAt(i)], month.Data<int64_t>()[month.SelAt(i)],
			    day.Data<int64_t>()[day.SelAt(i)], hour.Data<int64_t>()[hour.SelAt(i)],
			    minute.Data<int64_t>()[minute.SelAt(i)], seconds.Data<double>()[seconds.SelAt(i)]);
		});
	}
}

void MakeTimestampTZFromMicros(cxx::ScalarFunction::ExecInput &input) {
	const auto in = input.GetArg(0).GetView();
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	auto *out = result.GetDataMutable<cxx::timestamp_tz_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!in.IsValid(i)) {
			result.SetNull(i);
			continue;
		}
		const auto micros = in.Data<int64_t>()[in.SelAt(i)];
		if (!IsFinite(micros)) {
			ThrowConversion("Timestamp microseconds out of range: " + std::to_string(micros));
		}
		out[i].micros = micros;
	}
}

void RegisterMakeTimestampTZ(cxx::Extension &extension, cxx::Factory &factory) {
	const auto bigint = factory.ParseType("BIGINT");
	const auto timestamp_tz = factory.ParseType("TIMESTAMP WITH TIME ZONE");

	auto senary = cxx::ScalarFunction::Create(factory);
	senary.SetName("make_timestamptz")
	    .SetExecCallback(MakeTimestampTZExec<false>)
	    .SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
	senary.GetSignature()
	    .AddParameter("year", bigint)
	    .AddParameter("month", bigint)
	    .AddParameter("day", bigint)
	    .AddParameter("hour", bigint)
	    .AddParameter("minute", bigint)
	    .AddParameter("seconds", factory.ParseType("DOUBLE"))
	    .SetReturnType(timestamp_tz);
	extension.Register(senary);

	auto septenary = cxx::ScalarFunction::Create(factory);
	septenary.SetName("make_timestamptz")
	    .SetExecCallback(MakeTimestampTZExec<true>)
	    .SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
	septenary.GetSignature()
	    .AddParameter("year", bigint)
	    .AddParameter("month", bigint)
	    .AddParameter("day", bigint)
	    .AddParameter("hour", bigint)
	    .AddParameter("minute", bigint)
	    .AddParameter("seconds", factory.ParseType("DOUBLE"))
	    .AddParameter("timezone", factory.ParseType("VARCHAR"))
	    .SetReturnType(timestamp_tz);
	extension.Register(septenary);

	auto from_micros = cxx::ScalarFunction::Create(factory);
	from_micros.SetName("make_timestamptz").SetExecCallback(MakeTimestampTZFromMicros);
	from_micros.GetSignature().AddParameter("micros", bigint).SetReturnType(timestamp_tz);
	extension.Register(from_micros);
}

//----------------------------------------------------------------------------------------------------------------------
// time_bucket
//----------------------------------------------------------------------------------------------------------------------
//! The buckets without a time zone argument are computed in UTC
icu_bridge::Calendar &UTCCalendar(cxx::ScalarFunction::ExecInput &input) {
	return RequireZoneCalendar(ReadSettings(input.GetContext()), "UTC");
}

template <class TS>
void TimeBucketExec(cxx::ScalarFunction::ExecInput &input) {
	auto &calendar = UTCCalendar(input);
	const auto width = input.GetArg(0).GetView();
	const bool width_constant = input.GetArg(0).GetVectorType() == cxx::VectorType::CONSTANT;
	const auto in = input.GetArg(1).GetView();
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	auto *out = result.GetDataMutable<cxx::timestamp_tz_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!width.IsValid(i) || !in.IsValid(i)) {
			result.SetNull(i);
			continue;
		}
		const auto bucket_width = ToValue(width.Data<cxx::interval_t>()[width.SelAt(i)], width_constant);
		const auto instant = ToMicros(in.Data<TS>()[in.SelAt(i)]);
		out[i].micros = Run([&]() { return icu_bridge::TimeBucket(calendar, bucket_width, instant); });
	}
}

template <class TS>
void TimeBucketOffsetExec(cxx::ScalarFunction::ExecInput &input) {
	auto &calendar = UTCCalendar(input);
	const auto width = input.GetArg(0).GetView();
	const bool width_constant = input.GetArg(0).GetVectorType() == cxx::VectorType::CONSTANT;
	const auto in = input.GetArg(1).GetView();
	const auto offset = input.GetArg(2).GetView();
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	auto *out = result.GetDataMutable<cxx::timestamp_tz_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!width.IsValid(i) || !in.IsValid(i) || !offset.IsValid(i)) {
			result.SetNull(i);
			continue;
		}
		const auto bucket_width = ToValue(width.Data<cxx::interval_t>()[width.SelAt(i)], width_constant);
		const auto bucket_offset = ToValue(offset.Data<cxx::interval_t>()[offset.SelAt(i)], false);
		const auto instant = ToMicros(in.Data<TS>()[in.SelAt(i)]);
		out[i].micros =
		    Run([&]() { return icu_bridge::TimeBucketOffset(calendar, bucket_width, instant, bucket_offset); });
	}
}

template <class TS>
void TimeBucketOriginExec(cxx::ScalarFunction::ExecInput &input) {
	auto &calendar = UTCCalendar(input);
	const auto width = input.GetArg(0).GetView();
	const bool width_constant = input.GetArg(0).GetVectorType() == cxx::VectorType::CONSTANT &&
	                            input.GetArg(2).GetVectorType() == cxx::VectorType::CONSTANT;
	const auto in = input.GetArg(1).GetView();
	const auto origin = input.GetArg(2).GetView();
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	auto *out = result.GetDataMutable<cxx::timestamp_tz_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!width.IsValid(i) || !in.IsValid(i) || !origin.IsValid(i)) {
			result.SetNull(i);
			continue;
		}
		const auto bucket_origin = origin.Data<cxx::timestamp_tz_t>()[origin.SelAt(i)].micros;
		if (!IsFinite(bucket_origin)) {
			result.SetNull(i);
			continue;
		}
		const auto bucket_width = ToValue(width.Data<cxx::interval_t>()[width.SelAt(i)], width_constant);
		const auto instant = ToMicros(in.Data<TS>()[in.SelAt(i)]);
		out[i].micros =
		    Run([&]() { return icu_bridge::TimeBucketOrigin(calendar, bucket_width, instant, bucket_origin); });
	}
}

template <class TS>
void TimeBucketZoneExec(cxx::ScalarFunction::ExecInput &input) {
	const auto settings = ReadSettings(input.GetContext());
	const auto width = input.GetArg(0).GetView();
	const bool width_constant = input.GetArg(0).GetVectorType() == cxx::VectorType::CONSTANT &&
	                            input.GetArg(2).GetVectorType() == cxx::VectorType::CONSTANT;
	const auto in = input.GetArg(1).GetView();
	const auto zone = input.GetArg(2).GetView();
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	auto *out = result.GetDataMutable<cxx::timestamp_tz_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!width.IsValid(i) || !in.IsValid(i) || !zone.IsValid(i)) {
			result.SetNull(i);
			continue;
		}
		auto &calendar = RequireZoneCalendar(settings, ToString(zone.Data<cxx::varchar_t>()[zone.SelAt(i)]));
		const auto bucket_width = ToValue(width.Data<cxx::interval_t>()[width.SelAt(i)], width_constant);
		const auto instant = ToMicros(in.Data<TS>()[in.SelAt(i)]);
		out[i].micros = Run([&]() { return icu_bridge::TimeBucketZone(calendar, bucket_width, instant); });
	}
}

template <class TS>
void RegisterTimeBucket(cxx::Extension &extension, cxx::Factory &factory, const cxx::LogicalType &type) {
	const auto interval = factory.ParseType("INTERVAL");
	const auto timestamp_tz = factory.ParseType("TIMESTAMP WITH TIME ZONE");

	auto add = [&](cxx::ScalarFunction::ExecCallback exec, const char *third_name, const cxx::LogicalType &third) {
		auto function = cxx::ScalarFunction::Create(factory);
		function.SetName("time_bucket")
		    .SetExecCallback(exec)
		    .SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
		auto signature = function.GetSignature();
		signature.AddParameter("bucket_width", interval).AddParameter("timestamp", type);
		if (third_name) {
			signature.AddParameter(third_name, third);
		}
		signature.SetReturnType(timestamp_tz);
		extension.Register(function);
	};
	add(TimeBucketExec<TS>, nullptr, interval);
	add(TimeBucketOffsetExec<TS>, "offset", interval);
	add(TimeBucketOriginExec<TS>, "origin", timestamp_tz);
	add(TimeBucketZoneExec<TS>, "timezone", factory.ParseType("VARCHAR"));
}

//----------------------------------------------------------------------------------------------------------------------
// Casts
//----------------------------------------------------------------------------------------------------------------------
void RequireTimestampTZCastsEnabled(cxx::CastFunction::ExecInput &input) {
	auto disabled = input.GetContext().GetConfig().TryGetOption("disable_timestamptz_casts");
	if (disabled && !disabled->value.IsNull() && disabled->value.Get<bool>()) {
		throw cxx::Exception(DUCKDB_V2_ERROR_QUERY_BINDER,
		                     "Casting from TIMESTAMP WITH TIME ZONE to DATE without an explicit time zone has been "
		                     "disabled  - use \"AT TIME ZONE ...\"");
	}
}

template <class TS>
void CastToDate(cxx::CastFunction::ExecInput &input) {
	RequireTimestampTZCastsEnabled(input);
	auto &calendar = SettingsCalendar(ReadSettings(input.GetContext()));
	const auto in = input.GetInput().GetView();
	auto out = input.GetOutput();
	out.SetSize(input.GetRowCount());
	auto *data = out.GetDataMutable<cxx::date_t>();
	const auto try_mode = input.GetMode() == cxx::CastMode::TRY;
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!in.IsValid(i)) {
			out.SetNull(i);
			continue;
		}
		const auto instant = ToMicros(in.Data<TS>()[in.SelAt(i)]);
		if (!IsFinite(instant)) {
			data[i].days = instant > 0 ? std::numeric_limits<int32_t>::max() : -std::numeric_limits<int32_t>::max();
			continue;
		}
		std::string error;
		if (!icu_bridge::InstantToDate(calendar, instant, data[i].days, error)) {
			if (!try_mode) {
				ThrowConversion(error);
			}
			out.SetNull(i);
		}
	}
}

void CastToTimeTZ(cxx::CastFunction::ExecInput &input) {
	auto &calendar = SettingsCalendar(ReadSettings(input.GetContext()));
	const auto in = input.GetInput().GetView();
	auto out = input.GetOutput();
	out.SetSize(input.GetRowCount());
	auto *data = out.GetDataMutable<cxx::dtime_tz_t>();
	const auto try_mode = input.GetMode() == cxx::CastMode::TRY;
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!in.IsValid(i)) {
			out.SetNull(i);
			continue;
		}
		const auto instant = in.Data<cxx::timestamp_tz_t>()[in.SelAt(i)].micros;
		std::string error;
		int64_t time_micros = 0;
		int32_t offset = 0;
		if (!IsFinite(instant)) {
			error = "Can't get TIME of infinite TIMESTAMP";
		} else if (icu_bridge::InstantToTimeTZ(calendar, instant, time_micros, offset, error)) {
			data[i] = cxx::dtime_tz_t(time_micros, offset);
			continue;
		}
		if (!error.empty() && !try_mode) {
			ThrowConversion(error);
		}
		out.SetNull(i);
	}
}

void CastFromTime(cxx::CastFunction::ExecInput &input) {
	auto &calendar = SettingsCalendar(ReadSettings(input.GetContext()));
	const auto in = input.GetInput().GetView();
	auto out = input.GetOutput();
	out.SetSize(input.GetRowCount());
	auto *data = out.GetDataMutable<cxx::dtime_tz_t>();
	const auto try_mode = input.GetMode() == cxx::CastMode::TRY;
	// The session offset (with DST), which matches the offset '00:00:00'::TIMETZ gets
	bool have_offset = false;
	bool offset_valid = false;
	int32_t offset = 0;
	std::string error;
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!in.IsValid(i)) {
			out.SetNull(i);
			continue;
		}
		if (!have_offset) {
			offset_valid = icu_bridge::CurrentTimeTZOffset(calendar, offset, error);
			have_offset = true;
		}
		if (!offset_valid) {
			if (!try_mode) {
				ThrowConversion(error);
			}
			out.SetNull(i);
			continue;
		}
		data[i] = cxx::dtime_tz_t(in.Data<cxx::dtime_t>()[in.SelAt(i)].micros, offset);
	}
}

void RegisterCasts(cxx::Extension &extension, cxx::Factory &factory) {
	const auto timestamp_tz = factory.ParseType("TIMESTAMP WITH TIME ZONE");
	const auto timestamp_tz_ns = factory.ParseType("TIMESTAMPTZ_NS");
	const auto date = factory.ParseType("DATE");
	const auto time_tz = factory.ParseType("TIME WITH TIME ZONE");

	auto to_date = cxx::CastFunction::Create(factory);
	to_date.SetSourceType(timestamp_tz).SetTargetType(date).SetExecCallback(CastToDate<cxx::timestamp_tz_t>);
	extension.Register(to_date);

	auto ns_to_date = cxx::CastFunction::Create(factory);
	ns_to_date.SetSourceType(timestamp_tz_ns).SetTargetType(date).SetExecCallback(CastToDate<cxx::timestamp_tz_ns_t>);
	extension.Register(ns_to_date);

	auto to_time_tz = cxx::CastFunction::Create(factory);
	to_time_tz.SetSourceType(timestamp_tz).SetTargetType(time_tz).SetExecCallback(CastToTimeTZ);
	extension.Register(to_time_tz);

	auto from_time = cxx::CastFunction::Create(factory);
	from_time.SetSourceType(factory.ParseType("TIME")).SetTargetType(time_tz).SetExecCallback(CastFromTime);
	extension.Register(from_time);
}

} // namespace

void RegisterTruncBucketFunctions(cxx::Extension &extension, cxx::Factory &factory) {
	const auto timestamp_tz = factory.ParseType("TIMESTAMP WITH TIME ZONE");
	const auto timestamp_tz_ns = factory.ParseType("TIMESTAMPTZ_NS");
	for (const char *name : {"date_trunc", "datetrunc"}) {
		RegisterDateTrunc<cxx::timestamp_tz_t>(extension, factory, name, timestamp_tz);
		RegisterDateTrunc<cxx::timestamp_tz_ns_t>(extension, factory, name, timestamp_tz_ns);
	}
	RegisterMakeTimestampTZ(extension, factory);
	RegisterTimeBucket<cxx::timestamp_tz_t>(extension, factory, timestamp_tz);
	RegisterTimeBucket<cxx::timestamp_tz_ns_t>(extension, factory, timestamp_tz_ns);
	RegisterCasts(extension, factory);
}

} // namespace icu_stable
