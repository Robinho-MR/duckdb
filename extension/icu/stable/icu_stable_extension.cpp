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

#include "icu_stable_areas.hpp"
#include "icu_stable_common.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace cxx = duckdb::cxx;
using namespace icu_stable;

namespace {

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

void RegisterOptions(cxx::Extension &extension, cxx::Context &context) {
	auto &factory = context.GetFactory();
	const auto varchar = factory.ParseType("VARCHAR");

	const auto host = icu_bridge::HostTimeZone();
	// A configuration that is reused for a new database (restart) already holds the options
	if (!context.GetConfig().TryGetOption("TimeZone")) {
		auto time_zone = cxx::CustomOption::Create(factory);
		time_zone.SetName("TimeZone")
		    .SetType(varchar)
		    .SetDescription("The current time zone")
		    .SetDefaultValue(cxx::Value::Create(factory, cxx::varchar_t(host)))
		    .SetUpdateCallback(UpdateTimeZone);
		extension.Register(time_zone);
	}

	if (!context.GetConfig().TryGetOption("Calendar")) {
		auto calendar = cxx::CustomOption::Create(factory);
		calendar.SetName("Calendar")
		    .SetType(varchar)
		    .SetDescription("The current calendar")
		    .SetDefaultValue(cxx::Value::Create(factory, cxx::varchar_t(std::string("gregorian"))))
		    .SetUpdateCallback(UpdateCalendar);
		extension.Register(calendar);
	}
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

//! The plan keeps the name and the tag of the collation ("name/tag")
void CollateSerialize(cxx::ScalarFunction::SerializeInput &input) {
	input.Write(input.GetBindData<CollationBind>().key);
}

void CollateDeserialize(cxx::ScalarFunction::DeserializeInput &input) {
	const std::string key(input.GetData());
	const auto separator = key.find('/');
	if (separator == std::string::npos) {
		throw cxx::InvalidInputException("Invalid serialized collation");
	}
	input.SetBindData<CollationBind>(MakeCollationBind(key.substr(0, separator), key.substr(separator + 1)));
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
		try {
			bind.collation->SortKey(value.data(), value.size(), buffer, key, key_size);
		} catch (icu_bridge::DataCorruptionError &error) {
			throw cxx::Exception(DUCKDB_V2_ERROR_DATABASE_DATA_CORRUPTION, error.what());
		}
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
		function.SetExecCallback(CollateExec<false>).SetSerializeCallbacks(CollateSerialize, CollateDeserialize);
		function.SetUserData<CollationUser>(CollationUser {name, tag});
		function.GetSignature().AddParameter("str", varchar).SetReturnType(blob);
		extension.Register(function);

		auto collation = cxx::CollationInfo::Create(factory);
		collation.SetName(sql_name).SetFunctionName("collate_" + name);
		extension.Register(collation);

		// Kept for queries and plans that call the function that returned the sort key in hexadecimal
		auto hex_function = cxx::ScalarFunction::Create(factory);
		hex_function.SetName("icu_collate_" + name).SetBindCallback(CollateBind).SetInitCallback(CollationInit);
		hex_function.SetExecCallback(CollateExec<true>).SetSerializeCallbacks(CollateSerialize, CollateDeserialize);
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
	    .SetExecCallback(CollateExec<true>)
	    .SetSerializeCallbacks(CollateSerialize, CollateDeserialize);
	sort_key.GetSignature().AddParameter("str", varchar).AddParameter("collator", varchar).SetReturnType(varchar);
	extension.Register(sort_key);
}

//----------------------------------------------------------------------------------------------------------------------
// Cast from TIMESTAMPTZ to VARCHAR
//----------------------------------------------------------------------------------------------------------------------
void RegisterCasts(cxx::Extension &extension, cxx::Factory &factory) {
	const auto timestamp_tz = factory.ParseType("TIMESTAMP WITH TIME ZONE");
}

//----------------------------------------------------------------------------------------------------------------------
// Time zone conversion and current date and time
//----------------------------------------------------------------------------------------------------------------------
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
	const auto checked_offset = CheckedTimeTZOffset(offset);
	auto result = input.GetResult();
	auto *out = result.GetDataMutable<cxx::dtime_tz_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		out[i] = cxx::dtime_tz_t(time_of_day, checked_offset);
	}
}

void RegisterScalars(cxx::Extension &extension, cxx::Factory &factory) {
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
	RegisterOptions(extension, context);
	RegisterTableFunctions(extension, factory);
	RegisterCollations(extension, factory);
	RegisterCasts(extension, factory);
	RegisterScalars(extension, factory);
	RegisterDatePartFunctions(extension, factory);
	RegisterArithmeticFunctions(extension, factory);
	RegisterTruncBucketFunctions(extension, factory);
	RegisterRangeFunctions(extension, factory);
	RegisterStrptimeFunctions(extension, factory);
	RegisterTimeZoneFunctions(extension, factory);
}
