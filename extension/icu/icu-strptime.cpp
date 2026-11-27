#include "icu-casts.hpp"
#include "icu-datefunc.hpp"
#include "icu-vector.hpp"

namespace duckdb_icu {

//===--------------------------------------------------------------------===//
// strptime, try_strptime
//===--------------------------------------------------------------------===//
struct ICUStrptime : public ICUDateFunc {
	struct StrptimeInfo {
		bool is_try;
	};

	struct ICUStrptimeBindData : public BindData {
		ICUStrptimeBindData(cxx::Context &context, vector<string> format_strings_p, bool is_try_p)
		    : BindData(context), format_strings(std::move(format_strings_p)), is_try(is_try_p) {
			for (auto &format_string : format_strings) {
				formats.push_back(cxx::StrpTimeFormat::Create(format_string));
				has_utc_offset.push_back(formats.back().HasUtcOffset());
			}
		}

		vector<string> format_strings;
		vector<cxx::StrpTimeFormat> formats;
		vector<bool> has_utc_offset;
		bool is_try;

		bool operator==(const ICUStrptimeBindData &other) const {
			return format_strings == other.format_strings && is_try == other.is_try;
		}
	};

	//! The per-thread state of a parse: the resolved time zones, and what the formats parse into
	struct ICUStrptimeState {
		explicit ICUStrptimeState(Calendar &calendar) : cache(calendar) {
		}

		CalendarCacheState cache;
		cxx::StrpTimeResult parsed;
	};

	//! The formats of a call, and whether they need time zones: only those the engine's strptime cannot parse do
	struct FormatAnalysis {
		vector<string> format_strings;
		bool has_tz = false;
		bool has_ns = false;
	};

	static bool AnalyzeFormats(cxx::FunctionBindInput &input, FormatAnalysis &result) {
		auto format_value = input.TryGetConstantArgument(1);
		if (!format_value || format_value->IsNull()) {
			// the engine's strptime reports the formats it cannot take
			return false;
		}
		const auto type_id = format_value->GetLogicalType().GetTypeId();
		if (type_id == cxx::LogicalTypeId::VARCHAR) {
			const string format_string(format_value->Get<cxx::varchar_t>().view());
			auto format = cxx::StrpTimeFormat::Create(format_string);
			// If we have a time zone, we should use ICU for parsing and return a TSTZ instead.
			result.has_tz = format.HasTimeZoneName();
			result.has_ns = format.HasNanoseconds();
			result.format_strings.push_back(format_string);
			return result.has_tz;
		}
		if (type_id != cxx::LogicalTypeId::LIST) {
			return false;
		}
		const auto count = format_value->GetChildCount();
		if (count == 0) {
			throw InvalidInputException("strptime format list must not be empty");
		}
		for (idx_t i = 0; i < count; i++) {
			auto child = format_value->GetChild(i);
			const string format_string = child.IsNull() ? string() : string(child.Get<cxx::varchar_t>().view());
			auto format = cxx::StrpTimeFormat::Create(format_string);
			// If any format has UTC offsets or names, then we have to produce TSTZ
			result.has_tz = result.has_tz || format.HasTimeZoneName() || format.HasUtcOffset();
			result.has_ns = result.has_ns || format.HasNanoseconds();
			result.format_strings.push_back(format_string);
		}
		return result.has_tz;
	}

	static void ResolveTypes(cxx::ScalarFunction::ResolveTypesInput &input) {
		auto &factory = input.GetContext().GetFactory();
		FormatAnalysis analysis;
		if (AnalyzeFormats(input, analysis)) {
			input.SetReturnType(factory.ParseType(analysis.has_ns ? "TIMESTAMPTZ_NS" : "TIMESTAMPTZ"));
		} else {
			// the bind hands the call to the engine's strptime, which decides the type
			input.SetReturnType(factory.ParseType("TIMESTAMP"));
		}
	}

	static void StrpTimeBindFunction(cxx::ScalarFunction::BindInput &input) {
		FormatAnalysis analysis;
		if (!AnalyzeFormats(input, analysis)) {
			// Fall back to the engine's faster, non-TZ parsing
			input.CallOverridden();
			return;
		}
		input.SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
		input.SetBindData<ICUStrptimeBindData>(input.GetContext(), std::move(analysis.format_strings),
		                                       input.GetUserData<StrptimeInfo>().is_try);
	}

	static void InitParse(cxx::ScalarFunction::InitInput &input) {
		auto &info = input.GetBindData<ICUStrptimeBindData>();
		input.SetInitData<ICUStrptimeState>(*info.calendar);
	}

	static uint64_t ToMicros(Calendar *calendar, const cxx::TimeParts &parts, bool has_utc_offset) {
		// Get the parts in the current time zone
		uint64_t micros = uint64_t((parts.fraction + Interval::NANOS_PER_MICRO / 2) / Interval::NANOS_PER_MICRO);
		calendar->Set(CAL_EXTENDED_YEAR, parts.year); // strptime doesn't understand eras
		calendar->Set(CAL_MONTH, parts.month - 1);
		calendar->Set(CAL_DATE, parts.day);
		calendar->Set(CAL_HOUR_OF_DAY, parts.hour);
		calendar->Set(CAL_MINUTE, parts.minute);
		calendar->Set(CAL_SECOND, parts.second);
		calendar->Set(CAL_MILLISECOND, int32_t(micros / Interval::MICROS_PER_MSEC));
		micros %= Interval::MICROS_PER_MSEC;

		// This overrides the TZ setting, so only use it if an offset was parsed.
		// Note that we don't bother/worry about the DST setting because the two just combine.
		if (has_utc_offset) {
			calendar->Set(CAL_ZONE_OFFSET, int32_t(parts.utc_offset * Interval::MSECS_PER_SEC));
		}

		return micros;
	}

	static uint64_t ToNanos(Calendar *calendar, const cxx::TimeParts &parts, bool has_utc_offset) {
		// Get the parts in the current time zone
		uint64_t nanos = uint64_t(parts.fraction);
		calendar->Set(CAL_EXTENDED_YEAR, parts.year); // strptime doesn't understand eras
		calendar->Set(CAL_MONTH, parts.month - 1);
		calendar->Set(CAL_DATE, parts.day);
		calendar->Set(CAL_HOUR_OF_DAY, parts.hour);
		calendar->Set(CAL_MINUTE, parts.minute);
		calendar->Set(CAL_SECOND, parts.second);
		calendar->Set(CAL_MILLISECOND, int32_t(nanos / Interval::NANOS_PER_MSEC));
		nanos %= Interval::NANOS_PER_MSEC;

		// This overrides the TZ setting, so only use it if an offset was parsed.
		if (has_utc_offset) {
			calendar->Set(CAL_ZONE_OFFSET, int32_t(parts.utc_offset * Interval::MSECS_PER_SEC));
		}

		return nanos;
	}

	//! The timestamp of a special date such as infinity, as the engine's strptime gives it
	static bool SpecialTimestamp(date_t special, timestamp_tz_t &result) {
		if (special.days == DATE_INFINITY) {
			result.micros = TIMESTAMP_INFINITY;
		} else if (special.days == DATE_NINFINITY) {
			result.micros = TIMESTAMP_NINFINITY;
		} else {
			timestamp_t micros;
			if (!Timestamp::TryFromDatetime(special, dtime_t {}, micros)) {
				return false;
			}
			result.micros = micros.micros;
		}
		return true;
	}

	static bool SpecialTimestamp(date_t special, timestamp_tz_ns_t &result) {
		if (special.days == DATE_INFINITY) {
			result.nanos = TIMESTAMP_INFINITY;
		} else if (special.days == DATE_NINFINITY) {
			result.nanos = TIMESTAMP_NINFINITY;
		} else {
			result.nanos = int64_t(special.days) * Interval::NANOS_PER_DAY;
		}
		return true;
	}

	static bool GetParsedTime(Calendar *calendar, const cxx::TimeParts &parts, bool has_utc_offset,
	                          timestamp_tz_t &result) {
		return TryGetTime(calendar, ToMicros(calendar, parts, has_utc_offset), result);
	}

	static bool GetParsedTime(Calendar *calendar, const cxx::TimeParts &parts, bool has_utc_offset,
	                          timestamp_tz_ns_t &result) {
		return TryGetTimeNS(calendar, ToNanos(calendar, parts, has_utc_offset), result);
	}

	//! Parses `input` with the first format that matches it. Fails only when none does, or the time zone it names does
	//! not exist: an out of range timestamp throws, unless `is_try`.
	template <typename T>
	static bool ParseOne(Calendar *calendar, ICUStrptimeState &state, std::string_view input,
	                     const ICUStrptimeBindData &info, T &result) {
		for (idx_t i = 0; i < info.formats.size(); i++) {
			if (!info.formats[i].Parse(input, state.parsed)) {
				continue;
			}
			if (state.parsed.IsSpecial()) {
				if (SpecialTimestamp(state.parsed.GetSpecialDate(), result)) {
					return true;
				}
				continue;
			}
			// Use the calendar of the parsed time zone, if any - the shared calendar keeps the session zone
			auto tz_name = state.parsed.GetTimeZoneName();
			Calendar *row_calendar = calendar;
			if (!tz_name.empty()) {
				row_calendar = info.is_try ? state.cache.TryGetCalendar(tz_name) : state.cache.GetCalendar(tz_name);
				if (!row_calendar) {
					continue;
				}
			}
			if (GetParsedTime(row_calendar, state.parsed.GetParts(), info.has_utc_offset[i], result)) {
				return true;
			}
			if (!info.is_try) {
				throw ConversionException(std::is_same<T, timestamp_tz_ns_t>::value
				                              ? "ICU date overflows timestamp_ns range"
				                              : "ICU date overflows timestamp range");
			}
		}
		return false;
	}

	template <typename T>
	static void Parse(cxx::ScalarFunction::ExecInput &input) {
		auto &info = input.GetBindData<ICUStrptimeBindData>();
		auto &state = input.GetInitData<ICUStrptimeState>();
		CalendarPtr calendar_ptr(info.calendar->Copy());
		auto calendar = calendar_ptr.get();

		// the formats were compiled when the call was bound: only the text is read
		const auto count = input.GetRowCount();
		StringArgReader texts(input.GetArg(0));
		auto result = input.GetResult();
		ResultWriter<T> writer(result, count);
		for (idx_t row = 0; row < count; row++) {
			if (!texts.IsValid(row)) {
				writer.SetNull(row);
				continue;
			}
			const auto text = texts.Get(row);
			T parsed;
			if (ParseOne(calendar, state, text, info, parsed)) {
				writer.Set(row, parsed);
				continue;
			}
			if (info.is_try) {
				writer.SetNull(row);
				continue;
			}
			throw InvalidInputException(state.parsed.GetError(text, info.format_strings[0]));
		}
	}

	static void ParseTZ(cxx::ScalarFunction::ExecInput &input) {
		if (input.GetResult().GetLogicalType().GetTypeId() == cxx::LogicalTypeId::TIMESTAMP_TZ_NS) {
			Parse<timestamp_tz_ns_t>(input);
		} else {
			Parse<timestamp_tz_t>(input);
		}
	}

	static void AddFunctions(const ICURegistration &reg, const string &name, bool is_try) {
		for (auto format_type : {"VARCHAR", "VARCHAR[]"}) {
			auto function = reg.Scalar(
			    name, {{"text", "VARCHAR"}, {format_type == string("VARCHAR") ? "format" : "format-list", format_type}},
			    "ANY");
			function.SetResolveTypesCallback(ResolveTypes)
			    .SetBindCallback(StrpTimeBindFunction)
			    .SetInitCallback(InitParse)
			    .SetExecCallback(ParseTZ);
			function.SetUserData<StrptimeInfo>(StrptimeInfo {is_try});
			// like the engine's strptime, a NULL format makes a NULL rather than skipping the call
			function.SetNullHandling(cxx::FunctionNullHandling::SPECIAL);
			reg.extension.Register(function);
		}
	}

	//===--------------------------------------------------------------------===//
	// Casts from VARCHAR
	//===--------------------------------------------------------------------===//
	static string TimestampFormatError(std::string_view text) {
		return "invalid timestamp field format: \"" + string(text) +
		       "\", expected format is (YYYY-MM-DD HH:MM[:SS[.US]][±HH[:MM[:SS]]| ZONE])";
	}

	static string TimestampRangeError(std::string_view text) {
		return "timestamp field value out of range: \"" + string(text) + "\"";
	}

	static bool VarcharToTimestampTZUS(Calendar *cal, CalendarCacheState &cache, std::string_view input,
	                                   timestamp_tz_t &result, string &error, int32_t *nanos = nullptr) {
		cxx::TimestampText parsed;
		auto success = cxx::ParseTimestampTzText(input, parsed, nanos != nullptr);
		if (success != cxx::TimestampTextResult::SUCCESS) {
			error = success == cxx::TimestampTextResult::ERROR_RANGE ? TimestampRangeError(input)
			                                                         : TimestampFormatError(input);
			return false;
		}
		if (nanos) {
			*nanos = parsed.nanos;
		}
		if (parsed.has_offset) {
			result.micros = parsed.micros;
			return true;
		}
		// Convert parts to a TZ (default or parsed) if no offset was provided
		auto calendar = cal;

		// Use the calendar of the time zone if one was provided - the shared calendar keeps the session zone
		if (!parsed.tz_name.empty()) {
			calendar = cache.TryGetCalendar(parsed.tz_name, &error);
			if (!calendar) {
				return false;
			}
		}

		// Now get the parts in the given time zone
		return TryFromNaive(calendar, timestamp_t {parsed.micros}, result, error);
	}

	static void VarcharToTimestampTZ(cxx::CastFunction::ExecInput &input) {
		auto &info = input.GetBindData<BindData>();
		auto &cache = input.GetInitData<CalendarCacheState>();
		CalendarPtr cal(info.calendar->Copy());
		ExecuteCast<std::string_view, timestamp_tz_t>(
		    input, [&](std::string_view text, timestamp_tz_t &result, string &error) {
			    return VarcharToTimestampTZUS(cal.get(), cache, text, result, error);
		    });
	}

	static void VarcharToTimestampTZNS(cxx::CastFunction::ExecInput &input) {
		auto &info = input.GetBindData<BindData>();
		auto &cache = input.GetInitData<CalendarCacheState>();
		CalendarPtr cal(info.calendar->Copy());
		ExecuteCast<std::string_view, timestamp_tz_ns_t>(
		    input, [&](std::string_view text, timestamp_tz_ns_t &result, string &error) {
			    int32_t nanos = 0;
			    timestamp_tz_t ts_us;
			    if (!VarcharToTimestampTZUS(cal.get(), cache, text, ts_us, error, &nanos)) {
				    return false;
			    }
			    timestamp_ns_t ns;
			    if (!Timestamp::TryFromTimestampNanos(timestamp_t {ts_us.micros}, nanos, ns)) {
				    error = TimestampRangeError(text);
				    return false;
			    }
			    result.nanos = ns.nanos;
			    return true;
		    });
	}

	static void VarcharToTimeTZ(cxx::CastFunction::ExecInput &input) {
		auto &info = input.GetBindData<BindData>();
		CalendarPtr cal(info.calendar->Copy());
		ExecuteCast<std::string_view, dtime_tz_t>(input, [&](std::string_view text, dtime_tz_t &result, string &error) {
			int64_t micros;
			int32_t offset;
			bool has_offset;
			if (!cxx::ParseTimeTzText(text, micros, offset, has_offset)) {
				error = "time field value out of range: \"" + string(text) +
				        "\", expected format is ([YYYY-MM-DD ]HH:MM:SS[.MS])";
				return false;
			}
			if (!has_offset) {
				// Extract the offset from the calendar, and apply it to the offset +00 time we parsed.
				if (!TryGetTimeTZOffset(cal.get(), offset)) {
					error = TimeTZOffsetError(offset);
					return false;
				}
			}
			result = dtime_tz_t(micros, offset);
			return true;
		});
	}

	static void BindCastFromVarchar(cxx::CastFunction::BindInput &input) {
		ICUCasts::BindCastData(input, "VARCHAR to TIME/TIMESTAMPTZ");
	}

	static void AddCasts(const ICURegistration &reg) {
		ICUCasts::Register(reg, "VARCHAR", "TIMESTAMPTZ", false, BindCastFromVarchar, VarcharToTimestampTZ,
		                   ICUCasts::InitCalendarCache);
		ICUCasts::Register(reg, "VARCHAR", "TIMESTAMPTZ_NS", false, BindCastFromVarchar, VarcharToTimestampTZNS,
		                   ICUCasts::InitCalendarCache);
		ICUCasts::Register(reg, "VARCHAR", "TIMETZ", false, BindCastFromVarchar, VarcharToTimeTZ);
	}
};

//===--------------------------------------------------------------------===//
// strftime
//===--------------------------------------------------------------------===//
struct ICUStrftime : public ICUDateFunc {
	//! Fills the parts of a timestamp in the calendar; the fraction is in microseconds, or nanoseconds for _NS
	static cxx::TimeParts GetParts(Calendar *calendar, timestamp_tz_t input) {
		uint64_t micros = SetTime(calendar, input);
		cxx::TimeParts parts;
		parts.year = ExtractField(calendar, CAL_EXTENDED_YEAR); // strftime doesn't understand eras.
		parts.month = ExtractField(calendar, CAL_MONTH) + 1;
		parts.day = ExtractField(calendar, CAL_DATE);
		parts.hour = ExtractField(calendar, CAL_HOUR_OF_DAY);
		parts.minute = ExtractField(calendar, CAL_MINUTE);
		parts.second = ExtractField(calendar, CAL_SECOND);
		parts.fraction = int32_t(ExtractField(calendar, CAL_MILLISECOND) * Interval::MICROS_PER_MSEC + int64_t(micros));
		parts.utc_offset = (ExtractField(calendar, CAL_ZONE_OFFSET) + ExtractField(calendar, CAL_DST_OFFSET)) /
		                   int32_t(Interval::MSECS_PER_SEC);
		return parts;
	}

	static cxx::TimeParts GetParts(Calendar *calendar, timestamp_tz_ns_t input) {
		uint64_t nanos = SetTimeNS(calendar, input);
		cxx::TimeParts parts;
		parts.year = ExtractField(calendar, CAL_EXTENDED_YEAR); // strftime doesn't understand eras.
		parts.month = ExtractField(calendar, CAL_MONTH) + 1;
		parts.day = ExtractField(calendar, CAL_DATE);
		parts.hour = ExtractField(calendar, CAL_HOUR_OF_DAY);
		parts.minute = ExtractField(calendar, CAL_MINUTE);
		parts.second = ExtractField(calendar, CAL_SECOND);
		parts.fraction = int32_t(ExtractField(calendar, CAL_MILLISECOND) * Interval::NANOS_PER_MSEC + int64_t(nanos));
		parts.utc_offset = (ExtractField(calendar, CAL_ZONE_OFFSET) + ExtractField(calendar, CAL_DST_OFFSET)) /
		                   int32_t(Interval::MSECS_PER_SEC);
		return parts;
	}

	template <typename T>
	static void ICUStrftimeFunction(cxx::ScalarFunction::ExecInput &input) {
		auto &info = input.GetBindData<BindData>();
		CalendarPtr calendar(info.calendar->Copy());
		const auto &tz_name = info.tz_setting;
		constexpr bool NANOS = std::is_same<T, timestamp_tz_ns_t>::value;

		auto format_arg = input.GetArg(1);
		std::optional<cxx::StrfTimeFormat> constant_format;
		if (IsConstant(format_arg)) {
			// Common case of constant part.
			StringArgReader format(format_arg);
			if (format.IsValid(0)) {
				constant_format.emplace(cxx::StrfTimeFormat::Create(format.Get(0)));
			}
		}
		ExecuteStringRows<T, std::string_view>(input, [&](string &out, T value, std::string_view format_specifier) {
			if (!IsFinite(value)) {
				out = ToInfinity(value);
				return true;
			}
			std::optional<cxx::StrfTimeFormat> row_format;
			if (!constant_format) {
				row_format.emplace(cxx::StrfTimeFormat::Create(format_specifier));
			}
			auto &format = constant_format ? *constant_format : *row_format;
			const auto parts = GetParts(calendar.get(), value);
			// the date must exist, as the engine's strftime requires
			Date::FromDate(parts.year, parts.month, parts.day);
			format.Write(parts, NANOS, tz_name, out);
			return true;
		});
	}

	static void AddFunctions(const ICURegistration &reg, const string &name) {
		auto add = [&](const string &type, cxx::ScalarFunction::ExecCallback exec) {
			auto function = reg.Scalar(name, {{"data", type}, {"format", "VARCHAR"}}, "VARCHAR");
			function.SetBindCallback(Bind).SetExecCallback(exec);
			// throws for unsupported format specifiers
			function.SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
			reg.extension.Register(function);
		};
		add("TIMESTAMPTZ", ICUStrftimeFunction<timestamp_tz_t>);
		add("TIMESTAMPTZ_NS", ICUStrftimeFunction<timestamp_tz_ns_t>);
	}

	//===--------------------------------------------------------------------===//
	// Casts to VARCHAR
	//===--------------------------------------------------------------------===//
	template <typename T>
	static void CastOperation(Calendar *calendar, T input, string &out) {
		// Infinity is always formatted the same way
		if (!IsFinite(input)) {
			out = ToInfinity(input);
			return;
		}

		// decompose the timestamp
		auto ts_data = ICUHelpers::GetComponents(input, calendar);

		idx_t year_length;
		bool add_bc;
		const auto date_len = DateToStringCast::YearLength(ts_data.year, year_length, add_bc);

		char micro_buffer[9];
		const auto time_len = TimeToStringCast::MicrosLength(ts_data.microsecond, micro_buffer, ts_data.nanosecond);

		auto offset = ExtractField(calendar, CAL_ZONE_OFFSET) + ExtractField(calendar, CAL_DST_OFFSET);
		offset /= int32_t(Interval::MSECS_PER_SEC);
		int hour_offset = offset / Interval::SECS_PER_HOUR;
		int minute_offset = (offset % Interval::SECS_PER_HOUR) / Interval::SECS_PER_MINUTE;
		int second_offset = offset % Interval::SECS_PER_MINUTE;
		auto offset_str = Time::ToUTCOffset(hour_offset, minute_offset, second_offset);
		const auto offset_len = offset_str.size();

		const auto len = date_len + 1 + time_len + offset_len;
		out.resize(len);
		auto buffer = &out[0];

		DateToStringCast::Format(buffer, ts_data.year, ts_data.month, ts_data.day, year_length, add_bc);
		buffer += date_len;
		*buffer++ = ' ';

		TimeToStringCast::Format(buffer, time_len, ts_data.hour, ts_data.minute, ts_data.second, micro_buffer);
		buffer += time_len;

		memcpy(buffer, offset_str.c_str(), offset_len);
	}

	template <typename T>
	static void CastToVarchar(cxx::CastFunction::ExecInput &input) {
		auto &info = input.GetBindData<BindData>();
		CalendarPtr calendar(info.calendar->Copy());

		CastRows<StringResultWriter> rows(input);
		ArgReader<T> values(input.GetInput());
		string text;
		for (idx_t row = 0; row < rows.Count(); row++) {
			if (!values.IsValid(row)) {
				rows.SetNull(row);
				continue;
			}
			CastOperation<T>(calendar.get(), values.Get(row), text);
			rows.Writer().Set(row, text);
		}
		rows.Finish();
	}

	static void BindCastToVarchar(cxx::CastFunction::BindInput &input) {
		ICUCasts::BindCastData(input, "TIMESTAMPTZ to VARCHAR");
	}

	static void AddCasts(const ICURegistration &reg) {
		ICUCasts::Register(reg, "TIMESTAMPTZ", "VARCHAR", false, BindCastToVarchar, CastToVarchar<timestamp_tz_t>);
		ICUCasts::Register(reg, "TIMESTAMPTZ_NS", "VARCHAR", false, BindCastToVarchar,
		                   CastToVarchar<timestamp_tz_ns_t>);
	}
};

void RegisterICUStrptimeFunctions(const ICURegistration &reg) {
	ICUStrptime::AddFunctions(reg, "strptime", false);
	ICUStrptime::AddFunctions(reg, "try_strptime", true);

	ICUStrftime::AddFunctions(reg, "strftime");

	// Add string casts
	ICUStrptime::AddCasts(reg);
	ICUStrftime::AddCasts(reg);
}

} // namespace duckdb_icu
