#include "icu_stable_areas.hpp"
#include "icu_stable_common.hpp"

#include "icu_bridge_arithmetic.hpp"

#include <map>
#include <memory>

namespace icu_stable {

namespace {

//! Per-thread calendar of the settings, private to the arithmetic functions because the kernels reconfigure it
icu_bridge::Calendar &ArithmeticCalendar(const Settings &settings) {
	thread_local std::unique_ptr<icu_bridge::Calendar> cached;
	thread_local Settings cached_settings;
	if (!cached || !(cached_settings == settings)) {
		cached = icu_bridge::Calendar::Create(settings.time_zone, settings.calendar);
		cached_settings = settings;
	}
	return *cached;
}

[[noreturn]] void Rethrow(const icu_bridge::ArithmeticError &error) {
	int code = DUCKDB_V2_ERROR_RUNTIME_INTERNAL;
	switch (error.kind) {
	case icu_bridge::ArithmeticErrorKind::CONVERSION:
		code = DUCKDB_V2_ERROR_TYPE_CONVERSION;
		break;
	case icu_bridge::ArithmeticErrorKind::INVALID_INPUT:
		code = DUCKDB_V2_ERROR_INPUT_INVALID;
		break;
	case icu_bridge::ArithmeticErrorKind::OUT_OF_RANGE:
		code = DUCKDB_V2_ERROR_INPUT_OUT_OF_RANGE;
		break;
	case icu_bridge::ArithmeticErrorKind::NOT_IMPLEMENTED:
		code = DUCKDB_V2_ERROR_QUERY_NOT_IMPLEMENTED;
		break;
	default:
		break;
	}
	throw cxx::Exception(code, error.what());
}

//! Runs the body, translating the errors of the bridge
template <class F>
void Translate(F &&body) {
	try {
		body();
	} catch (const icu_bridge::ArithmeticError &error) {
		Rethrow(error);
	}
}

icu_bridge::IntervalValue ToBridge(const cxx::interval_t &interval) {
	return icu_bridge::IntervalValue {interval.months, interval.days, interval.micros};
}

cxx::interval_t FromBridge(const icu_bridge::IntervalValue &interval) {
	return cxx::interval_t {interval.months, interval.days, interval.micros};
}

//! Intervals do not work on calendars with 13 months
void BindIntervalMonths(cxx::ScalarFunction::BindInput &input) {
	auto &calendar = ArithmeticCalendar(ReadSettings(input.GetContext()));
	if (!icu_bridge::SupportsIntervals(calendar)) {
		throw cxx::Exception(DUCKDB_V2_ERROR_QUERY_NOT_IMPLEMENTED,
		                     "INTERVALs do not work with 13 month calendars. Try using DATE_DIFF instead.");
	}
}

//----------------------------------------------------------------------------------------------------------------------
// timestamptz +/- interval
//----------------------------------------------------------------------------------------------------------------------
//! TIMESTAMPTZ_ARG is the position of the timestamp argument, SUBTRACT negates the interval
template <idx_t TIMESTAMP_ARG, bool SUBTRACT>
void AddIntervalExec(cxx::ScalarFunction::ExecInput &input) {
	auto &calendar = ArithmeticCalendar(ReadSettings(input.GetContext()));
	const auto timestamps = input.GetArg(TIMESTAMP_ARG).GetView();
	const auto intervals = input.GetArg(1 - TIMESTAMP_ARG).GetView();
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	auto *out = result.GetDataMutable<cxx::timestamp_tz_t>();
	Translate([&] {
		for (idx_t i = 0; i < input.GetRowCount(); i++) {
			if (!timestamps.IsValid(i) || !intervals.IsValid(i)) {
				result.SetNull(i);
				continue;
			}
			const auto timestamp = timestamps.Data<cxx::timestamp_tz_t>()[timestamps.SelAt(i)].micros;
			const auto interval = ToBridge(intervals.Data<cxx::interval_t>()[intervals.SelAt(i)]);
			out[i].micros = SUBTRACT ? icu_bridge::SubInterval(calendar, timestamp, interval)
			                         : icu_bridge::AddInterval(calendar, timestamp, interval);
		}
	});
}

//! timestamptz - timestamptz -> interval
void SubTimestampsExec(cxx::ScalarFunction::ExecInput &input) {
	auto &calendar = ArithmeticCalendar(ReadSettings(input.GetContext()));
	const auto left = input.GetArg(0).GetView();
	const auto right = input.GetArg(1).GetView();
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	auto *out = result.GetDataMutable<cxx::interval_t>();
	Translate([&] {
		for (idx_t i = 0; i < input.GetRowCount(); i++) {
			if (!left.IsValid(i) || !right.IsValid(i)) {
				result.SetNull(i);
				continue;
			}
			const auto end = left.Data<cxx::timestamp_tz_t>()[left.SelAt(i)].micros;
			const auto start = right.Data<cxx::timestamp_tz_t>()[right.SelAt(i)].micros;
			out[i] = FromBridge(icu_bridge::SubTimestamps(calendar, end, start));
		}
	});
}

//----------------------------------------------------------------------------------------------------------------------
// age
//----------------------------------------------------------------------------------------------------------------------
void AgeBinaryExec(cxx::ScalarFunction::ExecInput &input) {
	const auto settings = ReadSettings(input.GetContext());
	auto &calendar = ArithmeticCalendar(settings);
	const auto gregorian = icu_bridge::IsGregorianCalendar(settings.calendar);
	const auto left = input.GetArg(0).GetView();
	const auto right = input.GetArg(1).GetView();
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	auto *out = result.GetDataMutable<cxx::interval_t>();
	Translate([&] {
		for (idx_t i = 0; i < input.GetRowCount(); i++) {
			if (!left.IsValid(i) || !right.IsValid(i)) {
				result.SetNull(i);
				continue;
			}
			const auto end = left.Data<cxx::timestamp_tz_t>()[left.SelAt(i)].micros;
			const auto start = right.Data<cxx::timestamp_tz_t>()[right.SelAt(i)].micros;
			out[i] = FromBridge(icu_bridge::Age(calendar, gregorian, end, start));
		}
	});
}

//! age(timestamp): the age at the midnight of the transaction start
void AgeUnaryExec(cxx::ScalarFunction::ExecInput &input) {
	auto &context = input.GetContext();
	const auto settings = ReadSettings(context);
	auto &calendar = ArithmeticCalendar(settings);
	const auto gregorian = icu_bridge::IsGregorianCalendar(settings.calendar);
	const auto in = input.GetArg(0).GetView();
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	auto *out = result.GetDataMutable<cxx::interval_t>();
	Translate([&] {
		const auto end = icu_bridge::MidnightOf(calendar, context.GetTransactionStartTimestamp().micros);
		for (idx_t i = 0; i < input.GetRowCount(); i++) {
			if (!in.IsValid(i)) {
				result.SetNull(i);
				continue;
			}
			const auto start = in.Data<cxx::timestamp_tz_t>()[in.SelAt(i)].micros;
			out[i] = FromBridge(icu_bridge::Age(calendar, gregorian, end, start));
		}
	});
}

//----------------------------------------------------------------------------------------------------------------------
// date_sub and date_diff
//----------------------------------------------------------------------------------------------------------------------
enum class DiffKind { SUB, DIFF, DIFF_NANOS };

//! Parses the part specifier of the previous row only once
struct PartCache {
	bool valid = false;
	std::string specifier;
	icu_bridge::DiffPart part;

	icu_bridge::DiffPart Get(std::string_view text) {
		if (!valid || specifier != text) {
			part = icu_bridge::ParseDiffPart(std::string(text));
			specifier = std::string(text);
			valid = true;
		}
		return part;
	}
};

template <class T, DiffKind KIND>
void DateDiffExec(cxx::ScalarFunction::ExecInput &input) {
	auto &calendar = ArithmeticCalendar(ReadSettings(input.GetContext()));
	const auto parts = input.GetArg(0).GetView();
	const auto starts = input.GetArg(1).GetView();
	const auto ends = input.GetArg(2).GetView();
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	auto *out = result.GetDataMutable<int64_t>();
	PartCache cache;
	Translate([&] {
		for (idx_t i = 0; i < input.GetRowCount(); i++) {
			if (!parts.IsValid(i) || !starts.IsValid(i) || !ends.IsValid(i)) {
				result.SetNull(i);
				continue;
			}
			const auto start = starts.Data<T>()[starts.SelAt(i)];
			const auto end = ends.Data<T>()[ends.SelAt(i)];
			int64_t start_value;
			int64_t end_value;
			if constexpr (std::is_same_v<T, cxx::timestamp_tz_ns_t>) {
				start_value = start.nanos;
				end_value = end.nanos;
			} else {
				start_value = start.micros;
				end_value = end.micros;
			}
			if (!IsFinite(start_value) || !IsFinite(end_value)) {
				result.SetNull(i);
				continue;
			}
			const auto part = cache.Get(parts.Data<cxx::varchar_t>()[parts.SelAt(i)].view());
			if constexpr (KIND == DiffKind::SUB) {
				out[i] = icu_bridge::DateSub(calendar, part, start_value, end_value);
			} else if constexpr (KIND == DiffKind::DIFF) {
				out[i] = icu_bridge::DateDiff(calendar, part, start_value, end_value);
			} else {
				out[i] = icu_bridge::DateDiffNanos(calendar, part, start_value, end_value);
			}
		}
	});
}

//----------------------------------------------------------------------------------------------------------------------
// Registration
//----------------------------------------------------------------------------------------------------------------------
void RegisterOperator(cxx::Extension &extension, cxx::Factory &factory, const char *name,
                      cxx::ScalarFunction::ExecCallback exec, const char *left_type, const char *right_type,
                      const char *result_type) {
	auto function = cxx::ScalarFunction::Create(factory);
	function.SetName(name)
	    .SetBindCallback(BindIntervalMonths)
	    .SetExecCallback(exec)
	    .SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
	function.GetSignature()
	    .AddParameter("left", factory.ParseType(left_type))
	    .AddParameter("right", factory.ParseType(right_type))
	    .SetReturnType(factory.ParseType(result_type));
	extension.Register(function);
}

void RegisterDiffFunction(cxx::Extension &extension, cxx::Factory &factory, const char *name,
                          cxx::ScalarFunction::ExecCallback exec, const char *type) {
	auto function = cxx::ScalarFunction::Create(factory);
	function.SetName(name).SetExecCallback(exec).SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
	function.GetSignature()
	    .AddParameter("part", factory.ParseType("VARCHAR"))
	    .AddParameter("startdate", factory.ParseType(type))
	    .AddParameter("enddate", factory.ParseType(type))
	    .SetReturnType(factory.ParseType("BIGINT"));
	extension.Register(function);
}

} // namespace

void RegisterArithmeticFunctions(cxx::Extension &extension, cxx::Factory &factory) {
	const char *tstz = "TIMESTAMP WITH TIME ZONE";
	const char *interval = "INTERVAL";
	RegisterOperator(extension, factory, "+", AddIntervalExec<0, false>, tstz, interval, tstz);
	RegisterOperator(extension, factory, "+", AddIntervalExec<1, false>, interval, tstz, tstz);
	RegisterOperator(extension, factory, "-", AddIntervalExec<0, true>, tstz, interval, tstz);
	RegisterOperator(extension, factory, "-", SubTimestampsExec, tstz, tstz, interval);

	auto age = cxx::ScalarFunction::Create(factory);
	age.SetName("age")
	    .SetBindCallback(BindIntervalMonths)
	    .SetExecCallback(AgeBinaryExec)
	    .SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
	age.GetSignature()
	    .AddParameter("timestamp1", factory.ParseType(tstz))
	    .AddParameter("timestamp2", factory.ParseType(tstz))
	    .SetReturnType(factory.ParseType(interval));
	extension.Register(age);

	auto age_unary = cxx::ScalarFunction::Create(factory);
	age_unary.SetName("age")
	    .SetBindCallback(BindIntervalMonths)
	    .SetExecCallback(AgeUnaryExec)
	    .SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
	age_unary.GetSignature()
	    .AddParameter("timestamp", factory.ParseType(tstz))
	    .SetReturnType(factory.ParseType(interval));
	extension.Register(age_unary);

	for (const char *name : {"date_sub", "datesub"}) {
		RegisterDiffFunction(extension, factory, name, DateDiffExec<cxx::timestamp_tz_t, DiffKind::SUB>, tstz);
	}
	for (const char *name : {"date_diff", "datediff"}) {
		RegisterDiffFunction(extension, factory, name, DateDiffExec<cxx::timestamp_tz_t, DiffKind::DIFF>, tstz);
		RegisterDiffFunction(extension, factory, name, DateDiffExec<cxx::timestamp_tz_ns_t, DiffKind::DIFF_NANOS>,
		                     "TIMESTAMPTZ_NS");
	}
}

} // namespace icu_stable
