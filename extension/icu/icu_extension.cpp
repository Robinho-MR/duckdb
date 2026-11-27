// The ICU extension, written against the stable C++ API (duckdb_cpp.hpp) only: the collations, time zones and
// calendars of DuckDB's date and time functions.

#ifndef DUCKDB_EXTENSION_NAME
#define DUCKDB_EXTENSION_NAME icu
#endif

#include "duckdb_cpp_extension.hpp"

#include "icu-datefunc.hpp"
#include "icu-vector.hpp"
#include "collation_collator.hpp"
#include "calendar.hpp"
#include "timezone.hpp"

namespace duckdb_icu {

//===--------------------------------------------------------------------===//
// Collations
//===--------------------------------------------------------------------===//
//! What a collation function collates with: a language and a country such as "de" and "DE", or a BCP 47 language tag
//! such as "und-u-ks-level1-kc-true"
struct CollationInfo {
	string language;
	string country;
	string tag;
};

struct IcuBindData {
	collation::Collator collator;
	string language;
	string country;
	string tag;

	IcuBindData(string language_p, string country_p)
	    : collator(country_p.empty() ? language_p : language_p + "_" + country_p), language(std::move(language_p)),
	      country(std::move(country_p)) {
	}

	//! A BCP 47 language tag such as "und-u-ks-level1-kc-true"
	explicit IcuBindData(string tag_p) : collator(collation::Collator::FromLanguageTag(tag_p)), tag(std::move(tag_p)) {
	}

	bool operator==(const IcuBindData &other) const {
		return language == other.language && country == other.country && tag == other.tag;
	}

	//! The prefix of the collation functions that are kept for backwards compatibility, which return the sort key in
	//! hexadecimal instead of as a blob. The collation functions themselves are named after the collation.
	static constexpr const char *HEX_FUNCTION_PREFIX = "icu_collate_";
};

//! Keeps the buffers the collator works in alive across the chunks of a scan, so that
//! generating sort keys does not allocate
struct CollatorLocalState {
	collation::CollationBuffer buffer;
};

static void CollatorInit(cxx::ScalarFunction::InitInput &input) {
	input.SetInitData<CollatorLocalState>();
}

//! Writes the sort key of every string, either as a blob or in hexadecimal. The hexadecimal
//! form is only used by the functions that are kept for backwards compatibility.
template <bool HEX>
static void ICUCollateFunction(cxx::ScalarFunction::ExecInput &input) {
	auto &info = input.GetBindData<IcuBindData>();
	// the collator is immutable, it is shared between the threads that run the function
	auto &collator = info.collator;
	auto &buffer = input.GetInitData<CollatorLocalState>().buffer;

	static const char DIGITS[] = "0123456789ABCDEF";
	// icu_sort_key also passes the collator, which the bind already read: only the string is collated
	const auto count = input.GetRowCount();
	StringArgReader texts(input.GetArg(0));
	auto result = input.GetResult();
	StringResultWriter writer(result, count);
	string out;
	for (idx_t row = 0; row < count; row++) {
		if (!texts.IsValid(row)) {
			writer.SetNull(row);
			continue;
		}
		const auto text = texts.Get(row);
		// create a sort key from the string, the trailing null byte is not part of the result
		collator.GetSortKey(text.data(), text.size(), buffer);
		auto &key = buffer.key;
		auto key_size = key.size() - 1;
		if (!HEX) {
			out.assign(reinterpret_cast<const char *>(key.data()), key_size);
		} else {
			out.resize(key_size * 2);
			for (idx_t i = 0; i < key_size; i++) {
				D_ASSERT(key[i] != 0);
				out[i * 2] = DIGITS[uint8_t(key[i]) / 16];
				out[i * 2 + 1] = DIGITS[uint8_t(key[i]) % 16];
			}
		}
		writer.Set(row, out);
	}
}

static void ICUCollateBind(cxx::ScalarFunction::BindInput &input) {
	auto &info = input.GetUserData<CollationInfo>();
	//! Return a tagged collator
	if (!info.tag.empty()) {
		input.SetBindData<IcuBindData>(info.tag);
		return;
	}
	input.SetBindData<IcuBindData>(info.language, info.country);
}

static void ICUSortKeyBind(cxx::ScalarFunction::BindInput &input) {
	auto val = input.GetConstantArgument(1);
	if (val.IsNull()) {
		throw BinderException("The collator argument in function icu_sort_key must not be NULL");
	}
	// the collator is a language and a country such as "de_DE", or a language tag such as "en-u-ka-shifted"
	const string collator(val.Get<cxx::varchar_t>().view());
	if (StringUtil::Split(collator, "_").size() > 2) {
		throw InvalidInputException("Expected one or two splits");
	}
	input.SetBindData<IcuBindData>(collator);
}

//! The collation and its function, from the language and country of its name or from a tag
static CollationInfo GetCollationInfo(const string &collation, const string &tag) {
	CollationInfo info;
	info.tag = tag;
	if (tag.empty()) {
		auto splits = StringUtil::Split(collation, "_");
		if (splits.size() == 1) {
			info.language = splits[0];
		} else if (splits.size() == 2) {
			info.language = splits[0];
			info.country = splits[1];
		} else {
			throw InvalidInputException("Expected one or two splits");
		}
	}
	return info;
}

//! Registers the collation function a collation pushes into a query, which writes the sort key as a blob, in the
//! collation schema of the system catalog
static void RegisterCollation(const ICURegistration &reg, const string &collation, const CollationInfo &info) {
	auto function = reg.Scalar(collation, {{"str", "VARCHAR"}}, "BLOB");
	function.SetSchema("collations");
	function.SetBindCallback(ICUCollateBind).SetInitCallback(CollatorInit).SetExecCallback(ICUCollateFunction<false>);
	function.SetUserData<CollationInfo>(info);
	reg.extension.Register(function);
}

//! Registers the function collations used before they wrote blobs, which is kept registered so that
//! queries and plans that call it directly keep working
static void RegisterHexCollation(const ICURegistration &reg, const string &collation, const CollationInfo &info) {
	auto function = reg.Scalar(IcuBindData::HEX_FUNCTION_PREFIX + collation, {{"str", "VARCHAR"}}, "VARCHAR");
	function.SetBindCallback(ICUCollateBind).SetInitCallback(CollatorInit).SetExecCallback(ICUCollateFunction<true>);
	function.SetUserData<CollationInfo>(info);
	reg.extension.Register(function);
}

//===--------------------------------------------------------------------===//
// Options
//===--------------------------------------------------------------------===//
static void SetICUTimeZone(cxx::CustomOption::UpdateInput &input) {
	auto value = input.GetValue();
	if (value.IsNull()) {
		throw InvalidInputException("TimeZone setting cannot be NULL");
	}
	string tz_str(value.Get<cxx::varchar_t>().view());
	ICUHelpers::GetTimeZone(tz_str);
	input.SetValue(cxx::Value::Create(input.GetContext().GetFactory(), cxx::varchar_t(tz_str)));
}

static void SetICUCalendar(cxx::CustomOption::UpdateInput &input) {
	auto value = input.GetValue();
	if (value.IsNull()) {
		throw InvalidInputException("Calendar setting cannot be NULL");
	}
	const string name(value.Get<cxx::varchar_t>().view());
	//	Try to be friendlier: look for a case insensitive match, and if we don't find one,
	//	make a suggestion
	for (const auto &candidate : Calendar::GetAvailableTypes()) {
		if (StringUtil::CIEquals(candidate, name)) {
			input.SetValue(cxx::Value::Create(input.GetContext().GetFactory(), cxx::varchar_t(candidate)));
			return;
		}
	}

	string candidate_str = StringUtil::CandidatesMessage(
	    StringUtil::TopNJaroWinkler(Calendar::GetAvailableTypes(), name), "Candidate calendars");

	throw NotImplementedException("Unknown Calendar '%s'!\n%s", name, candidate_str);
}

static void RegisterOption(const ICURegistration &reg, cxx::Context &context, const string &name,
                           const string &description, const string &default_value,
                           cxx::CustomOption::UpdateCallback callback) {
	// a database that is reopened keeps the options of its config, and with them those of an earlier load
	if (context.GetConfig().TryGetOption(name)) {
		return;
	}
	auto option = cxx::CustomOption::Create(reg.factory);
	option.SetName(name)
	    .SetDescription(description)
	    .SetType(reg.Type("VARCHAR"))
	    .SetDefaultValue(cxx::Value::Create(reg.factory, cxx::varchar_t(default_value)))
	    .SetUpdateCallback(callback);
	reg.extension.Register(option);
}

//===--------------------------------------------------------------------===//
// icu_calendar_names
//===--------------------------------------------------------------------===//
struct ICUCalendarData {
	idx_t offset = 0;
};

static void ICUCalendarBind(cxx::TableFunction::BindInput &input) {
	input.AddResultColumn("name", input.GetContext().GetFactory().ParseType("VARCHAR"));
}

static void ICUCalendarInit(cxx::TableFunction::InitGlobalInput &input) {
	input.SetGlobalState<ICUCalendarData>();
}

static void ICUCalendarFunction(cxx::TableFunction::ExecInput &input) {
	auto &data = input.GetGlobalState<ICUCalendarData>();
	const auto &types = Calendar::GetAvailableTypes();
	auto output = input.GetOutputChunk();
	auto name_col = output.GetVector(0);
	const auto capacity = output.GetCapacity();

	idx_t index = 0;
	while (index < capacity && data.offset < types.size()) {
		name_col.AssignStringUnsafe(index, types[data.offset++]);
		++index;
	}
	name_col.SetSize(index);
}

static void LoadInternal(cxx::Extension &extension, cxx::Context &context) {
	ICURegistration reg {extension, context.GetFactory()};

	// iterate over all the collations
	for (auto &collation : collation::Collator::GetCollations()) {
		auto info = GetCollationInfo(collation, "");
		RegisterCollation(reg, collation, info);
		RegisterHexCollation(reg, collation, info);
	}

	/**
	 * This collation function is inpired on the Postgres "ignore_accents":
	 * See: https://www.postgresql.org/docs/current/collation.html
	 * CREATE COLLATION ignore_accents (provider = icu, locale = 'und-u-ks-level1-kc-true', deterministic = false);
	 *
	 * Also, according with the source file: postgres/src/backend/utils/adt/pg_locale.c.
	 * "und-u-kc-ks-level1" is converted to the equivalent ICU format locale ID,
	 * e.g. "und@colcaselevel=yes;colstrength=primary"
	 *
	 */
	RegisterCollation(reg, "icu_noaccent", GetCollationInfo("icu_noaccent", "und-u-ks-level1-kc-true"));
	RegisterHexCollation(reg, "noaccent", GetCollationInfo("noaccent", "und-u-ks-level1-kc-true"));

	auto sort_key = reg.Scalar("icu_sort_key", {{"str", "VARCHAR"}, {"collator", "VARCHAR"}}, "VARCHAR");
	sort_key.SetBindCallback(ICUSortKeyBind).SetInitCallback(CollatorInit).SetExecCallback(ICUCollateFunction<true>);
	extension.Register(sort_key);

	// Time Zones
	auto tz = TimeZone::TryCreateDefault();
	// If the host time zone is unknown, fall back to UTC
	RegisterOption(reg, context, "TimeZone", "The current time zone", tz ? tz->GetId() : "UTC", SetICUTimeZone);

	RegisterICUCurrentFunctions(reg);
	RegisterICUDateAddFunctions(reg);
	RegisterICUDatePartFunctions(reg);
	RegisterICUDateSubFunctions(reg);
	RegisterICUDateTruncFunctions(reg);
	RegisterICUMakeDateFunctions(reg);
	RegisterICUTableRangeFunctions(reg);
	RegisterICUListRangeFunctions(reg);
	RegisterICUStrptimeFunctions(reg);
	RegisterICUTimeBucketFunctions(reg);
	RegisterICUTimeZoneFunctions(reg);

	// Calendars
	RegisterOption(reg, context, "Calendar", "The current calendar", "gregorian", SetICUCalendar);

	auto cal_names = cxx::TableFunction::Create(reg.factory);
	cal_names.SetStability(cxx::FunctionStability::CONSISTENT);
	cal_names.SetName("icu_calendar_names")
	    .SetBindCallback(ICUCalendarBind)
	    .SetInitGlobalCallback(ICUCalendarInit)
	    .SetExecCallback(ICUCalendarFunction);
	extension.Register(cal_names);
}

} // namespace duckdb_icu

DUCKDB_CPP_EXTENSION_ENTRYPOINT(duckdb::cxx::Extension &extension, duckdb::cxx::Context &context) {
	duckdb_icu::LoadInternal(extension, context);
}
