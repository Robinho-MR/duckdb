#include "icu-datefunc.hpp"
#include "icu-vector.hpp"

namespace duckdb_icu {

//! The constant part a call is bound with, if any
static bool TryGetConstantPart(cxx::ScalarFunction::BindInput &input, DatePartSpecifier &part, bool &valid) {
	auto part_value = input.TryGetConstantArgument(0);
	if (!part_value || part_value->IsNull()) {
		return false;
	}
	valid = TryGetDatePartSpecifier(part_value->Get<cxx::varchar_t>().view(), part);
	return true;
}

//! Withdraws the monotonicity in both timestamps for a call whose constant part makes it not monotone
static void ClearTimestampMonotonicity(cxx::ScalarFunction::BindInput &input) {
	input.SetArgumentMonotonicity(1, cxx::FunctionMonotonicity::UNKNOWN);
	input.SetArgumentMonotonicity(2, cxx::FunctionMonotonicity::UNKNOWN);
}

static void DeclareTimestampMonotonicity(cxx::ScalarFunction &function) {
	function.SetArgumentMonotonicity(1, cxx::FunctionMonotonicity::NON_INCREASING);
	function.SetArgumentMonotonicity(2, cxx::FunctionMonotonicity::NON_DECREASING);
	function.SetStatsCallback(ICUDateFunc::PropagateFiniteMonotoneBounds);
}

struct ICUCalendarSub : public ICUDateFunc {
	static bool SubtractsElapsedTime(DatePartSpecifier part) {
		switch (part) {
		case DatePartSpecifier::HOUR:
		case DatePartSpecifier::MINUTE:
		case DatePartSpecifier::SECOND:
		case DatePartSpecifier::EPOCH:
		case DatePartSpecifier::MILLISECONDS:
		case DatePartSpecifier::MICROSECONDS:
			return true;
		default:
			return false;
		}
	}

	static void BindSub(cxx::ScalarFunction::BindInput &input) {
		DatePartSpecifier part;
		bool valid;
		if (TryGetConstantPart(input, part, valid) && (!valid || !SubtractsElapsedTime(part))) {
			// calendar parts count whole local units, so moving one argument can step the result back,
			// e.g. a month boundary that falls between two timestamps or a local day that repeats
			ClearTimestampMonotonicity(input);
		}
		ICUDateFunc::Bind(input);
	}

	//	ICU only has 32 bit precision for date parts, so it can overflow a high resolution.
	//	Since there is no difference between ICU and the obvious calculations,
	//	we make these using the DuckDB internal type.
	static int64_t SubtractMicrosecond(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		// the span of two representable timestamps does not always fit in one, so this needs the same
		// overflow check the non-timezone date_diff uses
		return SubtractOverflowCheck(end_date.micros, start_date.micros);
	}

	template <int64_t MICROS_PER_UNIT>
	static int64_t SubtractUnits(timestamp_tz_t start_date, timestamp_tz_t end_date) {
		int64_t delta_us;
		if (TrySubtract(end_date.micros, start_date.micros, delta_us)) {
			return delta_us / MICROS_PER_UNIT;
		}
		return Interval::GetDifferenceInUnits(start_date.micros, end_date.micros, MICROS_PER_UNIT);
	}

	static int64_t SubtractMillisecond(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		return SubtractUnits<Interval::MICROS_PER_MSEC>(start_date, end_date);
	}

	static int64_t SubtractSecond(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		return SubtractUnits<Interval::MICROS_PER_SEC>(start_date, end_date);
	}

	static int64_t SubtractMinute(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		return SubtractUnits<Interval::MICROS_PER_MINUTE>(start_date, end_date);
	}

	static int64_t SubtractHour(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		SetTime(calendar, start_date);
		return SubtractField(calendar, CAL_HOUR_OF_DAY, end_date);
	}

	static int64_t SubtractDay(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		SetTime(calendar, start_date);
		return SubtractField(calendar, CAL_DATE, end_date);
	}

	static int64_t SubtractWeek(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		calendar->SetFirstDayOfWeek(CAL_MONDAY);
		calendar->SetMinimalDaysInFirstWeek(4);
		SetTime(calendar, start_date);
		return SubtractField(calendar, CAL_WEEK_OF_YEAR, end_date);
	}

	static int64_t SubtractMonth(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		SetTime(calendar, start_date);
		return SubtractField(calendar, CAL_MONTH, end_date);
	}

	static int64_t SubtractQuarter(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		// No ICU part for this, so do it manually.
		// This will not work for lunar calendars!
		return SubtractMonth(calendar, start_date, end_date) / 3;
	}

	static int64_t SubtractYear(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		SetTime(calendar, start_date);
		return SubtractField(calendar, CAL_YEAR, end_date);
	}

	static int64_t SubtractISOYear(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		calendar->SetFirstDayOfWeek(CAL_MONDAY);
		calendar->SetMinimalDaysInFirstWeek(4);
		SetTime(calendar, start_date);
		return SubtractField(calendar, CAL_YEAR_WOY, end_date);
	}

	static int64_t SubtractDecade(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		// No ICU part for this, so do it manually.
		return SubtractYear(calendar, start_date, end_date) / 10;
	}

	static int64_t SubtractCentury(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		// No ICU part for this, so do it manually.
		return SubtractYear(calendar, start_date, end_date) / 100;
	}

	static int64_t SubtractMillenium(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		// No ICU part for this, so do it manually.
		return SubtractYear(calendar, start_date, end_date) / 1000;
	}

	static int64_t SubtractEra(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		SetTime(calendar, start_date);
		return SubtractField(calendar, CAL_ERA, end_date);
	}

	template <typename T>
	static void ICUDateSubFunction(cxx::ScalarFunction::ExecInput &input) {
		auto &info = input.GetBindData<BindData>();
		CalendarPtr calendar(info.calendar->Copy());

		auto part_arg = input.GetArg(0);
		part_sub_t constant_func = nullptr;
		if (IsConstant(part_arg)) {
			// Common case of constant part.
			StringArgReader part(part_arg);
			if (part.IsValid(0)) {
				constant_func = SubtractFactory(GetDatePartSpecifier(part.Get(0)));
			}
		}
		ExecuteRows<int64_t, std::string_view, T, T>(
		    input, [&](std::string_view specifier, T start_date, T end_date) -> std::optional<int64_t> {
			    if (!IsFinite(start_date) || !IsFinite(end_date)) {
				    return std::nullopt;
			    }
			    auto part_func = constant_func ? constant_func : SubtractFactory(GetDatePartSpecifier(specifier));
			    return part_func(calendar.get(), start_date, end_date);
		    });
	}

	static void AddFunctions(const ICURegistration &reg, const string &name) {
		auto function =
		    reg.Scalar(name, {{"part", "VARCHAR"}, {"startdate", "TIMESTAMPTZ"}, {"enddate", "TIMESTAMPTZ"}}, "BIGINT");
		function.SetBindCallback(BindSub).SetExecCallback(ICUDateSubFunction<timestamp_tz_t>);
		// throws for unrecognized part specifiers and for dates that overflow the timestamp range
		function.SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
		DeclareTimestampMonotonicity(function);
		reg.extension.Register(function);
	}
};

ICUDateFunc::part_sub_t ICUDateFunc::SubtractFactory(DatePartSpecifier type) {
	switch (type) {
	case DatePartSpecifier::MILLENNIUM:
		return ICUCalendarSub::SubtractMillenium;
	case DatePartSpecifier::CENTURY:
		return ICUCalendarSub::SubtractCentury;
	case DatePartSpecifier::DECADE:
		return ICUCalendarSub::SubtractDecade;
	case DatePartSpecifier::YEAR:
		return ICUCalendarSub::SubtractYear;
	case DatePartSpecifier::QUARTER:
		return ICUCalendarSub::SubtractQuarter;
	case DatePartSpecifier::MONTH:
		return ICUCalendarSub::SubtractMonth;
	case DatePartSpecifier::WEEK:
	case DatePartSpecifier::YEARWEEK:
		return ICUCalendarSub::SubtractWeek;
	case DatePartSpecifier::ISOYEAR:
		return ICUCalendarSub::SubtractISOYear;
	case DatePartSpecifier::DAY:
	case DatePartSpecifier::DOW:
	case DatePartSpecifier::ISODOW:
	case DatePartSpecifier::DOY:
	case DatePartSpecifier::JULIAN_DAY:
		return ICUCalendarSub::SubtractDay;
	case DatePartSpecifier::HOUR:
		return ICUCalendarSub::SubtractHour;
	case DatePartSpecifier::MINUTE:
		return ICUCalendarSub::SubtractMinute;
	case DatePartSpecifier::SECOND:
	case DatePartSpecifier::EPOCH:
		return ICUCalendarSub::SubtractSecond;
	case DatePartSpecifier::MILLISECONDS:
		return ICUCalendarSub::SubtractMillisecond;
	case DatePartSpecifier::MICROSECONDS:
		return ICUCalendarSub::SubtractMicrosecond;
	case DatePartSpecifier::ERA:
		return ICUCalendarSub::SubtractEra;
	default:
		throw NotImplementedException("Specifier type not implemented for ICU subtraction");
	}
}

// MS-SQL differences can be computed using ICU by truncating both arguments
// to the desired part precision and then applying ICU subtraction/difference
struct ICUCalendarDiff : public ICUDateFunc {
	static bool TruncatesToSecondOrFiner(DatePartSpecifier part) {
		switch (part) {
		case DatePartSpecifier::SECOND:
		case DatePartSpecifier::EPOCH:
		case DatePartSpecifier::MILLISECONDS:
		case DatePartSpecifier::MICROSECONDS:
			return true;
		default:
			return false;
		}
	}

	static void BindDiff(cxx::ScalarFunction::BindInput &input) {
		DatePartSpecifier part;
		bool valid;
		if (TryGetConstantPart(input, part, valid) && (!valid || !TruncatesToSecondOrFiner(part))) {
			// date_diff truncates both arguments first, and truncating to a coarser part follows the local
			// time, which repeats or moves backwards when the UTC offset changes
			ClearTimestampMonotonicity(input);
		}
		ICUDateFunc::Bind(input);
	}

	static int64_t DifferenceEra(Calendar *calendar, timestamp_tz_t start_date, timestamp_tz_t end_date) {
		SetTime(calendar, start_date);
		const auto start_era = ExtractField(calendar, CAL_ERA);
		SetTime(calendar, end_date);
		return int64_t(ExtractField(calendar, CAL_ERA)) - start_era;
	}

	static int64_t DifferenceEra(Calendar *calendar, timestamp_tz_ns_t start_date, timestamp_tz_ns_t end_date) {
		SetTimeNS(calendar, start_date);
		const auto start_era = ExtractField(calendar, CAL_ERA);
		SetTimeNS(calendar, end_date);
		return int64_t(ExtractField(calendar, CAL_ERA)) - start_era;
	}

	static timestamp_tz_t TruncateForDiff(Calendar *calendar, timestamp_tz_t date, part_trunc_t trunc_func) {
		auto micros = SetTime(calendar, date);
		trunc_func(calendar, micros);
		return GetTimeUnsafe(calendar, micros);
	}

	static timestamp_tz_t TruncateForDiff(Calendar *calendar, timestamp_tz_ns_t date, part_trunc_t trunc_func) {
		auto nanos = SetTimeNS(calendar, date);
		// Adapt TIMESTAMPTZ_NS to the existing microsecond-or-coarser date_diff path.
		uint64_t micros = nanos / Interval::NANOS_PER_MICRO;
		trunc_func(calendar, micros);
		return GetTimeUnsafe(calendar, micros);
	}

	template <typename T>
	static int64_t DifferenceFunc(Calendar *calendar, T start_date, T end_date, part_trunc_t trunc_func,
	                              part_sub_t sub_func) {
		// Truncate the two arguments. This is safe because we will stay in range
		auto start_micros = TruncateForDiff(calendar, start_date, trunc_func);
		auto end_micros = TruncateForDiff(calendar, end_date, trunc_func);

		// Now use ICU difference
		return sub_func(calendar, start_micros, end_micros);
	}

	static part_trunc_t DiffTruncationFactory(DatePartSpecifier type) {
		switch (type) {
		case DatePartSpecifier::WEEK:
			//	Weeks are computed without anchors
			return TruncationFactory(DatePartSpecifier::DAY);
		default:
			break;
		}
		return TruncationFactory(type);
	}

	template <typename T>
	static void ICUDateDiffFunction(cxx::ScalarFunction::ExecInput &input) {
		auto &info = input.GetBindData<BindData>();
		CalendarPtr calendar_ptr(info.calendar->Copy());
		auto calendar = calendar_ptr.get();

		auto part_arg = input.GetArg(0);
		bool constant_part = false;
		DatePartSpecifier part = DatePartSpecifier::INVALID;
		part_trunc_t trunc_func = nullptr;
		part_sub_t sub_func = nullptr;
		if (IsConstant(part_arg)) {
			// Common case of constant part.
			StringArgReader part_reader(part_arg);
			if (part_reader.IsValid(0)) {
				constant_part = true;
				part = GetDatePartSpecifier(part_reader.Get(0));
				trunc_func = DiffTruncationFactory(part);
				sub_func = SubtractFactory(part);
			}
		}
		ExecuteRows<int64_t, std::string_view, T, T>(
		    input, [&](std::string_view specifier, T start_date, T end_date) -> std::optional<int64_t> {
			    if (!IsFinite(start_date) || !IsFinite(end_date)) {
				    return std::nullopt;
			    }
			    if (!constant_part) {
				    part = GetDatePartSpecifier(specifier);
			    }
			    if (part == DatePartSpecifier::ERA) {
				    return DifferenceEra(calendar, start_date, end_date);
			    }
			    if (!constant_part) {
				    trunc_func = DiffTruncationFactory(part);
				    sub_func = SubtractFactory(part);
			    }
			    return DifferenceFunc(calendar, start_date, end_date, trunc_func, sub_func);
		    });
	}

	template <typename T>
	static void AddFunction(const ICURegistration &reg, const string &name, const string &type) {
		auto function = reg.Scalar(name, {{"part", "VARCHAR"}, {"startdate", type}, {"enddate", type}}, "BIGINT");
		function.SetBindCallback(BindDiff).SetExecCallback(ICUDateDiffFunction<T>);
		// throws for unrecognized part specifiers and for dates that overflow the timestamp range
		function.SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
		DeclareTimestampMonotonicity(function);
		reg.extension.Register(function);
	}

	static void AddFunctions(const ICURegistration &reg, const string &name) {
		AddFunction<timestamp_tz_t>(reg, name, "TIMESTAMPTZ");
		AddFunction<timestamp_tz_ns_t>(reg, name, "TIMESTAMPTZ_NS");
	}
};

void RegisterICUDateSubFunctions(const ICURegistration &reg) {
	ICUCalendarSub::AddFunctions(reg, "date_sub");
	ICUCalendarSub::AddFunctions(reg, "datesub");

	ICUCalendarDiff::AddFunctions(reg, "date_diff");
	ICUCalendarDiff::AddFunctions(reg, "datediff");
}

} // namespace duckdb_icu
