//===----------------------------------------------------------------------===//
//                         DuckDB
//
// icu_stable_extension.cpp
//
// Proof of concept: the ICU extension on the stable C++ API. Only duckdb_cpp_extension.hpp is included here, the
// vendored calendar and collation engine is reached through icu_bridge.hpp.
//
//===----------------------------------------------------------------------===//

#include "duckdb_cpp_extension.hpp"

#include "icu_bridge.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace cxx = duckdb::cxx;
using cxx::idx_t;

namespace {

constexpr int64_t MICROS_PER_DAY = 86400LL * 1000000LL;

bool IsFinite(int64_t micros) {
	return micros != std::numeric_limits<int64_t>::max() && micros != -std::numeric_limits<int64_t>::max();
}

[[noreturn]] void ThrowConversion(const std::string &message) {
	throw cxx::Exception(DUCKDB_V2_ERROR_TYPE_CONVERSION, message);
}

//----------------------------------------------------------------------------------------------------------------------
// Settings and calendars
//----------------------------------------------------------------------------------------------------------------------
struct Settings {
	std::string time_zone = "UTC";
	std::string calendar = "gregorian";

	bool operator==(const Settings &other) const {
		return time_zone == other.time_zone && calendar == other.calendar;
	}
};

std::string ReadString(const cxx::Config &config, const char *name, const std::string &fallback) {
	auto option = config.TryGetOption(name);
	if (!option || option->value.IsNull()) {
		return fallback;
	}
	return std::string(option->value.Get<cxx::varchar_t>().view());
}

Settings ReadSettings(cxx::Context &context) {
	Settings settings;
	const auto &config = context.GetConfig();
	settings.time_zone = ReadString(config, "TimeZone", settings.time_zone);
	settings.calendar = ReadString(config, "Calendar", settings.calendar);
	return settings;
}

//! The calendar of the settings, kept per thread because creating one parses the time zone
icu_bridge::Calendar &SettingsCalendar(const Settings &settings) {
	thread_local std::unique_ptr<icu_bridge::Calendar> cached;
	thread_local Settings cached_settings;
	if (!cached || !(cached_settings == settings)) {
		cached = icu_bridge::Calendar::Create(settings.time_zone, settings.calendar);
		cached_settings = settings;
	}
	return *cached;
}

//! The calendar of an arbitrary time zone name, or nullptr with the reason
icu_bridge::Calendar *ZoneCalendar(const Settings &settings, const std::string &zone, std::string &error) {
	struct Entry {
		std::unique_ptr<icu_bridge::Calendar> calendar;
		std::string error;
	};
	thread_local std::map<std::pair<std::string, std::string>, Entry> cache;
	auto key = std::make_pair(settings.calendar, zone);
	auto entry = cache.find(key);
	if (entry == cache.end()) {
		Entry created;
		auto resolved = zone;
		if (icu_bridge::ResolveTimeZone(resolved, &created.error)) {
			created.calendar = icu_bridge::Calendar::Create(resolved, settings.calendar);
		}
		entry = cache.emplace(std::move(key), std::move(created)).first;
	}
	error = entry->second.error;
	return entry->second.calendar.get();
}

//----------------------------------------------------------------------------------------------------------------------
// Options
//----------------------------------------------------------------------------------------------------------------------
void UpdateTimeZone(cxx::CustomOption::UpdateInput &input) {
	auto value = input.GetValue();
	if (value.IsNull()) {
		throw cxx::InvalidInputException("TimeZone setting cannot be NULL");
	}
	std::string time_zone(value.Get<cxx::varchar_t>().view());
	std::string error;
	if (!icu_bridge::ResolveTimeZone(time_zone, &error)) {
		throw cxx::Exception(DUCKDB_V2_ERROR_QUERY_NOT_IMPLEMENTED, error);
	}
	input.SetValue(cxx::Value::Create(input.GetContext().GetFactory(), cxx::varchar_t(time_zone)));
}

void UpdateCalendar(cxx::CustomOption::UpdateInput &input) {
	auto value = input.GetValue();
	if (value.IsNull()) {
		throw cxx::InvalidInputException("Calendar setting cannot be NULL");
	}
	std::string calendar(value.Get<cxx::varchar_t>().view());
	std::string error;
	if (!icu_bridge::ResolveCalendar(calendar, &error)) {
		throw cxx::Exception(DUCKDB_V2_ERROR_QUERY_NOT_IMPLEMENTED, error);
	}
	input.SetValue(cxx::Value::Create(input.GetContext().GetFactory(), cxx::varchar_t(calendar)));
}

void RegisterOptions(cxx::Extension &extension, cxx::Factory &factory) {
	const auto varchar = factory.ParseType("VARCHAR");

	const auto host = icu_bridge::HostTimeZone();
	auto time_zone = cxx::CustomOption::Create(factory);
	time_zone.SetName("TimeZone")
	    .SetType(varchar)
	    .SetDescription("The current time zone")
	    .SetDefaultValue(cxx::Value::Create(factory, cxx::varchar_t(host)))
	    .SetUpdateCallback(UpdateTimeZone);
	extension.Register(time_zone);

	auto calendar = cxx::CustomOption::Create(factory);
	calendar.SetName("Calendar")
	    .SetType(varchar)
	    .SetDescription("The current calendar")
	    .SetDefaultValue(cxx::Value::Create(factory, cxx::varchar_t(std::string("gregorian"))))
	    .SetUpdateCallback(UpdateCalendar);
	extension.Register(calendar);
}

//----------------------------------------------------------------------------------------------------------------------
// Table functions: icu_calendar_names, pg_timezone_names
//----------------------------------------------------------------------------------------------------------------------
struct NamesState {
	idx_t offset = 0;
	std::vector<std::string> calendars;
	std::vector<icu_bridge::ZoneRow> zones;
};

void CalendarNamesBind(cxx::TableFunction::BindInput &input) {
	input.AddResultColumn("name", input.GetContext().GetFactory().ParseType("VARCHAR"));
}

void CalendarNamesInit(cxx::TableFunction::InitGlobalInput &input) {
	NamesState state;
	state.calendars = icu_bridge::CalendarNames();
	input.SetGlobalState<NamesState>(std::move(state));
}

void CalendarNamesExec(cxx::TableFunction::ExecInput &input) {
	auto &state = input.GetGlobalState<NamesState>();
	auto chunk = input.GetOutputChunk();
	auto name = chunk.GetVector(0);
	idx_t count = 0;
	while (count < chunk.GetCapacity() && state.offset < state.calendars.size()) {
		name.AssignString(count++, state.calendars[state.offset++]);
	}
	name.SetSize(count);
}

void ZoneNamesBind(cxx::TableFunction::BindInput &input) {
	auto &factory = input.GetContext().GetFactory();
	input.AddResultColumn("name", factory.ParseType("VARCHAR"));
	input.AddResultColumn("abbrev", factory.ParseType("VARCHAR"));
	input.AddResultColumn("utc_offset", factory.ParseType("INTERVAL"));
	input.AddResultColumn("is_dst", factory.ParseType("BOOLEAN"));
}

void ZoneNamesInit(cxx::TableFunction::InitGlobalInput &input) {
	NamesState state;
	state.zones = icu_bridge::ListTimeZones();
	input.SetGlobalState<NamesState>(std::move(state));
}

void ZoneNamesExec(cxx::TableFunction::ExecInput &input) {
	auto &state = input.GetGlobalState<NamesState>();
	auto chunk = input.GetOutputChunk();
	auto name = chunk.GetVector(0);
	auto abbrev = chunk.GetVector(1);
	auto utc_offset = chunk.GetVector(2);
	auto is_dst = chunk.GetVector(3);
	auto *offsets = utc_offset.GetDataMutable<cxx::interval_t>();
	auto *dst = is_dst.GetDataMutable<bool>();
	idx_t count = 0;
	while (count < chunk.GetCapacity() && state.offset < state.zones.size()) {
		const auto &row = state.zones[state.offset++];
		name.AssignString(count, row.name);
		abbrev.AssignString(count, row.abbrev);
		offsets[count] = cxx::interval_t {0, 0, row.utc_offset_seconds * 1000000};
		dst[count] = row.is_dst;
		count++;
	}
	name.SetSize(count);
}

void RegisterTableFunctions(cxx::Extension &extension, cxx::Factory &factory) {
	auto calendars = cxx::TableFunction::Create(factory);
	calendars.SetName("icu_calendar_names")
	    .SetBindCallback(CalendarNamesBind)
	    .SetInitGlobalCallback(CalendarNamesInit)
	    .SetExecCallback(CalendarNamesExec);
	extension.Register(calendars);

	auto zones = cxx::TableFunction::Create(factory);
	zones.SetName("pg_timezone_names")
	    .SetBindCallback(ZoneNamesBind)
	    .SetInitGlobalCallback(ZoneNamesInit)
	    .SetExecCallback(ZoneNamesExec);
	extension.Register(zones);
}

//----------------------------------------------------------------------------------------------------------------------
// Collations
//----------------------------------------------------------------------------------------------------------------------
//! What a collation function is registered with
struct CollationUser {
	std::string name;
	std::string tag;
};

//! What a bound call carries. Two calls are equal when they collate the same way.
struct CollationBind {
	std::shared_ptr<icu_bridge::Collation> collation;
	std::string key;

	bool operator==(const CollationBind &other) const {
		return key == other.key;
	}
};

CollationBind MakeCollationBind(const std::string &name, const std::string &tag) {
	try {
		return CollationBind {icu_bridge::Collation::Create(name, tag), name + "/" + tag};
	} catch (std::runtime_error &error) {
		throw cxx::InvalidInputException(error.what());
	}
}

void CollateBind(cxx::ScalarFunction::BindInput &input) {
	const auto &user = input.GetUserData<CollationUser>();
	auto bind = MakeCollationBind(user.name, user.tag);
	input.SetBindData<CollationBind>(std::move(bind));
}

void SortKeyBind(cxx::ScalarFunction::BindInput &input) {
	auto name = input.GetConstantArgument(1);
	if (name.IsNull()) {
		throw cxx::InvalidInputException("icu_sort_key requires a collator");
	}
	const std::string collator(name.Get<cxx::varchar_t>().view());
	if (std::count(collator.begin(), collator.end(), '_') > 1) {
		throw cxx::InvalidInputException("Expected one or two splits");
	}
	auto bind = MakeCollationBind(collator, std::string());
	input.SetBindData<CollationBind>(std::move(bind));
}

void CollationInit(cxx::ScalarFunction::InitInput &input) {
	input.SetInitData<icu_bridge::Collation::Buffer>();
}

//! Writes the sort key of every string, as a blob or in hexadecimal
template <bool HEX>
void CollateExec(cxx::ScalarFunction::ExecInput &input) {
	const auto &bind = input.GetBindData<CollationBind>();
	auto &buffer = input.GetInitData<icu_bridge::Collation::Buffer>();
	const auto in = input.GetArg(0).GetView();
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	static const char DIGITS[] = "0123456789ABCDEF";
	std::string hex;
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!in.IsValid(i)) {
			result.SetNull(i);
			continue;
		}
		const auto value = in.Data<cxx::varchar_t>()[in.SelAt(i)];
		const char *key;
		size_t key_size;
		bind.collation->SortKey(value.data(), value.size(), buffer, key, key_size);
		if (!HEX) {
			result.AssignString(i, std::string_view(key, key_size));
			continue;
		}
		hex.resize(key_size * 2);
		for (size_t b = 0; b < key_size; b++) {
			const auto byte = static_cast<uint8_t>(key[b]);
			hex[b * 2] = DIGITS[byte / 16];
			hex[b * 2 + 1] = DIGITS[byte % 16];
		}
		result.AssignString(i, hex);
	}
}

void RegisterCollations(cxx::Extension &extension, cxx::Factory &factory) {
	const auto varchar = factory.ParseType("VARCHAR");
	const auto blob = factory.ParseType("BLOB");

	auto register_collation = [&](const std::string &sql_name, const std::string &name, const std::string &tag) {
		auto function = cxx::ScalarFunction::Create(factory);
		function.SetName("collate_" + name).SetBindCallback(CollateBind).SetInitCallback(CollationInit);
		function.SetExecCallback(CollateExec<false>);
		function.SetUserData<CollationUser>(CollationUser {name, tag});
		function.GetSignature().AddParameter("str", varchar).SetReturnType(blob);
		extension.RegisterCollation(sql_name, function);

		// Kept for queries and plans that call the function that returned the sort key in hexadecimal
		auto hex_function = cxx::ScalarFunction::Create(factory);
		hex_function.SetName("icu_collate_" + name).SetBindCallback(CollateBind).SetInitCallback(CollationInit);
		hex_function.SetExecCallback(CollateExec<true>);
		hex_function.SetUserData<CollationUser>(CollationUser {name, tag});
		hex_function.GetSignature().AddParameter("str", varchar).SetReturnType(varchar);
		extension.Register(hex_function);
	};

	for (const auto &name : icu_bridge::Collation::Names()) {
		register_collation(name, name, std::string());
	}

	// Inspired by the Postgres "ignore_accents" collation
	register_collation("icu_noaccent", "noaccent", "und-u-ks-level1-kc-true");

	auto sort_key = cxx::ScalarFunction::Create(factory);
	sort_key.SetName("icu_sort_key")
	    .SetBindCallback(SortKeyBind)
	    .SetInitCallback(CollationInit)
	    .SetExecCallback(CollateExec<true>);
	sort_key.GetSignature().AddParameter("str", varchar).AddParameter("collator", varchar).SetReturnType(varchar);
	extension.Register(sort_key);
}

//----------------------------------------------------------------------------------------------------------------------
// Casts between TIMESTAMP and TIMESTAMPTZ
//----------------------------------------------------------------------------------------------------------------------
void CastFromNaive(cxx::CastFunction::ExecInput &input) {
	auto &calendar = SettingsCalendar(ReadSettings(input.GetContext()));
	const auto in = input.GetInput().GetView();
	auto out = input.GetOutput();
	auto *data = out.GetDataMutable<cxx::timestamp_tz_t>();
	const auto try_mode = input.GetMode() == cxx::CastMode::TRY;
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!in.IsValid(i)) {
			out.SetNull(i);
			continue;
		}
		const auto naive = in.Data<cxx::timestamp_t>()[in.SelAt(i)].micros;
		if (!IsFinite(naive)) {
			data[i].micros = naive;
			continue;
		}
		int64_t instant;
		if (!calendar.FromLocal(naive, instant)) {
			if (!try_mode) {
				ThrowConversion("ICU date overflows timestamp range");
			}
			out.SetNull(i);
			continue;
		}
		data[i].micros = instant;
	}
}

void CastToNaive(cxx::CastFunction::ExecInput &input) {
	auto &context = input.GetContext();
	auto disabled = context.GetConfig().TryGetOption("disable_timestamptz_casts");
	if (disabled && !disabled->value.IsNull() && disabled->value.Get<bool>()) {
		throw cxx::Exception(DUCKDB_V2_ERROR_INPUT_INVALID,
		                     "Casting from TIMESTAMP WITH TIME ZONE to TIMESTAMP without an explicit time zone has "
		                     "been disabled  - use \"AT TIME ZONE ...\"");
	}
	auto &calendar = SettingsCalendar(ReadSettings(context));
	const auto in = input.GetInput().GetView();
	auto out = input.GetOutput();
	auto *data = out.GetDataMutable<cxx::timestamp_t>();
	const auto try_mode = input.GetMode() == cxx::CastMode::TRY;
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!in.IsValid(i)) {
			out.SetNull(i);
			continue;
		}
		const auto instant = in.Data<cxx::timestamp_tz_t>()[in.SelAt(i)].micros;
		if (!IsFinite(instant)) {
			data[i].micros = instant;
			continue;
		}
		int64_t local;
		if (!calendar.ToLocal(instant, local)) {
			if (!try_mode) {
				ThrowConversion("ICU date overflows timestamp range");
			}
			out.SetNull(i);
			continue;
		}
		data[i].micros = local;
	}
}

//! TIMESTAMPTZ -> VARCHAR renders the instant in the session's time zone
void CastToVarchar(cxx::CastFunction::ExecInput &input) {
	auto &calendar = SettingsCalendar(ReadSettings(input.GetContext()));
	const auto in = input.GetInput().GetView();
	auto out = input.GetOutput();
	const auto try_mode = input.GetMode() == cxx::CastMode::TRY;
	std::string text;
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!in.IsValid(i)) {
			out.SetNull(i);
			continue;
		}
		const auto instant = in.Data<cxx::timestamp_tz_t>()[in.SelAt(i)].micros;
		if (instant == std::numeric_limits<int64_t>::max()) {
			out.AssignString(i, "infinity");
		} else if (instant == -std::numeric_limits<int64_t>::max()) {
			out.AssignString(i, "-infinity");
		} else if (calendar.Format(instant, text)) {
			out.AssignString(i, text);
		} else if (try_mode) {
			out.SetNull(i);
		} else {
			ThrowConversion("Unable to render the timestamp in the current time zone");
		}
	}
}

void RegisterCasts(cxx::Extension &extension, cxx::Factory &factory) {
	const auto timestamp = factory.ParseType("TIMESTAMP");
	const auto timestamp_tz = factory.ParseType("TIMESTAMP WITH TIME ZONE");

	// The implicit cost is the one the engine gives TIMESTAMP -> TIMESTAMPTZ
	auto from_naive = cxx::CastFunction::Create(factory);
	from_naive.SetSourceType(timestamp)
	    .SetTargetType(timestamp_tz)
	    .SetImplicitCastCost(123)
	    .SetExecCallback(CastFromNaive);
	extension.Register(from_naive);

	auto to_naive = cxx::CastFunction::Create(factory);
	to_naive.SetSourceType(timestamp_tz).SetTargetType(timestamp).SetExecCallback(CastToNaive);
	extension.Register(to_naive);

	auto to_varchar = cxx::CastFunction::Create(factory);
	to_varchar.SetSourceType(timestamp_tz).SetTargetType(factory.ParseType("VARCHAR")).SetExecCallback(CastToVarchar);
	extension.Register(to_varchar);
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar functions
//----------------------------------------------------------------------------------------------------------------------
//! year(timestamptz) and friends, extracted in the time zone and calendar of the settings
template <icu_bridge::Part PART>
void PartExec(cxx::ScalarFunction::ExecInput &input) {
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
void RegisterPart(cxx::Extension &extension, cxx::Factory &factory, const char *name) {
	auto function = cxx::ScalarFunction::Create(factory);
	function.SetName(name)
	    .SetExecCallback(PartExec<PART>)
	    .SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
	function.GetSignature()
	    .AddParameter("ts", factory.ParseType("TIMESTAMP WITH TIME ZONE"))
	    .SetReturnType(factory.ParseType("BIGINT"));
	extension.Register(function);
}

//! timezone(zone, timestamptz) -> timestamp: the wall clock of the instant in the zone
void ZoneToNaiveExec(cxx::ScalarFunction::ExecInput &input) {
	const auto settings = ReadSettings(input.GetContext());
	const auto zone = input.GetArg(0).GetView();
	const auto in = input.GetArg(1).GetView();
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	auto *out = result.GetDataMutable<cxx::timestamp_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!zone.IsValid(i) || !in.IsValid(i)) {
			result.SetNull(i);
			continue;
		}
		std::string error;
		auto *calendar = ZoneCalendar(settings, std::string(zone.Data<cxx::varchar_t>()[zone.SelAt(i)].view()), error);
		if (!calendar) {
			throw cxx::Exception(DUCKDB_V2_ERROR_QUERY_NOT_IMPLEMENTED, error);
		}
		const auto instant = in.Data<cxx::timestamp_tz_t>()[in.SelAt(i)].micros;
		if (!IsFinite(instant)) {
			out[i].micros = instant;
			continue;
		}
		if (!calendar->ToLocal(instant, out[i].micros)) {
			ThrowConversion("ICU date overflows timestamp range");
		}
	}
}

//! timezone(zone, timestamp) -> timestamptz: the instant the wall clock shows in the zone
void ZoneFromNaiveExec(cxx::ScalarFunction::ExecInput &input) {
	const auto settings = ReadSettings(input.GetContext());
	const auto zone = input.GetArg(0).GetView();
	const auto in = input.GetArg(1).GetView();
	auto result = input.GetResult();
	result.SetSize(input.GetRowCount());
	auto *out = result.GetDataMutable<cxx::timestamp_tz_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!zone.IsValid(i) || !in.IsValid(i)) {
			result.SetNull(i);
			continue;
		}
		std::string error;
		auto *calendar = ZoneCalendar(settings, std::string(zone.Data<cxx::varchar_t>()[zone.SelAt(i)].view()), error);
		if (!calendar) {
			throw cxx::Exception(DUCKDB_V2_ERROR_QUERY_NOT_IMPLEMENTED, error);
		}
		const auto naive = in.Data<cxx::timestamp_t>()[in.SelAt(i)].micros;
		if (!IsFinite(naive)) {
			out[i].micros = naive;
			continue;
		}
		if (!calendar->FromLocal(naive, out[i].micros)) {
			ThrowConversion("ICU date overflows timestamp range");
		}
	}
}

//! The date and the time of day (with the zone's offset) of the transaction start in the session's time zone
void CurrentDateExec(cxx::ScalarFunction::ExecInput &input) {
	auto &context = input.GetContext();
	auto &calendar = SettingsCalendar(ReadSettings(context));
	int64_t local;
	if (!calendar.ToLocal(context.GetTransactionStartTimestamp().micros, local)) {
		ThrowConversion("ICU date overflows timestamp range");
	}
	auto days = local / MICROS_PER_DAY;
	if (local % MICROS_PER_DAY < 0) {
		days--;
	}
	auto result = input.GetResult();
	auto *out = result.GetDataMutable<cxx::date_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		out[i].days = static_cast<int32_t>(days);
	}
}

void CurrentTimeExec(cxx::ScalarFunction::ExecInput &input) {
	auto &context = input.GetContext();
	auto &calendar = SettingsCalendar(ReadSettings(context));
	const auto start = context.GetTransactionStartTimestamp().micros;
	int64_t local;
	int64_t offset;
	if (!calendar.ToLocal(start, local) || !calendar.GetPart(start, icu_bridge::Part::ZONE_OFFSET, offset)) {
		ThrowConversion("ICU date overflows timestamp range");
	}
	auto time_of_day = local % MICROS_PER_DAY;
	if (time_of_day < 0) {
		time_of_day += MICROS_PER_DAY;
	}
	if (offset < -cxx::dtime_tz_t::MAX_OFFSET || offset > cxx::dtime_tz_t::MAX_OFFSET) {
		throw cxx::Exception(DUCKDB_V2_ERROR_INPUT_OUT_OF_RANGE, "Time zone offset is out of range");
	}
	auto result = input.GetResult();
	auto *out = result.GetDataMutable<cxx::dtime_tz_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		out[i] = cxx::dtime_tz_t(time_of_day, static_cast<int32_t>(offset));
	}
}

void RegisterScalars(cxx::Extension &extension, cxx::Factory &factory) {
	const auto varchar = factory.ParseType("VARCHAR");
	const auto timestamp = factory.ParseType("TIMESTAMP");
	const auto timestamp_tz = factory.ParseType("TIMESTAMP WITH TIME ZONE");

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

	auto to_naive = cxx::ScalarFunction::Create(factory);
	to_naive.SetName("timezone").SetExecCallback(ZoneToNaiveExec);
	to_naive.GetSignature().AddParameter("zone", varchar).AddParameter("ts", timestamp_tz).SetReturnType(timestamp);
	extension.Register(to_naive);

	auto from_naive = cxx::ScalarFunction::Create(factory);
	from_naive.SetName("timezone").SetExecCallback(ZoneFromNaiveExec);
	from_naive.GetSignature().AddParameter("zone", varchar).AddParameter("ts", timestamp).SetReturnType(timestamp_tz);
	extension.Register(from_naive);

	for (const char *name : {"current_date", "today"}) {
		auto current_date = cxx::ScalarFunction::Create(factory);
		current_date.SetName(name)
		    .SetExecCallback(CurrentDateExec)
		    .SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
		current_date.GetSignature().SetReturnType(factory.ParseType("DATE"));
		extension.Register(current_date);
	}

	auto current_time = cxx::ScalarFunction::Create(factory);
	current_time.SetName("get_current_time")
	    .SetExecCallback(CurrentTimeExec)
	    .SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
	current_time.GetSignature().SetReturnType(factory.ParseType("TIME WITH TIME ZONE"));
	extension.Register(current_time);
}

} // namespace

DUCKDB_CPP_EXTENSION_ENTRYPOINT(cxx::Extension &extension, cxx::Context &context) {
	auto &factory = context.GetFactory();
	RegisterOptions(extension, factory);
	RegisterTableFunctions(extension, factory);
	RegisterCollations(extension, factory);
	RegisterCasts(extension, factory);
	RegisterScalars(extension, factory);
}
