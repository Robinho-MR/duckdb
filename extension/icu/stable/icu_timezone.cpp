#include "icu_stable_areas.hpp"
#include "icu_stable_common.hpp"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

namespace icu_stable {

int32_t CheckedTimeTZOffset(int64_t offset) {
	if (offset >= -cxx::dtime_tz_t::MAX_OFFSET && offset <= cxx::dtime_tz_t::MAX_OFFSET) {
		return static_cast<int32_t>(offset);
	}
	const auto abs_offset = offset < 0 ? -offset : offset;
	char text[64];
	snprintf(text, sizeof(text), "%s%02d:%02d:%02d", offset < 0 ? "-" : "", static_cast<int>(abs_offset / 3600),
	         static_cast<int>(abs_offset / 60 % 60), static_cast<int>(abs_offset % 60));
	throw cxx::Exception(DUCKDB_V2_ERROR_INPUT_OUT_OF_RANGE,
	                     std::string("Time zone offset ") + text +
	                         " is out of range, expected a value between -15:59:59 and +15:59:59");
}

namespace {

constexpr int64_t PRECISION_SEC = 1;
constexpr int64_t PRECISION_MS = 1000;
constexpr int64_t PRECISION_US = 1000000;
constexpr int64_t PRECISION_NS = 1000000000;
constexpr int64_t NANOS_PER_MICRO = 1000;
constexpr int64_t MICROS_PER_SEC = 1000000;

int64_t FloorMod(int64_t value, int64_t modulus) {
	auto result = value % modulus;
	return result < 0 ? result + modulus : result;
}

//----------------------------------------------------------------------------------------------------------------------
// Timestamp types by precision
//----------------------------------------------------------------------------------------------------------------------
//! Rescales between timestamp precisions: multiplies up (failing on overflow) or rounds away from zero
bool Rescale(int64_t value, int64_t from, int64_t to, int64_t &result) {
	if (from == to || !IsFinite(value)) {
		result = value;
		return true;
	}
	if (from < to) {
		return !__builtin_mul_overflow(value, to / from, &result);
	}
	const auto scaling = (from / to) / 2;
	result = value / scaling;
	result += result < 0 ? -1 : 1;
	result /= 2;
	if (to < PRECISION_US) {
		int64_t micros;
		if (__builtin_mul_overflow(result, PRECISION_US / to, &micros) || !IsFinite(micros)) {
			return false;
		}
	}
	return true;
}

template <class T>
struct Ts;

template <>
struct Ts<cxx::timestamp_s_t> {
	static constexpr int64_t PRECISION = PRECISION_SEC;
	static bool Load(const cxx::timestamp_s_t &v, int64_t precision, int64_t &out) {
		return Rescale(v.seconds, PRECISION, precision, out);
	}
	static cxx::timestamp_s_t Make(int64_t v) {
		return cxx::timestamp_s_t {v};
	}
};

template <>
struct Ts<cxx::timestamp_ms_t> {
	static constexpr int64_t PRECISION = PRECISION_MS;
	static bool Load(const cxx::timestamp_ms_t &v, int64_t precision, int64_t &out) {
		return Rescale(v.millis, PRECISION, precision, out);
	}
	static cxx::timestamp_ms_t Make(int64_t v) {
		return cxx::timestamp_ms_t {v};
	}
};

template <>
struct Ts<cxx::timestamp_t> {
	static constexpr int64_t PRECISION = PRECISION_US;
	static bool Load(const cxx::timestamp_t &v, int64_t precision, int64_t &out) {
		return Rescale(v.micros, PRECISION, precision, out);
	}
	static cxx::timestamp_t Make(int64_t v) {
		return cxx::timestamp_t {v};
	}
};

template <>
struct Ts<cxx::timestamp_tz_t> {
	static constexpr int64_t PRECISION = PRECISION_US;
	static bool Load(const cxx::timestamp_tz_t &v, int64_t precision, int64_t &out) {
		return Rescale(v.micros, PRECISION, precision, out);
	}
	static cxx::timestamp_tz_t Make(int64_t v) {
		return cxx::timestamp_tz_t {v};
	}
};

template <>
struct Ts<cxx::timestamp_ns_t> {
	static constexpr int64_t PRECISION = PRECISION_NS;
	static bool Load(const cxx::timestamp_ns_t &v, int64_t precision, int64_t &out) {
		return Rescale(v.nanos, PRECISION, precision, out);
	}
	static cxx::timestamp_ns_t Make(int64_t v) {
		return cxx::timestamp_ns_t {v};
	}
};

template <>
struct Ts<cxx::timestamp_tz_ns_t> {
	static constexpr int64_t PRECISION = PRECISION_NS;
	static bool Load(const cxx::timestamp_tz_ns_t &v, int64_t precision, int64_t &out) {
		return Rescale(v.nanos, PRECISION, precision, out);
	}
	static cxx::timestamp_tz_ns_t Make(int64_t v) {
		return cxx::timestamp_tz_ns_t {v};
	}
};

template <>
struct Ts<cxx::date_t> {
	static constexpr int64_t PRECISION = PRECISION_US;
	static bool Load(const cxx::date_t &v, int64_t precision, int64_t &out) {
		constexpr int32_t INFINITE_DAYS = std::numeric_limits<int32_t>::max();
		if (v.days == INFINITE_DAYS || v.days == -INFINITE_DAYS) {
			out = v.days > 0 ? std::numeric_limits<int64_t>::max() : -std::numeric_limits<int64_t>::max();
			return true;
		}
		int64_t micros;
		if (__builtin_mul_overflow(static_cast<int64_t>(v.days), MICROS_PER_DAY, &micros) || !IsFinite(micros)) {
			return false;
		}
		return Rescale(micros, PRECISION_US, precision, out);
	}
};

//----------------------------------------------------------------------------------------------------------------------
// Zone shifts at microsecond or nanosecond precision
//----------------------------------------------------------------------------------------------------------------------
bool CombineNanos(int64_t micros, int64_t nanos, int64_t &out, const char *&error) {
	if (__builtin_mul_overflow(micros, NANOS_PER_MICRO, &out) || __builtin_add_overflow(out, nanos, &out) ||
	    !IsFinite(out)) {
		error = "ICU date overflows timestamp_ns range";
		return false;
	}
	return true;
}

//! The instant a wall clock shows in the calendar's zone
bool ShiftFromLocal(icu_bridge::Calendar &calendar, int64_t local, int64_t precision, int64_t &out,
                    const char *&error) {
	if (!IsFinite(local)) {
		out = local;
		return true;
	}
	error = "ICU date overflows timestamp range";
	if (precision == PRECISION_US) {
		return calendar.FromLocal(local, out);
	}
	int64_t instant;
	if (!calendar.FromLocal(local / NANOS_PER_MICRO, instant)) {
		return false;
	}
	return CombineNanos(instant, local % NANOS_PER_MICRO, out, error);
}

//! The wall clock of an instant in the calendar's zone
bool ShiftToLocal(icu_bridge::Calendar &calendar, int64_t instant, int64_t precision, int64_t &out,
                  const char *&error) {
	if (!IsFinite(instant)) {
		out = instant;
		return true;
	}
	error = "ICU date overflows timestamp range";
	if (precision == PRECISION_US) {
		return calendar.ToLocal(instant, out);
	}
	int64_t local;
	if (!calendar.ToLocal(instant / NANOS_PER_MICRO, local)) {
		return false;
	}
	return CombineNanos(local, instant % NANOS_PER_MICRO, out, error);
}

//! Casts between timestamps and timestamps with time zone are refused when the setting says so
void CheckCastsEnabled(cxx::Context &context, const std::string &from, const std::string &to) {
	auto disabled = context.GetConfig().TryGetOption("disable_timestamptz_casts");
	if (disabled && !disabled->value.IsNull() && disabled->value.Get<bool>()) {
		throw cxx::Exception(DUCKDB_V2_ERROR_QUERY_BINDER, "Casting from " + from + " to " + to +
		                                                       " without an explicit time zone has been disabled  - "
		                                                       "use \"AT TIME ZONE ...\"");
	}
}

//----------------------------------------------------------------------------------------------------------------------
// Casts between naive timestamps and timestamps with time zone
//----------------------------------------------------------------------------------------------------------------------
template <class SRC, class DST>
void FromNaiveCast(cxx::CastFunction::ExecInput &input) {
	auto &context = input.GetContext();
	CheckCastsEnabled(context, "TIMESTAMP", "TIMESTAMP WITH TIME ZONE");
	auto &calendar = SettingsCalendar(ReadSettings(context));
	const auto in = input.GetInput().GetView();
	auto out = input.GetOutput();
	out.SetSize(input.GetRowCount());
	auto *data = out.template GetDataMutable<DST>();
	CastFailures failures(input);
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!in.IsValid(i)) {
			out.SetNull(i);
			continue;
		}
		int64_t naive;
		if (!Ts<SRC>::Load(in.template Data<SRC>()[in.SelAt(i)], Ts<DST>::PRECISION, naive)) {
			failures.Fail(out, i, "Could not convert timestamp to the precision of the target");
			continue;
		}
		int64_t instant;
		const char *error;
		if (!ShiftFromLocal(calendar, naive, Ts<DST>::PRECISION, instant, error)) {
			failures.Fail(out, i, error);
			continue;
		}
		data[i] = Ts<DST>::Make(instant);
	}
	failures.Report();
}

template <class SRC, class DST>
void ToNaiveCast(cxx::CastFunction::ExecInput &input, const char *target_name) {
	auto &context = input.GetContext();
	CheckCastsEnabled(context, "TIMESTAMP WITH TIME ZONE", target_name);
	auto &calendar = SettingsCalendar(ReadSettings(context));
	const auto in = input.GetInput().GetView();
	auto out = input.GetOutput();
	out.SetSize(input.GetRowCount());
	auto *data = out.template GetDataMutable<DST>();
	CastFailures failures(input);
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!in.IsValid(i)) {
			out.SetNull(i);
			continue;
		}
		int64_t instant;
		Ts<SRC>::Load(in.template Data<SRC>()[in.SelAt(i)], Ts<SRC>::PRECISION, instant);
		int64_t local;
		const char *error;
		if (!ShiftToLocal(calendar, instant, Ts<SRC>::PRECISION, local, error)) {
			failures.Fail(out, i, error);
			continue;
		}
		int64_t scaled;
		if (!Rescale(local, Ts<SRC>::PRECISION, Ts<DST>::PRECISION, scaled)) {
			failures.Fail(out, i, "Could not convert Timestamp to higher precision.");
			continue;
		}
		data[i] = Ts<DST>::Make(scaled);
	}
	failures.Report();
}

template <class SRC, class DST, const char *NAME>
void ToNaiveCastNamed(cxx::CastFunction::ExecInput &input) {
	ToNaiveCast<SRC, DST>(input, NAME);
}

constexpr char NAME_TIMESTAMP[] = "TIMESTAMP";
constexpr char NAME_TIMESTAMP_MS[] = "TIMESTAMP_MS";
constexpr char NAME_TIMESTAMP_NS[] = "TIMESTAMP_NS";
constexpr char NAME_TIMESTAMP_S[] = "TIMESTAMP_S";

// The costs are the ones CastRules::ImplicitCast gives the casts, negative where the engine has no implicit cast
constexpr int64_t COST_TO_TIMESTAMP_TZ = 123;
constexpr int64_t NO_IMPLICIT_CAST = -1;

//! TIMESTAMPTZ_NS cannot be parsed by name, so it is created by id
cxx::LogicalType NamedType(cxx::Factory &factory, const std::string &name) {
	if (name == "TIMESTAMPTZ_NS") {
		return factory.CreateType(cxx::LogicalTypeId::TIMESTAMP_TZ_NS);
	}
	return factory.ParseType(name);
}

void RegisterCast(cxx::Extension &extension, cxx::Factory &factory, const char *source, const char *target,
                  int64_t cost, cxx::CastFunction::ExecCallback callback) {
	auto cast = cxx::CastFunction::Create(factory);
	cast.SetSourceType(NamedType(factory, source)).SetTargetType(NamedType(factory, target)).SetExecCallback(callback);
	// The result depends on the TimeZone and Calendar settings, and the setting that refuses the cast is read when it
	// is bound
	cast.SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
	const std::string source_name = source;
	const bool from_time_zone = source_name != "TIMESTAMP" && source_name != "TIMESTAMP_MS" &&
	                            source_name != "TIMESTAMP_NS" && source_name != "TIMESTAMP_S" && source_name != "DATE";
	cast.SetUserData<DisabledCastNames>(from_time_zone ? DisabledCastNames {"TIMESTAMP WITH TIME ZONE", target}
	                                                   : DisabledCastNames {"TIMESTAMP", "TIMESTAMP WITH TIME ZONE"});
	cast.SetBindCallback(CheckDisabledCastAtBind);
	if (cost >= 0) {
		cast.SetImplicitCastCost(cost);
	}
	extension.Register(cast);
}

void RegisterNaiveCasts(cxx::Extension &extension, cxx::Factory &factory) {
	using namespace cxx;
	const char *tz = "TIMESTAMP WITH TIME ZONE";
	const char *tz_ns = "TIMESTAMPTZ_NS";
	RegisterCast(extension, factory, "TIMESTAMP", tz, COST_TO_TIMESTAMP_TZ, FromNaiveCast<timestamp_t, timestamp_tz_t>);
	RegisterCast(extension, factory, "TIMESTAMP_MS", tz, COST_TO_TIMESTAMP_TZ,
	             FromNaiveCast<timestamp_ms_t, timestamp_tz_t>);
	RegisterCast(extension, factory, "TIMESTAMP_NS", tz, NO_IMPLICIT_CAST,
	             FromNaiveCast<timestamp_ns_t, timestamp_tz_t>);
	RegisterCast(extension, factory, "TIMESTAMP_NS", tz_ns, NO_IMPLICIT_CAST,
	             FromNaiveCast<timestamp_ns_t, timestamp_tz_ns_t>);
	RegisterCast(extension, factory, "TIMESTAMP_S", tz, COST_TO_TIMESTAMP_TZ,
	             FromNaiveCast<timestamp_s_t, timestamp_tz_t>);
	RegisterCast(extension, factory, "DATE", tz, COST_TO_TIMESTAMP_TZ, FromNaiveCast<date_t, timestamp_tz_t>);

	RegisterCast(extension, factory, tz, "TIMESTAMP", NO_IMPLICIT_CAST,
	             ToNaiveCastNamed<timestamp_tz_t, timestamp_t, NAME_TIMESTAMP>);
	RegisterCast(extension, factory, tz, "TIMESTAMP_MS", NO_IMPLICIT_CAST,
	             ToNaiveCastNamed<timestamp_tz_t, timestamp_ms_t, NAME_TIMESTAMP_MS>);
	RegisterCast(extension, factory, tz, "TIMESTAMP_NS", NO_IMPLICIT_CAST,
	             ToNaiveCastNamed<timestamp_tz_t, timestamp_ns_t, NAME_TIMESTAMP_NS>);
	RegisterCast(extension, factory, tz, "TIMESTAMP_S", NO_IMPLICIT_CAST,
	             ToNaiveCastNamed<timestamp_tz_t, timestamp_s_t, NAME_TIMESTAMP_S>);
	RegisterCast(extension, factory, tz_ns, "TIMESTAMP", NO_IMPLICIT_CAST,
	             ToNaiveCastNamed<timestamp_tz_ns_t, timestamp_t, NAME_TIMESTAMP>);
	RegisterCast(extension, factory, tz_ns, "TIMESTAMP_NS", NO_IMPLICIT_CAST,
	             ToNaiveCastNamed<timestamp_tz_ns_t, timestamp_ns_t, NAME_TIMESTAMP_NS>);
}

//----------------------------------------------------------------------------------------------------------------------
// timezone(zone, value)
//----------------------------------------------------------------------------------------------------------------------
//! Resolves the zone of a row, remembering the previous one
struct ZoneLookup {
	explicit ZoneLookup(cxx::Context &context) : settings(ReadSettings(context)) {
	}

	icu_bridge::Calendar &Get(std::string_view zone) {
		if (!calendar || last != zone) {
			std::string error;
			last = std::string(zone);
			calendar = ZoneCalendar(settings, last, error);
			if (!calendar) {
				last.clear();
				throw cxx::Exception(DUCKDB_V2_ERROR_QUERY_NOT_IMPLEMENTED, error);
			}
		}
		return *calendar;
	}

	Settings settings;
	std::string last;
	icu_bridge::Calendar *calendar = nullptr;
};

//! timezone(zone, naive) -> instant when FROM_NAIVE, timezone(zone, instant) -> naive otherwise
template <class SRC, class DST, bool FROM_NAIVE>
void ZoneExec(cxx::ScalarFunction::ExecInput &input) {
	ZoneLookup lookup(input.GetContext());
	const auto zone = input.GetArg(0).GetView();
	const auto in = input.GetArg(1).GetView();
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	auto *out = result.template GetDataMutable<DST>();
	// A constant zone is checked even when no row needs it
	if (input.GetRowCount() > 0 && zone.IsValid(0) && input.GetArg(0).GetVectorType() == cxx::VectorType::CONSTANT) {
		lookup.Get(zone.template Data<cxx::varchar_t>()[zone.SelAt(0)].view());
	}
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!zone.IsValid(i) || !in.IsValid(i)) {
			result.SetNull(i);
			continue;
		}
		int64_t value;
		Ts<SRC>::Load(in.template Data<SRC>()[in.SelAt(i)], Ts<SRC>::PRECISION, value);
		if (!IsFinite(value)) {
			out[i] = Ts<DST>::Make(value);
			continue;
		}
		auto &calendar = lookup.Get(zone.template Data<cxx::varchar_t>()[zone.SelAt(i)].view());
		int64_t shifted;
		const char *error;
		const auto ok = FROM_NAIVE ? ShiftFromLocal(calendar, value, Ts<SRC>::PRECISION, shifted, error)
		                           : ShiftToLocal(calendar, value, Ts<SRC>::PRECISION, shifted, error);
		if (!ok) {
			ThrowConversion(error);
		}
		out[i] = Ts<DST>::Make(shifted);
	}
}

//! The UTC offset in seconds of a zone at the start of the transaction, checked to fit a TIME WITH TIME ZONE
int32_t ZoneOffsetNow(cxx::Context &context, icu_bridge::Calendar &calendar) {
	int64_t offset;
	if (!calendar.GetPart(context.GetTransactionStartTimestamp().micros, icu_bridge::Part::ZONE_OFFSET, offset)) {
		ThrowConversion("Unable to extract the time zone offset");
	}
	return CheckedTimeTZOffset(offset);
}

//! timezone(zone, timetz): the time of day in the zone with the zone's current offset
void ZoneTimeTZExec(cxx::ScalarFunction::ExecInput &input) {
	auto &context = input.GetContext();
	ZoneLookup lookup(context);
	const auto zone = input.GetArg(0).GetView();
	const auto in = input.GetArg(1).GetView();
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	auto *out = result.GetDataMutable<cxx::dtime_tz_t>();
	std::string offset_zone;
	int32_t offset = 0;
	bool have_offset = false;
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!zone.IsValid(i) || !in.IsValid(i)) {
			result.SetNull(i);
			continue;
		}
		const auto name = zone.Data<cxx::varchar_t>()[zone.SelAt(i)].view();
		auto &calendar = lookup.Get(name);
		if (!have_offset || offset_zone != name) {
			offset = ZoneOffsetNow(context, calendar);
			offset_zone = std::string(name);
			have_offset = true;
		}
		const auto value = in.Data<cxx::dtime_tz_t>()[in.SelAt(i)];
		const auto shift = (static_cast<int64_t>(offset) - value.GetOffset()) * MICROS_PER_SEC;
		out[i] = cxx::dtime_tz_t(FloorMod(value.GetMicros() + shift, MICROS_PER_DAY), offset);
	}
}

void RegisterZoneFunction(cxx::Extension &extension, cxx::Factory &factory, const char *source, const char *target,
                          cxx::ScalarFunction::ExecCallback callback) {
	auto function = cxx::ScalarFunction::Create(factory);
	function.SetName("timezone").SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
	function.SetExecCallback(callback);
	function.GetSignature()
	    .AddParameter("timezone", NamedType(factory, "VARCHAR"))
	    .AddParameter("timestamp", NamedType(factory, source))
	    .SetReturnType(NamedType(factory, target));
	extension.Register(function);
}

void RegisterZoneFunctions(cxx::Extension &extension, cxx::Factory &factory) {
	using namespace cxx;
	const char *tz = "TIMESTAMP WITH TIME ZONE";
	RegisterZoneFunction(extension, factory, "TIMESTAMP", tz, ZoneExec<timestamp_t, timestamp_tz_t, true>);
	RegisterZoneFunction(extension, factory, tz, "TIMESTAMP", ZoneExec<timestamp_tz_t, timestamp_t, false>);
	RegisterZoneFunction(extension, factory, "TIME WITH TIME ZONE", "TIME WITH TIME ZONE", ZoneTimeTZExec);
	RegisterZoneFunction(extension, factory, "TIMESTAMP_NS", "TIMESTAMPTZ_NS",
	                     ZoneExec<timestamp_ns_t, timestamp_tz_ns_t, true>);
	RegisterZoneFunction(extension, factory, "TIMESTAMPTZ_NS", "TIMESTAMP_NS",
	                     ZoneExec<timestamp_tz_ns_t, timestamp_ns_t, false>);
}

//----------------------------------------------------------------------------------------------------------------------
// Local time of the transaction start
//----------------------------------------------------------------------------------------------------------------------
int64_t LocalStart(cxx::Context &context) {
	auto &calendar = SettingsCalendar(ReadSettings(context));
	int64_t local;
	if (!calendar.ToLocal(context.GetTransactionStartTimestamp().micros, local)) {
		ThrowConversion("ICU date overflows timestamp range");
	}
	return local;
}

void LocalTimestampExec(cxx::ScalarFunction::ExecInput &input) {
	const auto local = LocalStart(input.GetContext());
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	auto *out = result.GetDataMutable<cxx::timestamp_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		out[i].micros = local;
	}
}

void LocalTimeExec(cxx::ScalarFunction::ExecInput &input) {
	const auto time_of_day = FloorMod(LocalStart(input.GetContext()), MICROS_PER_DAY);
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	auto *out = result.GetDataMutable<cxx::dtime_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		out[i].micros = time_of_day;
	}
}

void RegisterLocalFunction(cxx::Extension &extension, cxx::Factory &factory, const char *name, const char *type,
                           cxx::ScalarFunction::ExecCallback callback) {
	auto function = cxx::ScalarFunction::Create(factory);
	function.SetName(name).SetExecCallback(callback).SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
	function.GetSignature().SetReturnType(NamedType(factory, type));
	extension.Register(function);
}

} // namespace

void RegisterTimeZoneFunctions(cxx::Extension &extension, cxx::Factory &factory) {
	RegisterNaiveCasts(extension, factory);
	RegisterZoneFunctions(extension, factory);
	RegisterLocalFunction(extension, factory, "current_localtimestamp", "TIMESTAMP", LocalTimestampExec);
	RegisterLocalFunction(extension, factory, "current_localtime", "TIME", LocalTimeExec);
}

} // namespace icu_stable
