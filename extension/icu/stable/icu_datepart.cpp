#include "icu_stable_areas.hpp"
#include "icu_stable_common.hpp"

namespace icu_stable {

//! year(timestamptz) and friends, extracted in the time zone and calendar of the settings
template <icu_bridge::Part PART>
static void PartExec(cxx::ScalarFunction::ExecInput &input) {
	auto &calendar = SettingsCalendar(ReadSettings(input.GetContext()));
	const auto in = input.GetArg(0).GetView();
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	auto *out = result.GetDataMutable<int64_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!in.IsValid(i)) {
			result.SetNull(i);
			continue;
		}
		const auto instant = in.Data<cxx::timestamp_tz_t>()[in.SelAt(i)].micros;
		if (!IsFinite(instant)) {
			result.SetNull(i);
			continue;
		}
		if (!calendar.GetPart(instant, PART, out[i])) {
			ThrowConversion("Unable to extract calendar part");
		}
	}
}

template <icu_bridge::Part PART>
static void RegisterPart(cxx::Extension &extension, cxx::Factory &factory, const char *name) {
	auto function = cxx::ScalarFunction::Create(factory);
	function.SetName(name)
	    .SetExecCallback(PartExec<PART>)
	    .SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
	function.GetSignature()
	    .AddParameter("ts", factory.ParseType("TIMESTAMP WITH TIME ZONE"))
	    .SetReturnType(factory.ParseType("BIGINT"));
	extension.Register(function);
}

void RegisterDatePartFunctions(cxx::Extension &extension, cxx::Factory &factory) {
	RegisterPart<icu_bridge::Part::ERA>(extension, factory, "era");
	RegisterPart<icu_bridge::Part::YEAR>(extension, factory, "year");
	RegisterPart<icu_bridge::Part::MONTH>(extension, factory, "month");
	RegisterPart<icu_bridge::Part::DAY>(extension, factory, "day");
	RegisterPart<icu_bridge::Part::DAY_OF_WEEK>(extension, factory, "dayofweek");
	RegisterPart<icu_bridge::Part::DAY_OF_YEAR>(extension, factory, "dayofyear");
	RegisterPart<icu_bridge::Part::HOUR>(extension, factory, "hour");
	RegisterPart<icu_bridge::Part::MINUTE>(extension, factory, "minute");
	RegisterPart<icu_bridge::Part::SECOND>(extension, factory, "second");
	RegisterPart<icu_bridge::Part::MILLISECOND>(extension, factory, "millisecond");
	RegisterPart<icu_bridge::Part::MICROSECOND>(extension, factory, "microsecond");
}

} // namespace icu_stable
