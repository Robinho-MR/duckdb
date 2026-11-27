#include "icu-datefunc.hpp"
#include "icu-vector.hpp"

namespace duckdb_icu {

static void ICUBindIntervalMonths(cxx::ScalarFunction::BindInput &input) {
	ICUDateFunc::BindData data(input.GetContext());
	TZCalendar calendar(*data.calendar, data.cal_setting);
	if (!calendar.SupportsIntervals()) {
		throw NotImplementedException("INTERVALs do not work with 13 month calendars. Try using DATE_DIFF instead.");
	}
	input.SetBindData<ICUDateFunc::BindData>(std::move(data));
}

struct ICUCalendarAdd {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right, TZCalendar &calendar_p);
};

struct ICUCalendarSub : public ICUDateFunc {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right, TZCalendar &calendar_p);
};

struct ICUCalendarAge : public ICUDateFunc {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right, TZCalendar &calendar_p);
};

static inline void CalendarAddHour(Calendar *calendar, int64_t interval_hour) {
	if (interval_hour >= 0) {
		while (interval_hour > 0) {
			calendar->Add(CAL_HOUR, interval_hour > NumericLimits<int32_t>::Maximum()
			                            ? NumericLimits<int32_t>::Maximum()
			                            : static_cast<int32_t>(interval_hour));
			interval_hour -= NumericLimits<int32_t>::Maximum();
		}
	} else {
		while (interval_hour < 0) {
			calendar->Add(CAL_HOUR, interval_hour < NumericLimits<int32_t>::Minimum()
			                            ? NumericLimits<int32_t>::Minimum()
			                            : static_cast<int32_t>(interval_hour));
			interval_hour -= NumericLimits<int32_t>::Minimum();
		}
	}
}

template <>
timestamp_tz_t ICUCalendarAdd::Operation(timestamp_tz_t timestamp, interval_t interval, TZCalendar &calendar_p) {
	if (!IsFinite(timestamp)) {
		return timestamp;
	}
	auto calendar = calendar_p.GetCalendar();

	int64_t millis = timestamp.micros / Interval::MICROS_PER_MSEC;
	int64_t micros = timestamp.micros % Interval::MICROS_PER_MSEC;

	// Manually move the µs
	micros += interval.micros % Interval::MICROS_PER_MSEC;
	if (micros >= Interval::MICROS_PER_MSEC) {
		micros -= Interval::MICROS_PER_MSEC;
		++millis;
	} else if (micros < 0) {
		micros += Interval::MICROS_PER_MSEC;
		--millis;
	}

	// Now use the calendar to add the other parts
	calendar->SetTime(millis);

	// Break units apart to avoid overflow
	auto interval_h = interval.micros / Interval::MICROS_PER_MSEC;

	const auto interval_ms = static_cast<int32_t>(interval_h % Interval::MSECS_PER_SEC);
	interval_h /= Interval::MSECS_PER_SEC;

	const auto interval_s = static_cast<int32_t>(interval_h % Interval::SECS_PER_MINUTE);
	interval_h /= Interval::SECS_PER_MINUTE;

	const auto interval_m = static_cast<int32_t>(interval_h % Interval::MINS_PER_HOUR);
	interval_h /= Interval::MINS_PER_HOUR;

	if (interval.months < 0 || interval.days < 0 || interval.micros < 0) {
		// Add interval fields from lowest to highest (non-ragged to ragged)
		calendar->Add(CAL_MILLISECOND, interval_ms);
		calendar->Add(CAL_SECOND, interval_s);
		calendar->Add(CAL_MINUTE, interval_m);
		CalendarAddHour(calendar, interval_h);

		// PG Adds months before days
		calendar->Add(CAL_MONTH, interval.months);
		calendar->Add(CAL_DATE, interval.days);
	} else {
		// PG Adds months before days
		calendar->Add(CAL_MONTH, interval.months);
		calendar->Add(CAL_DATE, interval.days);

		// Add interval fields from highest to lowest (ragged to non-ragged)
		CalendarAddHour(calendar, interval_h);
		calendar->Add(CAL_MINUTE, interval_m);
		calendar->Add(CAL_SECOND, interval_s);
		calendar->Add(CAL_MILLISECOND, interval_ms);
	}

	return ICUDateFunc::GetTime(calendar, uint64_t(micros));
}

template <>
timestamp_tz_t ICUCalendarAdd::Operation(interval_t interval, timestamp_tz_t timestamp, TZCalendar &calendar) {
	return Operation<timestamp_tz_t, interval_t, timestamp_tz_t>(timestamp, interval, calendar);
}

template <>
timestamp_tz_ns_t ICUCalendarAdd::Operation(timestamp_tz_ns_t ns, interval_t interval, TZCalendar &calendar) {
	if (!IsFinite(ns)) {
		return ns;
	}
	timestamp_tz_t us;
	int64_t nanos;
	const auto precision = Interval::NANOS_PER_MICRO;
	Timestamp::SplitTime(ns.nanos, precision, us.micros, nanos);
	us = Operation<timestamp_tz_t, interval_t, timestamp_tz_t>(us, interval, calendar);
	if (!Timestamp::TryCombineUnits(us.micros, nanos, precision, ns.nanos)) {
		throw OutOfRangeException("Cannot add interval to timestamp_tz_ns");
	}
	return ns;
}

template <>
timestamp_tz_ns_t ICUCalendarAdd::Operation(interval_t interval, timestamp_tz_ns_t timestamp, TZCalendar &calendar) {
	return Operation<timestamp_tz_ns_t, interval_t, timestamp_tz_ns_t>(timestamp, interval, calendar);
}

template <>
timestamp_tz_t ICUCalendarSub::Operation(timestamp_tz_t timestamp, interval_t interval, TZCalendar &calendar) {
	const auto negated = Interval::Invert(interval);
	return ICUCalendarAdd::template Operation<timestamp_tz_t, interval_t, timestamp_tz_t>(timestamp, negated, calendar);
}

template <>
timestamp_tz_ns_t ICUCalendarSub::Operation(timestamp_tz_ns_t ns, interval_t interval, TZCalendar &calendar) {
	const auto negated = Interval::Invert(interval);
	return ICUCalendarAdd::template Operation<timestamp_tz_ns_t, interval_t, timestamp_tz_ns_t>(ns, negated, calendar);
}

template <>
interval_t ICUCalendarSub::Operation(timestamp_tz_t end_date, timestamp_tz_t start_date, TZCalendar &calendar_p) {
	if (!IsFinite(end_date) || !IsFinite(start_date)) {
		throw InvalidInputException("Cannot subtract infinite timestamps");
	}
	if (start_date.micros > end_date.micros) {
		auto negated = Operation<timestamp_tz_t, timestamp_tz_t, interval_t>(start_date, end_date, calendar_p);
		return Interval::Make(-negated.months, -negated.days, -negated.micros);
	}
	auto calendar = calendar_p.GetCalendar();

	auto start_micros = ICUDateFunc::SetTime(calendar, start_date);
	auto end_micros = (uint64_t)(end_date.micros % Interval::MICROS_PER_MSEC);

	// Borrow 1ms from end_date if we wrap. This works because start_date <= end_date
	// and if the µs are out of order, then there must be an extra ms.
	if (start_micros > (idx_t)end_micros) {
		end_date.micros -= Interval::MICROS_PER_MSEC;
		end_micros += Interval::MICROS_PER_MSEC;
	}

	//	Timestamp differences do not use months, so start with days
	interval_t result;
	result.months = 0;
	result.days = SubtractField(calendar, CAL_DATE, end_date);

	auto hour_diff = SubtractField(calendar, CAL_HOUR_OF_DAY, end_date);
	auto min_diff = SubtractField(calendar, CAL_MINUTE, end_date);
	auto sec_diff = SubtractField(calendar, CAL_SECOND, end_date);
	auto ms_diff = SubtractField(calendar, CAL_MILLISECOND, end_date);
	auto micros_diff = int32_t(ms_diff * Interval::MICROS_PER_MSEC + int64_t(end_micros - start_micros));
	result.micros = Time::FromTime(hour_diff, min_diff, sec_diff, micros_diff).micros;

	return result;
}

template <>
interval_t ICUCalendarAge::Operation(timestamp_tz_t end_date, timestamp_tz_t start_date, TZCalendar &calendar_p) {
	auto calendar = calendar_p.GetCalendar();
	if (calendar_p.IsGregorian()) {
		auto start_data = ICUHelpers::GetComponents(start_date, calendar);
		auto end_data = ICUHelpers::GetComponents(end_date, calendar);
		return GetAge(end_data, start_data, start_date.micros > end_date.micros);
	}
	// fallback for non-gregorian calendars, since Interval::GetAge does not handle
	if (start_date.micros > end_date.micros) {
		auto negated = Operation<timestamp_tz_t, timestamp_tz_t, interval_t>(start_date, end_date, calendar_p);
		return Interval::Make(-negated.months, -negated.days, -negated.micros);
	}

	auto start_micros = ICUDateFunc::SetTime(calendar, start_date);
	auto end_micros = (uint64_t)(end_date.micros % Interval::MICROS_PER_MSEC);

	// Borrow 1ms from end_date if we wrap. This works because start_date <= end_date
	// and if the µs are out of order, then there must be an extra ms.
	if (start_micros > (idx_t)end_micros) {
		end_date.micros -= Interval::MICROS_PER_MSEC;
		end_micros += Interval::MICROS_PER_MSEC;
	}

	//	Lunar calendars have uneven numbers of months, so we just diff months, not years
	interval_t result;
	result.months = SubtractField(calendar, CAL_MONTH, end_date);
	result.days = SubtractField(calendar, CAL_DATE, end_date);

	auto hour_diff = SubtractField(calendar, CAL_HOUR_OF_DAY, end_date);
	auto min_diff = SubtractField(calendar, CAL_MINUTE, end_date);
	auto sec_diff = SubtractField(calendar, CAL_SECOND, end_date);
	auto ms_diff = SubtractField(calendar, CAL_MILLISECOND, end_date);
	auto micros_diff = int32_t(ms_diff * Interval::MICROS_PER_MSEC + int64_t(end_micros - start_micros));
	result.micros = Time::FromTime(hour_diff, min_diff, sec_diff, micros_diff).micros;

	return result;
}

struct ICUDateAdd : public ICUDateFunc {
	template <typename TA, typename TR, typename OP>
	static void ExecuteUnary(cxx::ScalarFunction::ExecInput &input) {
		auto &info = input.GetBindData<BindData>();
		TZCalendar calendar(*info.calendar, info.cal_setting);

		//	Subtract argument from current_date (at midnight)
		const auto end_date = CurrentMidnight(calendar.GetCalendar(), input.GetContext());

		ExecuteRows<TR, TA>(input, [&](TA start_date) -> std::optional<TR> {
			return OP::template Operation<timestamp_tz_t, TA, TR>(end_date, start_date, calendar);
		});
	}

	template <typename TA, typename TB, typename TR, typename OP>
	static void ExecuteBinary(cxx::ScalarFunction::ExecInput &input) {
		auto &info = input.GetBindData<BindData>();
		TZCalendar calendar(*info.calendar, info.cal_setting);

		ExecuteRows<TR, TA, TB>(input, [&](TA left, TB right) -> std::optional<TR> {
			return OP::template Operation<TA, TB, TR>(left, right, calendar);
		});
	}

	static void Register(const ICURegistration &reg, cxx::ScalarFunction &function) {
		function.SetBindCallback(ICUBindIntervalMonths);
		// throws for dates that overflow the timestamp range
		function.SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
		reg.extension.Register(function);
	}

	template <typename T>
	static void AddDateAddFunctions(const ICURegistration &reg, const string &name, const string &type) {
		//	temporal + interval
		auto tstz_interval = reg.Scalar(name, {{"left", type}, {"right", "INTERVAL"}}, type);
		tstz_interval.SetExecCallback(ExecuteBinary<T, interval_t, T, ICUCalendarAdd>);
		Register(reg, tstz_interval);

		//	interval + temporal
		auto interval_tstz = reg.Scalar(name, {{"left", "INTERVAL"}, {"right", type}}, type);
		interval_tstz.SetExecCallback(ExecuteBinary<interval_t, T, T, ICUCalendarAdd>);
		Register(reg, interval_tstz);
	}

	template <typename T>
	static void AddDateSubFunction(const ICURegistration &reg, const string &name, const string &type) {
		//	temporal - interval
		auto tstz_interval = reg.Scalar(name, {{"left", type}, {"right", "INTERVAL"}}, type);
		tstz_interval.SetExecCallback(ExecuteBinary<T, interval_t, T, ICUCalendarSub>);
		Register(reg, tstz_interval);
	}

	static void AddDateSubOperators(const ICURegistration &reg, const string &name) {
		AddDateSubFunction<timestamp_tz_t>(reg, name, "TIMESTAMPTZ");
		AddDateSubFunction<timestamp_tz_ns_t>(reg, name, "TIMESTAMPTZ_NS");

		//	temporal - temporal
		auto tstz_tstz = reg.Scalar(name, {{"left", "TIMESTAMPTZ"}, {"right", "TIMESTAMPTZ"}}, "INTERVAL");
		tstz_tstz.SetExecCallback(ExecuteBinary<timestamp_tz_t, timestamp_tz_t, interval_t, ICUCalendarSub>);
		Register(reg, tstz_tstz);
	}

	static void AddDateAgeFunctions(const ICURegistration &reg, const string &name) {
		//	age(temporal, temporal)
		auto binary = reg.Scalar(name, {{"timestamp1", "TIMESTAMPTZ"}, {"timestamp2", "TIMESTAMPTZ"}}, "INTERVAL");
		binary.SetExecCallback(ExecuteBinary<timestamp_tz_t, timestamp_tz_t, interval_t, ICUCalendarAge>);
		Register(reg, binary);

		// age(temporal): the age is taken at the start of the transaction
		auto unary = reg.Scalar(name, {{"timestamp", "TIMESTAMPTZ"}}, "INTERVAL");
		unary.SetExecCallback(ExecuteUnary<timestamp_tz_t, interval_t, ICUCalendarAge>);
		Register(reg, unary);
	}
};

timestamp_tz_t ICUDateFunc::Add(TZCalendar &calendar, timestamp_tz_t timestamp, interval_t interval) {
	return ICUCalendarAdd::Operation<timestamp_tz_t, interval_t, timestamp_tz_t>(timestamp, interval, calendar);
}

timestamp_tz_t ICUDateFunc::Sub(TZCalendar &calendar, timestamp_tz_t timestamp, interval_t interval) {
	return ICUCalendarSub::Operation<timestamp_tz_t, interval_t, timestamp_tz_t>(timestamp, interval, calendar);
}

interval_t ICUDateFunc::Sub(TZCalendar &calendar, timestamp_tz_t end_date, timestamp_tz_t start_date) {
	return ICUCalendarSub::Operation<timestamp_tz_t, timestamp_tz_t, interval_t>(end_date, start_date, calendar);
}

void RegisterICUDateAddFunctions(const ICURegistration &reg) {
	ICUDateAdd::AddDateAddFunctions<timestamp_tz_t>(reg, "+", "TIMESTAMPTZ");
	ICUDateAdd::AddDateAddFunctions<timestamp_tz_ns_t>(reg, "+", "TIMESTAMPTZ_NS");
	ICUDateAdd::AddDateSubOperators(reg, "-");
	ICUDateAdd::AddDateAgeFunctions(reg, "age");
}

} // namespace duckdb_icu
