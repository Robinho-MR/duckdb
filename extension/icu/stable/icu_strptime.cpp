#include "icu_stable_areas.hpp"
#include "icu_stable_common.hpp"

#include "icu_bridge_strptime.hpp"

#include <memory>
#include <string>
#include <vector>

namespace icu_stable {

namespace {

[[noreturn]] void Throw(const icu_bridge::Failure &failure) {
	switch (failure.kind) {
	case icu_bridge::ErrorKind::CONVERSION:
		ThrowConversion(failure.message);
	case icu_bridge::ErrorKind::OUT_OF_RANGE:
		throw cxx::Exception(DUCKDB_V2_ERROR_INPUT_OUT_OF_RANGE, failure.message);
	case icu_bridge::ErrorKind::NOT_IMPLEMENTED:
		throw cxx::Exception(DUCKDB_V2_ERROR_QUERY_NOT_IMPLEMENTED, failure.message);
	default:
		throw cxx::InvalidInputException(failure.message);
	}
}

icu_bridge::ZoneLookup MakeZoneLookup(const Settings &settings) {
	return [settings](const std::string &zone, std::string &error) {
		return ZoneCalendar(settings, zone, error);
	};
}

//----------------------------------------------------------------------------------------------------------------------
// strptime, try_strptime
//----------------------------------------------------------------------------------------------------------------------
struct StrptimeBind {
	std::vector<std::string> format_strings;
	//! Null when the format is NULL
	std::shared_ptr<icu_bridge::StrptimeFormats> formats;
	bool is_try = false;

	bool operator==(const StrptimeBind &other) const {
		return is_try == other.is_try && format_strings == other.format_strings;
	}
};

struct StrptimeUser {
	bool is_try;
	bool is_list;
};

//! Compiles the constant format argument (a format, or a list of formats)
StrptimeBind CompileFormats(cxx::FunctionBindInput &input, bool is_try, bool is_list) {
	StrptimeBind bind;
	bind.is_try = is_try;
	auto constant = input.TryGetConstantArgument(1);
	if (!constant) {
		throw cxx::InvalidInputException("strptime format must be a constant");
	}
	if (constant->IsNull()) {
		return bind;
	}
	if (is_list) {
		if (constant->GetChildCount() == 0) {
			throw cxx::InvalidInputException("strptime format list must not be empty");
		}
		for (idx_t i = 0; i < constant->GetChildCount(); i++) {
			auto child = constant->GetChild(i);
			if (child.IsNull()) {
				throw cxx::InvalidInputException("strptime format list must not contain NULL");
			}
			bind.format_strings.emplace_back(child.Get<cxx::varchar_t>().view());
		}
	} else {
		bind.format_strings.emplace_back(constant->Get<cxx::varchar_t>().view());
	}
	icu_bridge::Failure failure;
	bind.formats = icu_bridge::StrptimeFormats::Create(bind.format_strings, is_list, failure);
	if (!bind.formats) {
		Throw(failure);
	}
	return bind;
}

void StrptimeResolveTypes(cxx::ScalarFunction::ResolveTypesInput &input) {
	const auto &user = input.GetUserData<StrptimeUser>();
	auto &factory = input.GetContext().GetFactory();
	auto bind = CompileFormats(input, user.is_try, user.is_list);
	auto kind = icu_bridge::TimestampKind::TIMESTAMP;
	if (bind.formats) {
		kind = bind.formats->Kind();
	}
	switch (kind) {
	case icu_bridge::TimestampKind::TIMESTAMP:
		input.SetReturnType(factory.ParseType("TIMESTAMP"));
		break;
	case icu_bridge::TimestampKind::TIMESTAMP_NS:
		input.SetReturnType(factory.ParseType("TIMESTAMP_NS"));
		break;
	case icu_bridge::TimestampKind::TIMESTAMP_TZ:
		input.SetReturnType(factory.ParseType("TIMESTAMP WITH TIME ZONE"));
		break;
	case icu_bridge::TimestampKind::TIMESTAMP_TZ_NS:
		input.SetReturnType(factory.ParseType("TIMESTAMPTZ_NS"));
		break;
	}
}

void StrptimeBindCallback(cxx::ScalarFunction::BindInput &input) {
	const auto &user = input.GetUserData<StrptimeUser>();
	input.SetBindData<StrptimeBind>(CompileFormats(input, user.is_try, user.is_list));
}

void StrptimeExec(cxx::ScalarFunction::ExecInput &input) {
	const auto &bind = input.GetBindData<StrptimeBind>();
	const auto count = input.GetRowCount();
	auto result = input.GetResult();
	result.SetSize(count);
	if (!bind.formats) {
		for (idx_t i = 0; i < count; i++) {
			result.SetNull(i);
		}
		return;
	}
	const auto settings = ReadSettings(input.GetContext());
	auto &calendar = SettingsCalendar(settings);
	const auto zones = MakeZoneLookup(settings);
	const auto in = input.GetArg(0).GetView();
	auto *out = result.GetDataMutable<int64_t>();
	for (idx_t i = 0; i < count; i++) {
		if (!in.IsValid(i)) {
			result.SetNull(i);
			continue;
		}
		const auto text = in.Data<cxx::varchar_t>()[in.SelAt(i)].view();
		icu_bridge::Failure failure;
		if (bind.formats->Parse(text, calendar, zones, bind.is_try, out[i], failure)) {
			continue;
		}
		if (bind.is_try) {
			result.SetNull(i);
			continue;
		}
		Throw(failure);
	}
}

void RegisterStrptime(cxx::Extension &extension, cxx::Factory &factory) {
	const auto varchar = factory.ParseType("VARCHAR");
	const auto varchar_list = factory.ParseType("VARCHAR[]");
	const auto any = factory.CreateType(cxx::LogicalTypeId::ANY);

	for (const bool is_try : {false, true}) {
		for (const bool is_list : {false, true}) {
			auto function = cxx::ScalarFunction::Create(factory);
			function.SetName(is_try ? "try_strptime" : "strptime")
			    .SetResolveTypesCallback(StrptimeResolveTypes)
			    .SetBindCallback(StrptimeBindCallback)
			    .SetExecCallback(StrptimeExec)
			    .SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
			function.SetUserData<StrptimeUser>(StrptimeUser {is_try, is_list});
			function.GetSignature()
			    .AddParameter("text", varchar)
			    .AddParameter(is_list ? "format-list" : "format", is_list ? varchar_list : varchar)
			    .SetReturnType(any);
			extension.Register(function);
		}
	}
}

//----------------------------------------------------------------------------------------------------------------------
// strftime
//----------------------------------------------------------------------------------------------------------------------
template <class T, bool IS_NANOS>
void StrftimeExec(cxx::ScalarFunction::ExecInput &input) {
	const auto count = input.GetRowCount();
	auto result = input.GetResult();
	result.SetSize(count);
	const auto settings = ReadSettings(input.GetContext());
	auto &calendar = SettingsCalendar(settings);
	const auto in = input.GetArg(0).GetView();
	const auto format = input.GetArg(1).GetView();
	icu_bridge::Strftime strftime;
	std::string text;
	for (idx_t i = 0; i < count; i++) {
		if (!in.IsValid(i) || !format.IsValid(i)) {
			result.SetNull(i);
			continue;
		}
		const int64_t instant = IS_NANOS ? in.Data<cxx::timestamp_tz_ns_t>()[in.SelAt(i)].nanos
		                                 : in.Data<cxx::timestamp_tz_t>()[in.SelAt(i)].micros;
		if (!IsFinite(instant)) {
			result.AssignString(i, instant > 0 ? "infinity" : "-infinity");
			continue;
		}
		icu_bridge::Failure failure;
		if (!strftime.SetFormat(format.Data<cxx::varchar_t>()[format.SelAt(i)].view(), failure) ||
		    !strftime.Format(calendar, instant, IS_NANOS, settings.time_zone, text, failure)) {
			Throw(failure);
		}
		result.AssignString(i, text);
	}
}

void RegisterStrftime(cxx::Extension &extension, cxx::Factory &factory) {
	const auto varchar = factory.ParseType("VARCHAR");

	auto tstz = cxx::ScalarFunction::Create(factory);
	tstz.SetName("strftime")
	    .SetExecCallback(StrftimeExec<cxx::timestamp_tz_t, false>)
	    .SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
	tstz.GetSignature()
	    .AddParameter("data", factory.ParseType("TIMESTAMP WITH TIME ZONE"))
	    .AddParameter("format", varchar)
	    .SetReturnType(varchar);
	extension.Register(tstz);

	auto tstz_ns = cxx::ScalarFunction::Create(factory);
	tstz_ns.SetName("strftime")
	    .SetExecCallback(StrftimeExec<cxx::timestamp_tz_ns_t, true>)
	    .SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
	tstz_ns.GetSignature()
	    .AddParameter("data", factory.ParseType("TIMESTAMPTZ_NS"))
	    .AddParameter("format", varchar)
	    .SetReturnType(varchar);
	extension.Register(tstz_ns);
}

//----------------------------------------------------------------------------------------------------------------------
// Casts
//----------------------------------------------------------------------------------------------------------------------
//! TIMESTAMPTZ -> VARCHAR renders the instant in the session's time zone
template <class T, bool IS_NANOS>
void TimestampTZToVarcharExec(cxx::CastFunction::ExecInput &input) {
	auto &calendar = SettingsCalendar(ReadSettings(input.GetContext()));
	const auto in = input.GetInput().GetView();
	auto out = input.GetOutput();
	out.SetSize(input.GetRowCount());
	const auto try_mode = input.GetMode() == cxx::CastMode::TRY;
	bool failed = false;
	icu_bridge::Failure first_failure;
	std::string text;
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!in.IsValid(i)) {
			out.SetNull(i);
			continue;
		}
		const int64_t instant = IS_NANOS ? in.Data<cxx::timestamp_tz_ns_t>()[in.SelAt(i)].nanos
		                                 : in.Data<cxx::timestamp_tz_t>()[in.SelAt(i)].micros;
		if (!IsFinite(instant)) {
			out.AssignString(i, instant > 0 ? "infinity" : "-infinity");
			continue;
		}
		icu_bridge::Failure failure;
		if (icu_bridge::TimestampTZToString(calendar, instant, IS_NANOS, text, failure)) {
			out.AssignString(i, text);
		} else {
			if (!try_mode) {
				Throw(failure);
			}
			out.SetNull(i);
			if (!failed) {
				failed = true;
				first_failure = failure;
			}
		}
	}
	if (failed) {
		Throw(first_failure);
	}
}

template <bool IS_NANOS>
void VarcharToTimestampTZExec(cxx::CastFunction::ExecInput &input) {
	const auto settings = ReadSettings(input.GetContext());
	auto &calendar = SettingsCalendar(settings);
	const auto zones = MakeZoneLookup(settings);
	const auto in = input.GetInput().GetView();
	auto out = input.GetOutput();
	out.SetSize(input.GetRowCount());
	auto *data = out.GetDataMutable<int64_t>();
	const auto try_mode = input.GetMode() == cxx::CastMode::TRY;
	bool failed = false;
	icu_bridge::Failure first_failure;
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!in.IsValid(i)) {
			out.SetNull(i);
			continue;
		}
		const auto text = in.Data<cxx::varchar_t>()[in.SelAt(i)].view();
		icu_bridge::Failure failure;
		if (icu_bridge::VarcharToTimestampTZ(calendar, zones, text, IS_NANOS, data[i], failure)) {
			continue;
		}
		if (!try_mode) {
			Throw(failure);
		}
		out.SetNull(i);
		if (!failed) {
			failed = true;
			first_failure = failure;
		}
	}
	if (failed) {
		Throw(first_failure);
	}
}

void VarcharToTimeTZExec(cxx::CastFunction::ExecInput &input) {
	auto &calendar = SettingsCalendar(ReadSettings(input.GetContext()));
	const auto in = input.GetInput().GetView();
	auto out = input.GetOutput();
	out.SetSize(input.GetRowCount());
	auto *data = out.GetDataMutable<cxx::dtime_tz_t>();
	const auto try_mode = input.GetMode() == cxx::CastMode::TRY;
	bool failed = false;
	icu_bridge::Failure first_failure;
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		if (!in.IsValid(i)) {
			out.SetNull(i);
			continue;
		}
		const auto text = in.Data<cxx::varchar_t>()[in.SelAt(i)].view();
		int64_t micros;
		int32_t offset;
		icu_bridge::Failure failure;
		if (icu_bridge::VarcharToTimeTZ(calendar, text, micros, offset, failure)) {
			data[i] = cxx::dtime_tz_t(micros, offset);
			continue;
		}
		if (!try_mode) {
			Throw(failure);
		}
		out.SetNull(i);
		if (!failed) {
			failed = true;
			first_failure = failure;
		}
	}
	if (failed) {
		Throw(first_failure);
	}
}

void RegisterStringCasts(cxx::Extension &extension, cxx::Factory &factory) {
	const auto varchar = factory.ParseType("VARCHAR");
	const auto timestamp_tz = factory.ParseType("TIMESTAMP WITH TIME ZONE");
	const auto timestamp_tz_ns = factory.ParseType("TIMESTAMPTZ_NS");

	auto register_cast = [&](const cxx::LogicalType &source, const cxx::LogicalType &target,
	                         cxx::CastFunction::ExecCallback callback) {
		auto cast = cxx::CastFunction::Create(factory);
		cast.SetSourceType(source).SetTargetType(target).SetExecCallback(callback);
		extension.Register(cast);
	};
	register_cast(timestamp_tz, varchar, TimestampTZToVarcharExec<cxx::timestamp_tz_t, false>);
	register_cast(timestamp_tz_ns, varchar, TimestampTZToVarcharExec<cxx::timestamp_tz_ns_t, true>);
	register_cast(varchar, timestamp_tz, VarcharToTimestampTZExec<false>);
	register_cast(varchar, timestamp_tz_ns, VarcharToTimestampTZExec<true>);
	register_cast(varchar, factory.ParseType("TIME WITH TIME ZONE"), VarcharToTimeTZExec);
}

} // namespace

void RegisterStrptimeFunctions(cxx::Extension &extension, cxx::Factory &factory) {
	RegisterStrptime(extension, factory);
	RegisterStrftime(extension, factory);
	RegisterStringCasts(extension, factory);
}

} // namespace icu_stable
