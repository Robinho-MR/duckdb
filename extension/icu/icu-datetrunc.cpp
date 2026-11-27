#include "icu-datefunc.hpp"
#include "icu-vector.hpp"

namespace duckdb_icu {

struct ICUDateTrunc : public ICUDateFunc {
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

	static void BindTrunc(cxx::ScalarFunction::BindInput &input) {
		auto part_value = input.TryGetConstantArgument(0);
		if (part_value && !part_value->IsNull()) {
			DatePartSpecifier part;
			if (!TryGetDatePartSpecifier(part_value->Get<cxx::varchar_t>().view(), part) ||
			    !TruncatesToSecondOrFiner(part)) {
				// coarser parts follow the local time, which repeats or moves backwards when the UTC offset changes
				input.SetArgumentMonotonicity(1, cxx::FunctionMonotonicity::UNKNOWN);
			}
		}
		ICUDateFunc::Bind(input);
	}

	static void PreserveOffsets(Calendar *calendar) {
		//	We have to extract _everything_ before setting anything
		//	Otherwise ICU will clear the fStamp fields
		//	This also means we must call this method first.

		//	Force reuse of offsets when reassembling truncated sub-hour times.
		const auto zone_offset = ExtractField(calendar, CAL_ZONE_OFFSET);
		const auto dst_offset = ExtractField(calendar, CAL_DST_OFFSET);

		calendar->Set(CAL_ZONE_OFFSET, zone_offset);
		calendar->Set(CAL_DST_OFFSET, dst_offset);
	}

	static void TruncMicrosecondInternal(Calendar *calendar, uint64_t &micros) {
	}

	static void TruncMicrosecond(Calendar *calendar, uint64_t &micros) {
		PreserveOffsets(calendar);
		TruncMicrosecondInternal(calendar, micros);
	}

	static void TruncMillisecondInternal(Calendar *calendar, uint64_t &micros) {
		TruncMicrosecondInternal(calendar, micros);
		micros = 0;
	}

	static void TruncMillisecond(Calendar *calendar, uint64_t &micros) {
		PreserveOffsets(calendar);
		TruncMillisecondInternal(calendar, micros);
	}

	static void TruncSecondInternal(Calendar *calendar, uint64_t &micros) {
		TruncMillisecondInternal(calendar, micros);
		calendar->Set(CAL_MILLISECOND, 0);
	}

	static void TruncSecond(Calendar *calendar, uint64_t &micros) {
		PreserveOffsets(calendar);
		TruncSecondInternal(calendar, micros);
	}

	static void TruncMinuteInternal(Calendar *calendar, uint64_t &micros) {
		TruncSecondInternal(calendar, micros);
		calendar->Set(CAL_SECOND, 0);
	}

	static void TruncMinute(Calendar *calendar, uint64_t &micros) {
		PreserveOffsets(calendar);
		TruncMinuteInternal(calendar, micros);
	}

	static void TruncHour(Calendar *calendar, uint64_t &micros) {
		TruncMinuteInternal(calendar, micros);
		calendar->Set(CAL_MINUTE, 0);
	}

	static void TruncDay(Calendar *calendar, uint64_t &micros) {
		TruncHour(calendar, micros);
		calendar->Set(CAL_HOUR_OF_DAY, 0);
	}

	static void TruncWeek(Calendar *calendar, uint64_t &micros) {
		calendar->SetFirstDayOfWeek(CAL_MONDAY);
		calendar->SetMinimalDaysInFirstWeek(4);
		TruncDay(calendar, micros);
		calendar->Set(CAL_DAY_OF_WEEK, CAL_MONDAY);
	}

	static void TruncMonth(Calendar *calendar, uint64_t &micros) {
		TruncDay(calendar, micros);
		calendar->Set(CAL_DATE, 1);
	}

	static void TruncQuarter(Calendar *calendar, uint64_t &micros) {
		TruncMonth(calendar, micros);
		auto mm = ExtractField(calendar, CAL_MONTH);
		calendar->Set(CAL_MONTH, (mm / 3) * 3);
	}

	static void TruncYear(Calendar *calendar, uint64_t &micros) {
		TruncMonth(calendar, micros);
		calendar->Set(CAL_MONTH, CAL_JANUARY);
	}

	static void TruncISOYear(Calendar *calendar, uint64_t &micros) {
		TruncWeek(calendar, micros);
		calendar->Set(CAL_WEEK_OF_YEAR, 1);
	}

	static void TruncDecade(Calendar *calendar, uint64_t &micros) {
		TruncYear(calendar, micros);
		auto yyyy = ExtractField(calendar, CAL_YEAR) / 10;
		calendar->Set(CAL_YEAR, yyyy * 10);
	}

	static void TruncCentury(Calendar *calendar, uint64_t &micros) {
		TruncYear(calendar, micros);
		auto yyyy = ExtractField(calendar, CAL_YEAR) / 100;
		calendar->Set(CAL_YEAR, yyyy * 100);
	}

	static void TruncMillenium(Calendar *calendar, uint64_t &micros) {
		TruncYear(calendar, micros);
		auto yyyy = ExtractField(calendar, CAL_YEAR) / 1000;
		calendar->Set(CAL_YEAR, yyyy * 1000);
	}

	static void TruncEra(Calendar *calendar, uint64_t &micros) {
		TruncYear(calendar, micros);
		auto era = ExtractField(calendar, CAL_ERA);
		calendar->Set(CAL_YEAR, 0);
		calendar->Set(CAL_ERA, era);
	}

	template <typename T>
	static void ICUDateTruncFunction(cxx::ScalarFunction::ExecInput &input) {
		auto &info = input.GetBindData<BindData>();
		CalendarPtr calendar(info.calendar->Copy());

		auto part_arg = input.GetArg(0);
		part_trunc_t constant_truncator = nullptr;
		if (IsConstant(part_arg)) {
			// Common case of constant part.
			StringArgReader part(part_arg);
			if (part.IsValid(0)) {
				constant_truncator = TruncationFactory(GetDatePartSpecifier(part.Get(0)));
			}
		}
		ExecuteRows<timestamp_tz_t, std::string_view, T>(
		    input, [&](std::string_view specifier, T value) -> std::optional<timestamp_tz_t> {
			    if (!IsFinite(value)) {
				    return timestamp_tz_t {Raw(value)};
			    }
			    auto truncator =
			        constant_truncator ? constant_truncator : TruncationFactory(GetDatePartSpecifier(specifier));
			    auto micros = SetTime(calendar.get(), TruncCast<T, timestamp_tz_t>(value));
			    truncator(calendar.get(), micros);
			    return GetTimeUnsafe(calendar.get(), micros);
		    });
	}

	template <typename T>
	static void AddFunction(const ICURegistration &reg, const string &name, const string &type) {
		auto function = reg.Scalar(name, {{"part", "VARCHAR"}, {"timestamp", type}}, "TIMESTAMPTZ");
		function.SetBindCallback(BindTrunc).SetExecCallback(ICUDateTruncFunction<T>);
		// throws for unrecognized part specifiers and for dates that overflow the timestamp range
		function.SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
		function.SetArgumentMonotonicity(1, cxx::FunctionMonotonicity::NON_DECREASING);
		reg.extension.Register(function);
	}

	static void AddBinaryTimestampFunction(const ICURegistration &reg, const string &name) {
		AddFunction<timestamp_tz_t>(reg, name, "TIMESTAMPTZ");
		AddFunction<timestamp_tz_ns_t>(reg, name, "TIMESTAMPTZ_NS");
	}
};

ICUDateFunc::part_trunc_t ICUDateFunc::TruncationFactory(DatePartSpecifier type) {
	switch (type) {
	case DatePartSpecifier::ERA:
		return ICUDateTrunc::TruncEra;
	case DatePartSpecifier::MILLENNIUM:
		return ICUDateTrunc::TruncMillenium;
	case DatePartSpecifier::CENTURY:
		return ICUDateTrunc::TruncCentury;
	case DatePartSpecifier::DECADE:
		return ICUDateTrunc::TruncDecade;
	case DatePartSpecifier::YEAR:
		return ICUDateTrunc::TruncYear;
	case DatePartSpecifier::QUARTER:
		return ICUDateTrunc::TruncQuarter;
	case DatePartSpecifier::MONTH:
		return ICUDateTrunc::TruncMonth;
	case DatePartSpecifier::WEEK:
	case DatePartSpecifier::YEARWEEK:
		return ICUDateTrunc::TruncWeek;
	case DatePartSpecifier::ISOYEAR:
		return ICUDateTrunc::TruncISOYear;
	case DatePartSpecifier::DAY:
	case DatePartSpecifier::DOW:
	case DatePartSpecifier::ISODOW:
	case DatePartSpecifier::DOY:
	case DatePartSpecifier::JULIAN_DAY:
		return ICUDateTrunc::TruncDay;
	case DatePartSpecifier::HOUR:
		return ICUDateTrunc::TruncHour;
	case DatePartSpecifier::MINUTE:
		return ICUDateTrunc::TruncMinute;
	case DatePartSpecifier::SECOND:
	case DatePartSpecifier::EPOCH:
		return ICUDateTrunc::TruncSecond;
	case DatePartSpecifier::MILLISECONDS:
		return ICUDateTrunc::TruncMillisecond;
	case DatePartSpecifier::MICROSECONDS:
		return ICUDateTrunc::TruncMicrosecond;
	default:
		throw NotImplementedException("Specifier type not implemented for ICU DATETRUNC");
	}
}

timestamp_tz_t ICUDateFunc::CurrentMidnight(Calendar *calendar, cxx::Context &context) {
	const auto current_timestamp = context.GetTransactionStartTime();
	auto current_micros = SetTime(calendar, current_timestamp);
	ICUDateTrunc::TruncDay(calendar, current_micros);
	return GetTime(calendar);
}

void RegisterICUDateTruncFunctions(const ICURegistration &reg) {
	ICUDateTrunc::AddBinaryTimestampFunction(reg, "date_trunc");
	ICUDateTrunc::AddBinaryTimestampFunction(reg, "datetrunc");
}

} // namespace duckdb_icu
