#include "icu_stable_areas.hpp"
#include "icu_stable_common.hpp"
#include "icu_bridge_datepart.hpp"

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace icu_stable {

using icu_bridge::DatePart;

//----------------------------------------------------------------------------------------------------------------------
// Shared pieces
//----------------------------------------------------------------------------------------------------------------------
namespace {

//! The calendar of one execution thread. The week parts change the week rules of the calendar, so it is not shared.
struct DatePartState {
	std::unique_ptr<icu_bridge::Calendar> calendar;
};

//! The part a single-part function extracts
struct PartInfo {
	DatePart part;
};

//! The part (when constant) and the struct fields of a date_part call
struct PartBind {
	bool constant_part = false;
	DatePart part = DatePart::YEAR;
	std::vector<DatePart> parts;

	bool operator==(const PartBind &other) const {
		return constant_part == other.constant_part && part == other.part && parts == other.parts;
	}
};

template <class T>
struct TimeTraits;

template <>
struct TimeTraits<cxx::timestamp_tz_t> {
	static bool IsFiniteValue(cxx::timestamp_tz_t value) {
		return IsFinite(value.micros);
	}
	static int64_t Instant(cxx::timestamp_tz_t value) {
		return value.micros;
	}
	static cxx::LogicalType Type(cxx::Factory &factory) {
		return factory.ParseType("TIMESTAMP WITH TIME ZONE");
	}
};

template <>
struct TimeTraits<cxx::timestamp_tz_ns_t> {
	static bool IsFiniteValue(cxx::timestamp_tz_ns_t value) {
		return IsFinite(value.nanos);
	}
	//! Truncates towards -infinity, not the epoch
	static int64_t Instant(cxx::timestamp_tz_ns_t value) {
		auto micros = value.nanos / 1000;
		if (value.nanos % 1000 < 0) {
			--micros;
		}
		return micros;
	}
	static cxx::LogicalType Type(cxx::Factory &factory) {
		return factory.ParseType("TIMESTAMPTZ_NS");
	}
};

void InitState(cxx::ScalarFunction::InitInput &input) {
	// Without a context (an index key being evaluated) the defaults apply
	const auto settings = input.HasContext() ? ReadSettings(input.GetContext()) : Settings();
	input.SetInitData<DatePartState>(
	    DatePartState {icu_bridge::Calendar::Create(settings.time_zone, settings.calendar)});
}

DatePart ParsePart(const std::string &name) {
	DatePart part;
	if (!icu_bridge::TryParseDatePart(name, part)) {
		ThrowConversion("extract specifier \"" + name + "\" not recognized");
	}
	return part;
}

[[noreturn]] void ThrowExtract() {
	ThrowConversion("Unable to extract calendar part");
}

//! Calls fn(calendar, instant, row) for every finite row, and writes NULL for the other rows
template <class IN, class FN>
void ForEachInstant(cxx::ScalarFunction::ExecInput &input, cxx::Vector &result, FN &&fn) {
	auto &calendar = *input.GetInitData<DatePartState>().calendar;
	const auto in = input.GetArg(input.GetArgCount() - 1).GetView();
	const auto count = input.GetRowCount();
	for (idx_t i = 0; i < count; i++) {
		if (!in.IsValid(i)) {
			result.SetNull(i);
			continue;
		}
		const auto value = in.Data<IN>()[in.SelAt(i)];
		if (!TimeTraits<IN>::IsFiniteValue(value)) {
			result.SetNull(i);
			continue;
		}
		fn(calendar, TimeTraits<IN>::Instant(value), i);
	}
}

//----------------------------------------------------------------------------------------------------------------------
// Single-part functions
//----------------------------------------------------------------------------------------------------------------------
template <class IN>
void BigintPartExec(cxx::ScalarFunction::ExecInput &input) {
	const auto part = input.GetUserData<PartInfo>().part;
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	auto *out = result.GetDataMutable<int64_t>();
	ForEachInstant<IN>(input, result, [&](icu_bridge::Calendar &calendar, int64_t instant, idx_t row) {
		if (!icu_bridge::ExtractBigintPart(calendar, part, instant, out[row])) {
			ThrowExtract();
		}
	});
}

template <class IN>
void DoublePartExec(cxx::ScalarFunction::ExecInput &input) {
	const auto part = input.GetUserData<PartInfo>().part;
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	auto *out = result.GetDataMutable<double>();
	ForEachInstant<IN>(input, result, [&](icu_bridge::Calendar &calendar, int64_t instant, idx_t row) {
		if (!icu_bridge::ExtractDoublePart(calendar, part, instant, out[row])) {
			ThrowExtract();
		}
	});
}

template <class IN>
void LastDayExec(cxx::ScalarFunction::ExecInput &input) {
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	auto *out = result.GetDataMutable<cxx::date_t>();
	ForEachInstant<IN>(input, result, [&](icu_bridge::Calendar &calendar, int64_t instant, idx_t row) {
		if (!icu_bridge::ExtractLastDay(calendar, instant, out[row].days)) {
			ThrowExtract();
		}
	});
}

template <class IN, bool (*EXTRACT)(icu_bridge::Calendar &, int64_t, std::string_view &)>
void NameExec(cxx::ScalarFunction::ExecInput &input) {
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	ForEachInstant<IN>(input, result, [&](icu_bridge::Calendar &calendar, int64_t instant, idx_t row) {
		std::string_view name;
		if (!EXTRACT(calendar, instant, name)) {
			ThrowExtract();
		}
		result.AssignString(row, name);
	});
}

//! The epoch is read from the instant itself: no calendar and no client context are needed
template <class IN>
void EpochExec(cxx::ScalarFunction::ExecInput &input) {
	const auto in = input.GetArg(0).GetView();
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	auto *out = result.GetDataMutable<double>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!in.IsValid(i)) {
			result.SetNull(i);
			continue;
		}
		const auto value = in.Data<IN>()[in.SelAt(i)];
		if (!TimeTraits<IN>::IsFiniteValue(value)) {
			result.SetNull(i);
			continue;
		}
		out[i] = icu_bridge::ExtractEpochOfInstant(TimeTraits<IN>::Instant(value));
	}
}

//! Registers a function over TIMESTAMPTZ and TIMESTAMPTZ_NS
template <class SETUP>
void RegisterOverloads(cxx::Extension &extension, cxx::Factory &factory, const char *name, SETUP &&setup,
                       bool needs_calendar = true) {
	for (int ns = 0; ns < 2; ns++) {
		auto function = cxx::ScalarFunction::Create(factory);
		function.SetName(name).SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
		if (needs_calendar) {
			function.SetInitCallback(InitState);
		}
		setup(function, ns == 1);
		extension.Register(function);
	}
}

void RegisterSinglePart(cxx::Extension &extension, cxx::Factory &factory, const char *name) {
	DatePart part;
	if (!icu_bridge::TryParseDatePart(name, part)) {
		ThrowConversion(std::string("unknown date part function ") + name);
	}
	const auto is_double = icu_bridge::IsDoublePart(part);
	RegisterOverloads(
	    extension, factory, name,
	    [&](cxx::ScalarFunction &function, bool ns) {
		    function.SetUserData<PartInfo>(PartInfo {part});
		    if (part == DatePart::ERA || part == DatePart::ISOYEAR) {
			    function.SetArgumentMonotonicity(0, cxx::Monotonicity::NON_DECREASING)
			        .SetStatisticsPropagation(cxx::StatisticsPropagation::FINITE_MONOTONE_BOUNDS);
		    }
		    if (part == DatePart::EPOCH) {
			    // The epoch does not depend on the time zone
			    function.SetStability(cxx::FunctionStability::CONSISTENT);
		    }
		    if (part == DatePart::EPOCH) {
			    function.SetExecCallback(ns ? EpochExec<cxx::timestamp_tz_ns_t> : EpochExec<cxx::timestamp_tz_t>);
		    } else if (ns) {
			    function.SetExecCallback(is_double ? DoublePartExec<cxx::timestamp_tz_ns_t>
			                                       : BigintPartExec<cxx::timestamp_tz_ns_t>);
		    } else {
			    function.SetExecCallback(is_double ? DoublePartExec<cxx::timestamp_tz_t>
			                                       : BigintPartExec<cxx::timestamp_tz_t>);
		    }
		    const auto input_type =
		        ns ? TimeTraits<cxx::timestamp_tz_ns_t>::Type(factory) : TimeTraits<cxx::timestamp_tz_t>::Type(factory);
		    function.GetSignature()
		        .AddParameter("ts", input_type)
		        .SetReturnType(factory.ParseType(is_double ? "DOUBLE" : "BIGINT"));
	    },
	    part != DatePart::EPOCH);
}

template <cxx::ScalarFunction::ExecCallback TZ, cxx::ScalarFunction::ExecCallback TZ_NS>
void RegisterTimestampFunction(cxx::Extension &extension, cxx::Factory &factory, const char *name,
                               const char *result_type) {
	RegisterOverloads(extension, factory, name, [&](cxx::ScalarFunction &function, bool ns) {
		function.SetExecCallback(ns ? TZ_NS : TZ);
		const auto input_type =
		    ns ? TimeTraits<cxx::timestamp_tz_ns_t>::Type(factory) : TimeTraits<cxx::timestamp_tz_t>::Type(factory);
		function.GetSignature().AddParameter("ts", input_type).SetReturnType(factory.ParseType(result_type));
	});
}

//----------------------------------------------------------------------------------------------------------------------
// date_part(part, ts) and date_part([parts], ts)
//----------------------------------------------------------------------------------------------------------------------
void BinaryBind(cxx::ScalarFunction::BindInput &input) {
	const auto part = input.TryGetConstantArgument(0);
	if (part && !part->IsNull()) {
		// A constant part is bound to the equivalent single-part function
		const auto parsed = ParsePart(std::string(part->Get<cxx::varchar_t>().view()));
		input.ReplaceCall(icu_bridge::UnaryFunctionName(parsed), {1});
		return;
	}
	input.SetBindData<PartBind>(PartBind());
}

template <class IN>
void BinaryExec(cxx::ScalarFunction::ExecInput &input) {
	const auto &bind = input.GetBindData<PartBind>();
	const auto parts = input.GetArg(0).GetView();
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	const auto is_bigint = bind.constant_part && !icu_bridge::IsDoublePart(bind.part);
	auto *bigints = is_bigint ? result.GetDataMutable<int64_t>() : nullptr;
	auto *doubles = is_bigint ? nullptr : result.GetDataMutable<double>();
	ForEachInstant<IN>(input, result, [&](icu_bridge::Calendar &calendar, int64_t instant, idx_t row) {
		auto part = bind.part;
		if (!bind.constant_part) {
			if (!parts.IsValid(row)) {
				result.SetNull(row);
				return;
			}
			part = ParsePart(std::string(parts.Data<cxx::varchar_t>()[parts.SelAt(row)].view()));
		}
		if (icu_bridge::IsDoublePart(part)) {
			double value;
			if (!icu_bridge::ExtractDoublePart(calendar, part, instant, value)) {
				ThrowExtract();
			}
			doubles[row] = value;
		} else {
			int64_t value;
			if (!icu_bridge::ExtractBigintPart(calendar, part, instant, value)) {
				ThrowExtract();
			}
			if (is_bigint) {
				bigints[row] = value;
			} else {
				doubles[row] = static_cast<double>(value);
			}
		}
	});
}

//! The part names of the constant list argument
std::vector<std::pair<std::string, DatePart>> ReadPartListValue(const cxx::Value &list) {
	if (list.GetLogicalType().GetTypeId() != cxx::LogicalTypeId::LIST) {
		ThrowConversion("date_part can only take constant lists of part names");
	}
	if (list.IsNull() || list.GetChildCount() == 0) {
		ThrowConversion("date_part requires non-empty lists of part names");
	}
	std::set<std::string> seen;
	std::vector<std::pair<std::string, DatePart>> result;
	for (idx_t i = 0; i < list.GetChildCount(); i++) {
		const auto child = list.GetChild(i);
		if (child.IsNull()) {
			ThrowConversion("NULL struct entry name in date_part");
		}
		auto name = std::string(child.Get<cxx::varchar_t>().view());
		const auto part = ParsePart(name);
		std::string lower;
		for (auto c : name) {
			lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		if (!seen.insert(lower).second) {
			ThrowConversion("Duplicate struct entry name \"" + name + "\" in date_part");
		}
		result.emplace_back(std::move(name), part);
	}
	return result;
}

void StructResolveTypes(cxx::ScalarFunction::ResolveTypesInput &input) {
	const auto list = input.TryGetConstantArgument(0);
	if (!list) {
		ThrowConversion("date_part can only take constant lists of part names");
	}
	std::string type = "STRUCT(";
	auto first = true;
	for (const auto &entry : ReadPartListValue(*list)) {
		type += first ? "\"" : ", \"";
		first = false;
		type += entry.first + "\" " + (icu_bridge::IsDoublePart(entry.second) ? "DOUBLE" : "BIGINT");
	}
	type += ")";
	input.SetReturnType(input.GetContext().GetFactory().ParseType(type));
}

void StructBind(cxx::ScalarFunction::BindInput &input) {
	PartBind bind;
	for (const auto &entry : ReadPartListValue(input.GetConstantArgument(0))) {
		bind.parts.push_back(entry.second);
	}
	// The epoch does not depend on the time zone, a call that only asks for it is consistent
	bool only_epoch = true;
	for (const auto part : bind.parts) {
		only_epoch = only_epoch && part == DatePart::EPOCH;
	}
	if (only_epoch) {
		input.SetStability(cxx::FunctionStability::CONSISTENT);
	}
	input.SetBindData<PartBind>(std::move(bind));
}

template <class IN>
void StructExec(cxx::ScalarFunction::ExecInput &input) {
	const auto &parts = input.GetBindData<PartBind>().parts;
	auto &calendar = *input.GetInitData<DatePartState>().calendar;
	const auto in = input.GetArg(1).GetView();
	const auto count = input.GetRowCount();
	auto result = input.GetResult();
	result.SetSize(count);
	std::vector<cxx::Vector> children;
	for (idx_t col = 0; col < parts.size(); col++) {
		children.push_back(result.GetChild(col));
		children.back().SetSize(count);
	}
	for (idx_t i = 0; i < count; i++) {
		if (!in.IsValid(i)) {
			result.SetNull(i);
			for (auto &child : children) {
				child.SetNull(i);
			}
			continue;
		}
		const auto value = in.Data<IN>()[in.SelAt(i)];
		if (!TimeTraits<IN>::IsFiniteValue(value)) {
			for (auto &child : children) {
				child.SetNull(i);
			}
			continue;
		}
		const auto instant = TimeTraits<IN>::Instant(value);
		for (idx_t col = 0; col < parts.size(); col++) {
			if (icu_bridge::IsDoublePart(parts[col])) {
				if (!icu_bridge::ExtractDoublePart(calendar, parts[col], instant,
				                                   children[col].GetDataMutable<double>()[i])) {
					ThrowExtract();
				}
			} else if (!icu_bridge::ExtractBigintPart(calendar, parts[col], instant,
			                                          children[col].GetDataMutable<int64_t>()[i])) {
				ThrowExtract();
			}
		}
	}
}

void RegisterDatePartFunction(cxx::Extension &extension, cxx::Factory &factory, const char *name) {
	const auto any = factory.CreateType(cxx::LogicalTypeId::ANY);
	const auto varchar = factory.ParseType("VARCHAR");
	const auto varchar_list = factory.ParseType("VARCHAR[]");
	for (int ns = 0; ns < 2; ns++) {
		const auto input_type =
		    ns ? TimeTraits<cxx::timestamp_tz_ns_t>::Type(factory) : TimeTraits<cxx::timestamp_tz_t>::Type(factory);

		auto binary = cxx::ScalarFunction::Create(factory);
		binary.SetName(name)
		    .SetBindCallback(BinaryBind)
		    .SetInitCallback(InitState)
		    .SetExecCallback(ns ? BinaryExec<cxx::timestamp_tz_ns_t> : BinaryExec<cxx::timestamp_tz_t>)
		    .SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
		// A constant part is bound to the single-part function, which sets the type of the call
		binary.GetSignature()
		    .AddParameter("part", varchar)
		    .AddParameter("ts", input_type)
		    .SetReturnType(factory.ParseType("DOUBLE"));
		extension.Register(binary);

		auto structs = cxx::ScalarFunction::Create(factory);
		structs.SetName(name)
		    .SetResolveTypesCallback(StructResolveTypes)
		    .SetBindCallback(StructBind)
		    .SetInitCallback(InitState)
		    .SetExecCallback(ns ? StructExec<cxx::timestamp_tz_ns_t> : StructExec<cxx::timestamp_tz_t>)
		    .SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
		structs.GetSignature()
		    .AddParameter("part_list", varchar_list)
		    .AddParameter("ts", input_type)
		    .SetReturnType(any);
		extension.Register(structs);
	}
}

} // namespace

void RegisterDatePartFunctions(cxx::Extension &extension, cxx::Factory &factory) {
	// BIGINT parts
	for (auto name :
	     {"era",         "year",        "month",   "day",       "decade",   "century",       "millennium",
	      "microsecond", "millisecond", "second",  "minute",    "hour",     "dayofweek",     "isodow",
	      "week",        "dayofyear",   "quarter", "isoyear",   "timezone", "timezone_hour", "timezone_minute",
	      "yearweek",    "dayofmonth",  "weekday", "weekofyear"}) {
		RegisterSinglePart(extension, factory, name);
	}
	// DOUBLE parts
	RegisterSinglePart(extension, factory, "epoch");
	RegisterSinglePart(extension, factory, "julian");

	RegisterTimestampFunction<LastDayExec<cxx::timestamp_tz_t>, LastDayExec<cxx::timestamp_tz_ns_t>>(
	    extension, factory, "last_day", "DATE");
	RegisterTimestampFunction<NameExec<cxx::timestamp_tz_t, icu_bridge::ExtractMonthName>,
	                          NameExec<cxx::timestamp_tz_ns_t, icu_bridge::ExtractMonthName>>(extension, factory,
	                                                                                          "monthname", "VARCHAR");
	RegisterTimestampFunction<NameExec<cxx::timestamp_tz_t, icu_bridge::ExtractDayName>,
	                          NameExec<cxx::timestamp_tz_ns_t, icu_bridge::ExtractDayName>>(extension, factory,
	                                                                                        "dayname", "VARCHAR");

	RegisterDatePartFunction(extension, factory, "date_part");
	RegisterDatePartFunction(extension, factory, "datepart");
}

} // namespace icu_stable
