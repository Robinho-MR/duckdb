#include "icu-datefunc.hpp"
#include "icu-vector.hpp"

namespace duckdb_icu {

struct ICUDatePart : public ICUDateFunc {
	typedef int64_t (*part_bigint_t)(Calendar *calendar, const uint64_t micros);
	typedef double (*part_double_t)(Calendar *calendar, const uint64_t micros);

	// Date part adapters
	static int64_t ExtractEra(Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, CAL_ERA);
	}

	static int64_t ExtractYear(Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, CAL_YEAR);
	}

	static int64_t ExtractDecade(Calendar *calendar, const uint64_t micros) {
		return ExtractYear(calendar, micros) / 10;
	}

	static int64_t ExtractCentury(Calendar *calendar, const uint64_t micros) {
		const auto era = ExtractEra(calendar, micros);
		const auto cccc = ((ExtractYear(calendar, micros) - 1) / 100) + 1;
		return era > 0 ? cccc : -cccc;
	}

	static int64_t ExtractMillenium(Calendar *calendar, const uint64_t micros) {
		const auto era = ExtractEra(calendar, micros);
		const auto mmmm = ((ExtractYear(calendar, micros) - 1) / 1000) + 1;
		return era > 0 ? mmmm : -mmmm;
	}

	static int64_t ExtractMonth(Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, CAL_MONTH) + 1;
	}

	static int64_t ExtractQuarter(Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, CAL_MONTH) / Interval::MONTHS_PER_QUARTER + 1;
	}

	static int64_t ExtractDay(Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, CAL_DATE);
	}

	static int64_t ExtractDayOfWeek(Calendar *calendar, const uint64_t micros) {
		// [Sun(0), Sat(6)]
		return ExtractField(calendar, CAL_DAY_OF_WEEK) - CAL_SUNDAY;
	}

	static int64_t ExtractISODayOfWeek(Calendar *calendar, const uint64_t micros) {
		// [Mon(1), Sun(7)]
		return 1 + (ExtractField(calendar, CAL_DAY_OF_WEEK) + 7 - CAL_MONDAY) % 7;
	}

	static int64_t ExtractWeek(Calendar *calendar, const uint64_t micros) {
		calendar->SetFirstDayOfWeek(CAL_MONDAY);
		calendar->SetMinimalDaysInFirstWeek(4);
		return ExtractField(calendar, CAL_WEEK_OF_YEAR);
	}

	static int64_t ExtractISOYear(Calendar *calendar, const uint64_t micros) {
		calendar->SetFirstDayOfWeek(CAL_MONDAY);
		calendar->SetMinimalDaysInFirstWeek(4);
		return ExtractField(calendar, CAL_YEAR_WOY);
	}

	static int64_t ExtractYearWeek(Calendar *calendar, const uint64_t micros) {
		calendar->SetFirstDayOfWeek(CAL_MONDAY);
		calendar->SetMinimalDaysInFirstWeek(4);
		const auto iyyy = ExtractField(calendar, CAL_YEAR_WOY);
		const auto ww = ExtractField(calendar, CAL_WEEK_OF_YEAR);
		return iyyy * 100 + ((iyyy > 0) ? ww : -ww);
	}

	static int64_t ExtractDayOfYear(Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, CAL_DAY_OF_YEAR);
	}

	static int64_t ExtractHour(Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, CAL_HOUR_OF_DAY);
	}

	static int64_t ExtractMinute(Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, CAL_MINUTE);
	}

	static int64_t ExtractSecond(Calendar *calendar, const uint64_t micros) {
		return ExtractField(calendar, CAL_SECOND);
	}

	static int64_t ExtractMillisecond(Calendar *calendar, const uint64_t micros) {
		return ExtractSecond(calendar, micros) * Interval::MSECS_PER_SEC + ExtractField(calendar, CAL_MILLISECOND);
	}

	static int64_t ExtractMicrosecond(Calendar *calendar, const uint64_t micros) {
		return ExtractMillisecond(calendar, micros) * Interval::MICROS_PER_MSEC + int64_t(micros);
	}

	static double ExtractEpoch(Calendar *calendar, const uint64_t micros) {
		// the milliseconds carry the fraction of a second, so the division has to keep it
		auto result = double(calendar->GetTime()) / double(Interval::MSECS_PER_SEC);
		result += static_cast<double>(micros) / double(Interval::MICROS_PER_SEC);
		return result;
	}

	static int64_t ExtractTimezone(Calendar *calendar, const uint64_t micros) {
		auto millis = ExtractField(calendar, CAL_ZONE_OFFSET);
		millis += ExtractField(calendar, CAL_DST_OFFSET);
		return millis / Interval::MSECS_PER_SEC;
	}

	static int64_t ExtractTimezoneHour(Calendar *calendar, const uint64_t micros) {
		auto secs = ExtractTimezone(calendar, micros);
		return secs / Interval::SECS_PER_HOUR;
	}

	static int64_t ExtractTimezoneMinute(Calendar *calendar, const uint64_t micros) {
		auto secs = ExtractTimezone(calendar, micros);
		return (secs % Interval::SECS_PER_HOUR) / Interval::SECS_PER_MINUTE;
	}

	//	PG uses doubles for JDs so we can only use them with other double types
	static double ExtractJulianDay(Calendar *calendar, const uint64_t micros) {
		//	We need days + fraction
		auto days = ExtractField(calendar, CAL_JULIAN_DAY);
		auto frac = ExtractHour(calendar, micros);

		frac *= Interval::MINS_PER_HOUR;
		frac += ExtractMinute(calendar, micros);

		frac *= Interval::MICROS_PER_MINUTE;
		frac += ExtractMicrosecond(calendar, micros);

		double result = static_cast<double>(frac);
		result /= Interval::MICROS_PER_DAY;
		result += days;

		return result;
	}

	static part_bigint_t PartCodeBigintFactory(DatePartSpecifier part) {
		switch (part) {
		case DatePartSpecifier::YEAR:
			return ExtractYear;
		case DatePartSpecifier::MONTH:
			return ExtractMonth;
		case DatePartSpecifier::DAY:
			return ExtractDay;
		case DatePartSpecifier::DECADE:
			return ExtractDecade;
		case DatePartSpecifier::CENTURY:
			return ExtractCentury;
		case DatePartSpecifier::MILLENNIUM:
			return ExtractMillenium;
		case DatePartSpecifier::MICROSECONDS:
			return ExtractMicrosecond;
		case DatePartSpecifier::MILLISECONDS:
			return ExtractMillisecond;
		case DatePartSpecifier::SECOND:
			return ExtractSecond;
		case DatePartSpecifier::MINUTE:
			return ExtractMinute;
		case DatePartSpecifier::HOUR:
			return ExtractHour;
		case DatePartSpecifier::DOW:
			return ExtractDayOfWeek;
		case DatePartSpecifier::ISODOW:
			return ExtractISODayOfWeek;
		case DatePartSpecifier::WEEK:
			return ExtractWeek;
		case DatePartSpecifier::ISOYEAR:
			return ExtractISOYear;
		case DatePartSpecifier::DOY:
			return ExtractDayOfYear;
		case DatePartSpecifier::QUARTER:
			return ExtractQuarter;
		case DatePartSpecifier::YEARWEEK:
			return ExtractYearWeek;
		case DatePartSpecifier::ERA:
			return ExtractEra;
		case DatePartSpecifier::TIMEZONE:
			return ExtractTimezone;
		case DatePartSpecifier::TIMEZONE_HOUR:
			return ExtractTimezoneHour;
		case DatePartSpecifier::TIMEZONE_MINUTE:
			return ExtractTimezoneMinute;
		default:
			throw InternalException("Unsupported ICU BIGINT extractor");
		}
	}

	static part_double_t PartCodeDoubleFactory(DatePartSpecifier part) {
		switch (part) {
		case DatePartSpecifier::EPOCH:
			return ExtractEpoch;
		case DatePartSpecifier::JULIAN_DAY:
			return ExtractJulianDay;
		default:
			throw InternalException("Unsupported ICU DOUBLE extractor");
		}
	}

	static date_t MakeLastDay(Calendar *calendar, const uint64_t micros) {
		// Set the calendar to midnight on the last day of the month
		calendar->Set(CAL_MILLISECOND, 0);
		calendar->Set(CAL_SECOND, 0);
		calendar->Set(CAL_MINUTE, 0);
		calendar->Set(CAL_HOUR_OF_DAY, 0);

		const auto dd = calendar->GetActualMaximum(CAL_DATE);
		if (calendar->HasFailed()) {
			throw InternalException("Unable to extract last day.");
		}

		calendar->Set(CAL_DATE, dd);

		//	Offset to UTC
		auto millis = calendar->GetTime();
		millis += ExtractField(calendar, CAL_ZONE_OFFSET);
		millis += ExtractField(calendar, CAL_DST_OFFSET);

		return Date::EpochToDate(millis / Interval::MSECS_PER_SEC);
	}

	static const char *MonthName(Calendar *calendar, const uint64_t micros) {
		const auto mm = ExtractMonth(calendar, micros) - 1;
		if (mm == 12) {
			return "Undecimber";
		}
		return Date::MONTH_NAMES[mm];
	}

	static const char *DayName(Calendar *calendar, const uint64_t micros) {
		return Date::DAY_NAMES[ExtractDayOfWeek(calendar, micros)];
	}

	//! The epoch is read from the instant itself, every other part from the calendar of the settings
	static cxx::FunctionStability PartCodeStability(DatePartSpecifier part_code) {
		return part_code == DatePartSpecifier::EPOCH ? cxx::FunctionStability::CONSISTENT
		                                             : cxx::FunctionStability::CONSISTENT_WITHIN_QUERY;
	}

	//===--------------------------------------------------------------------===//
	// Single parts
	//===--------------------------------------------------------------------===//
	struct UnaryPartInfo {
		DatePartSpecifier part;
	};

	template <typename INPUT_TYPE, typename RESULT_TYPE>
	static void UnaryTimestampFunction(cxx::ScalarFunction::ExecInput &input) {
		auto &info = input.GetBindData<BindData>();
		const auto &part = input.GetUserData<UnaryPartInfo>().part;
		CalendarPtr calendar_ptr(info.calendar->Copy());
		auto calendar = calendar_ptr.get();

		ExecuteRows<RESULT_TYPE, INPUT_TYPE>(input, [&](INPUT_TYPE value) -> std::optional<RESULT_TYPE> {
			if (!IsFinite(value)) {
				return std::nullopt;
			}
			const auto micros = SetTime(calendar, TruncCast<INPUT_TYPE, timestamp_tz_t>(value));
			if constexpr (std::is_same<RESULT_TYPE, double>::value) {
				return PartCodeDoubleFactory(part)(calendar, micros);
			} else {
				return PartCodeBigintFactory(part)(calendar, micros);
			}
		});
	}

	template <typename RESULT_TYPE>
	static void AddUnaryPartCodeFunctions(const ICURegistration &reg, const string &name, DatePartSpecifier part,
	                                      bool non_decreasing = false) {
		const auto result_type = std::is_same<RESULT_TYPE, double>::value ? "DOUBLE" : "BIGINT";
		auto add = [&](const string &type, cxx::ScalarFunction::ExecCallback exec) {
			auto function = reg.Scalar(name, {{"ts", type}}, result_type);
			function.SetBindCallback(Bind).SetExecCallback(exec);
			function.template SetUserData<UnaryPartInfo>(UnaryPartInfo {part});
			function.SetStability(PartCodeStability(part));
			if (non_decreasing) {
				// the engine derives the bounds of the result from those of the argument
				function.SetArgumentMonotonicity(0, cxx::FunctionMonotonicity::NON_DECREASING);
				function.SetStatsCallback(PropagateFiniteMonotoneBounds);
			}
			reg.extension.Register(function);
		};
		add("TIMESTAMPTZ", UnaryTimestampFunction<timestamp_tz_t, RESULT_TYPE>);
		add("TIMESTAMPTZ_NS", UnaryTimestampFunction<timestamp_tz_ns_t, RESULT_TYPE>);
	}

	//===--------------------------------------------------------------------===//
	// date_part(part, ts)
	//===--------------------------------------------------------------------===//
	template <typename INPUT_TYPE>
	static void BinaryTimestampFunction(cxx::ScalarFunction::ExecInput &input) {
		auto &info = input.GetBindData<BindData>();
		CalendarPtr calendar_ptr(info.calendar->Copy());
		auto calendar = calendar_ptr.get();

		ExecuteRows<double, std::string_view, INPUT_TYPE>(
		    input, [&](std::string_view specifier, INPUT_TYPE value) -> std::optional<double> {
			    if (!IsFinite(value)) {
				    return std::nullopt;
			    }
			    const auto micros = SetTime(calendar, TruncCast<INPUT_TYPE, timestamp_tz_t>(value));
			    const auto part_code = GetDatePartSpecifier(specifier);
			    if (IsBigintDatepart(part_code)) {
				    return double(PartCodeBigintFactory(part_code)(calendar, micros));
			    } else {
				    return PartCodeDoubleFactory(part_code)(calendar, micros);
			    }
		    });
	}

	static void BindBinaryDatePart(cxx::ScalarFunction::BindInput &input) {
		// Bind the equivalent unary function for a constant first argument.
		auto part_constant = input.TryGetConstantArgument(0);
		if (part_constant && !part_constant->IsNull()) {
			const auto part_code = GetDatePartSpecifier(part_constant->Get<cxx::varchar_t>().view());
			input.ReplaceCall(DatePartFunctionName(part_code), {1});
			return;
		}
		Bind(input);
	}

	//===--------------------------------------------------------------------===//
	// date_part([parts], ts)
	//===--------------------------------------------------------------------===//
	struct DatePartInfo {
		string name;
	};

	struct BindStructData : public BindData {
		BindStructData(cxx::Context &context, vector<DatePartSpecifier> part_codes_p)
		    : BindData(context), part_codes(std::move(part_codes_p)) {
		}

		vector<DatePartSpecifier> part_codes;

		bool operator==(const BindStructData &other) const {
			return BindData::operator==(other) && part_codes == other.part_codes;
		}
	};

	//! The names and parts of the constant list of parts of a call
	static void GetStructParts(cxx::FunctionBindInput &input, const string &name, vector<string> &part_names,
	                           vector<DatePartSpecifier> &part_codes) {
		auto parts_list = input.TryGetConstantArgument(0);
		if (!parts_list || parts_list->IsNull() ||
		    parts_list->GetLogicalType().GetTypeId() != cxx::LogicalTypeId::LIST) {
			throw BinderException("%s can only take constant lists of part names", name);
		}
		const auto count = parts_list->GetChildCount();
		if (count == 0) {
			throw BinderException("%s requires non-empty lists of part names", name);
		}
		for (idx_t col = 0; col < count; ++col) {
			auto part_value = parts_list->GetChild(col);
			if (part_value.IsNull()) {
				throw BinderException("NULL struct entry name in %s", name);
			}
			const auto part_name = part_value.GetLogicalType().GetTypeId() == cxx::LogicalTypeId::VARCHAR
			                           ? string(part_value.Get<cxx::varchar_t>().view())
			                           : part_value.ToText();
			const auto part_code = GetDatePartSpecifier(part_name);
			for (auto &existing : part_names) {
				if (StringUtil::CIEquals(existing, part_name)) {
					throw BinderException("Duplicate struct entry name \"%s\" in %s", part_name, name);
				}
			}
			part_names.push_back(part_name);
			part_codes.push_back(part_code);
		}
	}

	static string QuoteIdentifier(const string &name) {
		return "\"" + StringUtil::Replace(name, "\"", "\"\"") + "\"";
	}

	static void ResolveStruct(cxx::ScalarFunction::ResolveTypesInput &input) {
		const auto &name = input.GetUserData<DatePartInfo>().name;
		vector<string> part_names;
		vector<DatePartSpecifier> part_codes;
		GetStructParts(input, name, part_names, part_codes);
		string type = "STRUCT(";
		for (idx_t col = 0; col < part_names.size(); ++col) {
			if (col > 0) {
				type += ", ";
			}
			type += QuoteIdentifier(part_names[col]) + (IsBigintDatepart(part_codes[col]) ? " BIGINT" : " DOUBLE");
		}
		type += ")";
		input.SetReturnType(input.GetContext().GetFactory().ParseType(type));
	}

	static void BindStruct(cxx::ScalarFunction::BindInput &input) {
		const auto &name = input.GetUserData<DatePartInfo>().name;
		vector<string> part_names;
		vector<DatePartSpecifier> part_codes;
		GetStructParts(input, name, part_names, part_codes);
		bool consistent = true;
		for (auto part_code : part_codes) {
			consistent = consistent && PartCodeStability(part_code) == cxx::FunctionStability::CONSISTENT;
		}
		if (consistent) {
			input.SetStability(cxx::FunctionStability::CONSISTENT);
		}
		input.SetBindData<BindStructData>(input.GetContext(), std::move(part_codes));
	}

	template <typename INPUT_TYPE>
	static void StructFunction(cxx::ScalarFunction::ExecInput &input) {
		auto &info = input.GetBindData<BindStructData>();
		CalendarPtr calendar_ptr(info.calendar->Copy());
		auto calendar = calendar_ptr.get();

		const auto count = input.GetRowCount();
		ArgReader<INPUT_TYPE> values(input.GetArg(input.GetArgCount() - 1));

		auto result = input.GetResult();
		const auto column_count = info.part_codes.size();
		vector<cxx::Vector> children;
		vector<void *> child_data;
		vector<cxx::ValidityMask> child_validity;
		for (idx_t col = 0; col < column_count; ++col) {
			children.push_back(result.GetChild(col));
			child_data.push_back(children.back().GetDataMutable());
			child_validity.push_back(children.back().GetValidityMutable());
			child_validity.back().SetAllValid(count);
		}
		auto result_validity = result.GetValidityMutable();
		result_validity.SetAllValid(count);

		for (idx_t i = 0; i < count; ++i) {
			if (!values.IsValid(i)) {
				result_validity.SetInvalid(i);
				for (auto &validity : child_validity) {
					validity.SetInvalid(i);
				}
				continue;
			}
			const auto value = values.Get(i);
			if (!IsFinite(value)) {
				for (auto &validity : child_validity) {
					validity.SetInvalid(i);
				}
				continue;
			}
			auto micros = SetTime(calendar, TruncCast<INPUT_TYPE, timestamp_tz_t>(value));
			for (idx_t col = 0; col < column_count; ++col) {
				const auto part_code = info.part_codes[col];
				if (IsBigintDatepart(part_code)) {
					static_cast<int64_t *>(child_data[col])[i] = PartCodeBigintFactory(part_code)(calendar, micros);
				} else {
					static_cast<double *>(child_data[col])[i] = PartCodeDoubleFactory(part_code)(calendar, micros);
				}
			}
		}
	}

	static void AddDatePartFunctions(const ICURegistration &reg, const string &name) {
		auto add_binary = [&](const string &type, cxx::ScalarFunction::ExecCallback exec) {
			auto function = reg.Scalar(name, {{"part", "VARCHAR"}, {"ts", type}}, "DOUBLE");
			function.SetBindCallback(BindBinaryDatePart).SetExecCallback(exec);
			function.SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
			reg.extension.Register(function);
		};
		auto add_struct = [&](const string &type, cxx::ScalarFunction::ExecCallback exec) {
			auto function = reg.Scalar(name, {{"part_list", "VARCHAR[]"}, {"ts", type}}, "ANY");
			function.SetResolveTypesCallback(ResolveStruct).SetBindCallback(BindStruct).SetExecCallback(exec);
			function.SetUserData<DatePartInfo>(DatePartInfo {name});
			function.SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
			reg.extension.Register(function);
		};
		add_binary("TIMESTAMPTZ", BinaryTimestampFunction<timestamp_tz_t>);
		add_struct("TIMESTAMPTZ", StructFunction<timestamp_tz_t>);
		add_binary("TIMESTAMPTZ_NS", BinaryTimestampFunction<timestamp_tz_ns_t>);
		add_struct("TIMESTAMPTZ_NS", StructFunction<timestamp_tz_ns_t>);
	}

	//===--------------------------------------------------------------------===//
	// last_day, monthname, dayname
	//===--------------------------------------------------------------------===//
	template <typename INPUT_TYPE>
	static void LastDayFunction(cxx::ScalarFunction::ExecInput &input) {
		auto &info = input.GetBindData<BindData>();
		CalendarPtr calendar_ptr(info.calendar->Copy());
		auto calendar = calendar_ptr.get();
		ExecuteRows<date_t, INPUT_TYPE>(input, [&](INPUT_TYPE value) -> std::optional<date_t> {
			if (!IsFinite(value)) {
				return std::nullopt;
			}
			const auto micros = SetTime(calendar, TruncCast<INPUT_TYPE, timestamp_tz_t>(value));
			return MakeLastDay(calendar, micros);
		});
	}

	template <typename INPUT_TYPE, bool MONTH>
	static void NameFunction(cxx::ScalarFunction::ExecInput &input) {
		auto &info = input.GetBindData<BindData>();
		CalendarPtr calendar_ptr(info.calendar->Copy());
		auto calendar = calendar_ptr.get();
		ExecuteStringRows<INPUT_TYPE>(input, [&](string &out, INPUT_TYPE value) {
			if (!IsFinite(value)) {
				return false;
			}
			const auto micros = SetTime(calendar, TruncCast<INPUT_TYPE, timestamp_tz_t>(value));
			out = MONTH ? MonthName(calendar, micros) : DayName(calendar, micros);
			return true;
		});
	}

	static void AddUnaryFunctions(const ICURegistration &reg, const string &name, const string &result_type,
	                              cxx::ScalarFunction::ExecCallback tstz_exec,
	                              cxx::ScalarFunction::ExecCallback tstz_ns_exec) {
		auto add = [&](const string &type, cxx::ScalarFunction::ExecCallback exec) {
			auto function = reg.Scalar(name, {{"ts", type}}, result_type);
			function.SetBindCallback(Bind).SetExecCallback(exec);
			function.SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
			reg.extension.Register(function);
		};
		add("TIMESTAMPTZ", tstz_exec);
		add("TIMESTAMPTZ_NS", tstz_ns_exec);
	}
};

void RegisterICUDatePartFunctions(const ICURegistration &reg) {
	using P = DatePartSpecifier;
	// register the individual operators

	// year/decade use CAL_YEAR (year-of-era, positive in both BC and AD), which is non-monotonic
	// across the BC/AD flip; leave them unannotated. century/millennium are negated in era 0, which is the only era
	// of some calendars, and yearweek decreases within negative years, so these are unannotated as well.

	//	BIGINTs
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "era", P::ERA, true);
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "year", P::YEAR);
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "month", P::MONTH);
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "day", P::DAY);
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "decade", P::DECADE);
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "century", P::CENTURY);
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "millennium", P::MILLENNIUM);
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "microsecond", P::MICROSECONDS);
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "millisecond", P::MILLISECONDS);
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "second", P::SECOND);
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "minute", P::MINUTE);
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "hour", P::HOUR);
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "dayofweek", P::DOW);
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "isodow", P::ISODOW);
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "week", P::WEEK); //  Note that WeekOperator is ISO-8601
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "dayofyear", P::DOY);
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "quarter", P::QUARTER);
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "isoyear", P::ISOYEAR, true);
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "timezone", P::TIMEZONE);
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "timezone_hour", P::TIMEZONE_HOUR);
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "timezone_minute", P::TIMEZONE_MINUTE);

	//	DOUBLEs
	ICUDatePart::AddUnaryPartCodeFunctions<double>(reg, "epoch", P::EPOCH);
	ICUDatePart::AddUnaryPartCodeFunctions<double>(reg, "julian", P::JULIAN_DAY);

	//  register combinations
	// yearweek subtracts the week for ISO year <= 0, so it is not monotone across that range
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "yearweek", P::YEARWEEK);

	//  register various aliases
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "dayofmonth", P::DAY);
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "weekday", P::DOW);
	ICUDatePart::AddUnaryPartCodeFunctions<int64_t>(reg, "weekofyear", P::WEEK);

	//  register the last_day function
	ICUDatePart::AddUnaryFunctions(reg, "last_day", "DATE", ICUDatePart::LastDayFunction<timestamp_tz_t>,
	                               ICUDatePart::LastDayFunction<timestamp_tz_ns_t>);

	// register the dayname/monthname functions
	ICUDatePart::AddUnaryFunctions(reg, "monthname", "VARCHAR", ICUDatePart::NameFunction<timestamp_tz_t, true>,
	                               ICUDatePart::NameFunction<timestamp_tz_ns_t, true>);
	ICUDatePart::AddUnaryFunctions(reg, "dayname", "VARCHAR", ICUDatePart::NameFunction<timestamp_tz_t, false>,
	                               ICUDatePart::NameFunction<timestamp_tz_ns_t, false>);

	// finally the actual date_part function
	ICUDatePart::AddDatePartFunctions(reg, "date_part");
	ICUDatePart::AddDatePartFunctions(reg, "datepart");
}

} // namespace duckdb_icu
