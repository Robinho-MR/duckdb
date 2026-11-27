#include "icu-datefunc.hpp"
#include "icu-vector.hpp"

namespace duckdb_icu {

//! How often a long loop checks whether the query was interrupted
static constexpr idx_t INTERRUPT_CHECK_INTERVAL = 2048;

struct ICUListRange : public ICUDateFunc {
	template <bool INCLUSIVE_BOUND>
	static uint64_t ListLength(cxx::Context &context, timestamp_tz_t start_value, timestamp_tz_t end_value,
	                           interval_t increment_value, TZCalendar &calendar) {
		bool is_positive = increment_value.months > 0 || increment_value.days > 0 || increment_value.micros > 0;
		bool is_negative = increment_value.months < 0 || increment_value.days < 0 || increment_value.micros < 0;
		if (!is_negative && !is_positive) {
			// interval is 0: no result
			return 0;
		}
		// We don't allow infinite bounds because they generate errors or infinite loops
		if (!IsFinite(start_value) || !IsFinite(end_value)) {
			throw InvalidInputException("Interval infinite bounds not supported");
		}

		if (is_negative && is_positive) {
			// we don't allow a mix of
			throw InvalidInputException("Interval with mix of negative/positive entries not supported");
		}
		if (start_value.micros > end_value.micros && is_positive) {
			return 0;
		}
		if (start_value.micros < end_value.micros && is_negative) {
			return 0;
		}
		int64_t total_values = 0;
		auto keep_going = [&]() {
			if (is_negative) {
				return INCLUSIVE_BOUND ? start_value.micros >= end_value.micros : start_value.micros > end_value.micros;
			}
			return INCLUSIVE_BOUND ? start_value.micros <= end_value.micros : start_value.micros < end_value.micros;
		};
		while (keep_going()) {
			start_value = Add(calendar, start_value, increment_value);
			total_values++;
			if (total_values > int64_t(NumericLimits<uint32_t>::Maximum())) {
				throw InvalidInputException("Lists larger than 2^32 elements are not supported");
			}
			// counting the elements can take billions of steps
			if (idx_t(total_values) % INTERRUPT_CHECK_INTERVAL == 0) {
				context.InterruptCheck();
			}
		}
		return uint64_t(total_values);
	}

	template <bool INCLUSIVE_BOUND>
	static void ICUListRangeFunction(cxx::ScalarFunction::ExecInput &input) {
		auto &bind_info = input.GetBindData<BindData>();
		TZCalendar calendar(*bind_info.calendar, bind_info.cal_setting);
		auto &context = input.GetContext();

		ArgReader<timestamp_tz_t> starts(input.GetArg(0));
		ArgReader<timestamp_tz_t> ends(input.GetArg(1));
		ArgReader<interval_t> increments(input.GetArg(2));

		const auto count = input.GetRowCount();
		auto result = input.GetResult();
		ListResultWriter<timestamp_tz_t> writer(result, count);
		for (idx_t i = 0; i < count; i++) {
			if (!starts.IsValid(i) || !ends.IsValid(i) || !increments.IsValid(i)) {
				writer.SetNull(i);
				continue;
			}
			auto range_value = starts.Get(i);
			const auto increment = increments.Get(i);
			const auto length = ListLength<INCLUSIVE_BOUND>(context, range_value, ends.Get(i), increment, calendar);
			writer.StartList(i);
			for (idx_t range_idx = 0; range_idx < length; range_idx++) {
				if (range_idx > 0) {
					range_value = Add(calendar, range_value, increment);
				}
				writer.Append(range_value);
				if ((range_idx + 1) % INTERRUPT_CHECK_INTERVAL == 0) {
					context.InterruptCheck();
				}
			}
		}
		writer.Finish();
	}

	static void AddFunction(const ICURegistration &reg, const string &name, cxx::ScalarFunction::ExecCallback exec) {
		auto function = reg.Scalar(name, {{"start", "TIMESTAMPTZ"}, {"stop", "TIMESTAMPTZ"}, {"step", "INTERVAL"}},
		                           "TIMESTAMPTZ[]");
		function.SetBindCallback(Bind).SetExecCallback(exec);
		// throws for infinite or mixed-sign intervals
		function.SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
		reg.extension.Register(function);
	}
};

void RegisterICUListRangeFunctions(const ICURegistration &reg) {
	ICUListRange::AddFunction(reg, "range", ICUListRange::ICUListRangeFunction<false>);
	// generate_series: similar to range, but inclusive instead of exclusive bounds on the RHS
	ICUListRange::AddFunction(reg, "generate_series", ICUListRange::ICUListRangeFunction<true>);
}

} // namespace duckdb_icu
