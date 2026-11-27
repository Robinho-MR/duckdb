#include "icu-datefunc.hpp"
#include "icu-vector.hpp"

namespace duckdb_icu {

struct ICUTableRange {
	struct ICURangeBindData {
		ICURangeBindData(cxx::Context &context) {
			ICUDateFunc::ReadSettings(context, tz_setting, cal_setting);
			auto tz = TimeZone::TryCreate(tz_setting);
			calendar = Calendar::TryCreate(cal_setting, tz ? std::move(tz) : TimeZone::TryCreate("UTC"));
			if (!calendar) {
				throw InternalException("Unable to create calendar.");
			}
		}

		string tz_setting;
		string cal_setting;
		CalendarPtr calendar;
	};

	//! Where the range of the current input row is
	struct ICURangeLocalState {
		bool initialized_row = false;
		idx_t current_input_row = 0;
		timestamp_tz_t current_state;
		timestamp_tz_t start;
		timestamp_tz_t end;
		interval_t increment;
		bool inclusive_bound = false;
		bool greater_than_check = false;
		bool empty_range = false;

		bool Finished(timestamp_tz_t current_value) const {
			if (greater_than_check) {
				if (inclusive_bound) {
					return current_value.micros > end.micros;
				} else {
					return current_value.micros >= end.micros;
				}
			} else {
				if (inclusive_bound) {
					return current_value.micros < end.micros;
				} else {
					return current_value.micros <= end.micros;
				}
			}
		}
	};

	template <bool GENERATE_SERIES>
	static void GenerateRangeDateTimeParameters(cxx::DataChunk &input, idx_t row_id, ICURangeLocalState &result) {
		// the local state is reused for every input row
		result.empty_range = false;
		ArgReader<timestamp_tz_t> starts(input.GetVector(0));
		ArgReader<timestamp_tz_t> ends(input.GetVector(1));
		ArgReader<interval_t> increments(input.GetVector(2));
		if (!starts.IsValid(row_id) || !ends.IsValid(row_id) || !increments.IsValid(row_id)) {
			result.start = timestamp_tz_t {0};
			result.end = timestamp_tz_t {0};
			result.increment = Interval::Make(0, 0, 0);
			result.greater_than_check = true;
			result.inclusive_bound = false;
			return;
		}

		result.start = starts.Get(row_id);
		result.end = ends.Get(row_id);
		result.increment = increments.Get(row_id);

		// Infinities either cause errors or infinite loops, so just ban them
		if (!IsFinite(result.start) || !IsFinite(result.end)) {
			throw BinderException("RANGE with infinite bounds is not supported");
		}

		if (result.increment.months == 0 && result.increment.days == 0 && result.increment.micros == 0) {
			throw BinderException("interval cannot be 0!");
		}
		// all elements should point in the same direction
		if (result.increment.months > 0 || result.increment.days > 0 || result.increment.micros > 0) {
			if (result.increment.months < 0 || result.increment.days < 0 || result.increment.micros < 0) {
				throw BinderException("RANGE with composite interval that has mixed signs is not supported");
			}
			result.greater_than_check = true;
			if (result.start.micros > result.end.micros) {
				result.empty_range = true;
			}
		} else {
			result.greater_than_check = false;
			if (result.start.micros < result.end.micros) {
				result.empty_range = true;
			}
		}
		result.inclusive_bound = GENERATE_SERIES;
	}

	template <bool GENERATE_SERIES>
	static void Bind(cxx::TableFunction::BindInput &input) {
		auto &context = input.GetContext();
		input.AddResultColumn(GENERATE_SERIES ? "generate_series" : "range",
		                      context.GetFactory().ParseType("TIMESTAMPTZ"));
		input.SetBindData<ICURangeBindData>(context);

		// Estimate the cardinality using micros, when the arguments are constants
		std::optional<cxx::Value> bounds[2];
		std::optional<cxx::Value> step;
		for (idx_t i = 0; i < 3; i++) {
			auto value = input.TryGetConstantArgument(i);
			if (!value || value->IsNull()) {
				return;
			}
			if (i >= 2) {
				step = std::move(value);
			} else {
				bounds[i] = std::move(value);
			}
		}
		const auto increment_value = step->Get<interval_t>();
		int64_t increment = 0;
		if (!Interval::TryGetMicro(increment_value, increment) || !increment) {
			return;
		}
		int64_t delta = 0;
		if (!TrySubtract(bounds[1]->Get<timestamp_tz_t>().micros, bounds[0]->Get<timestamp_tz_t>().micros, delta)) {
			return;
		}
		input.SetCardinality(idx_t(delta / increment), false);
	}

	static void InitLocal(cxx::Operator::InitLocalInput &input) {
		input.SetLocalState<ICURangeLocalState>();
	}

	template <bool GENERATE_SERIES>
	static void Execute(cxx::Operator::ExecuteInput &input) {
		auto &bind_data = input.GetBindData<ICURangeBindData>();
		auto &state = input.GetLocalState<ICURangeLocalState>();
		auto chunk = input.GetInput();
		auto output = input.GetOutput();
		TZCalendar calendar(*bind_data.calendar, bind_data.cal_setting);
		while (true) {
			if (!state.initialized_row) {
				// initialize for the current input row
				if (state.current_input_row >= chunk.GetRowCount()) {
					// ran out of rows
					state.current_input_row = 0;
					state.initialized_row = false;
					input.SetResult(cxx::OperatorResult::NEED_MORE_INPUT);
					return;
				}
				if (state.current_input_row == 0) {
					// the arguments can arrive in any representation, e.g. as a sequence
					for (idx_t col = 0; col < chunk.GetVectorCount(); col++) {
						chunk.GetVector(col).Flatten();
					}
				}
				GenerateRangeDateTimeParameters<GENERATE_SERIES>(chunk, state.current_input_row, state);
				state.initialized_row = true;
				state.current_state = state.start;
			}
			if (state.empty_range) {
				// empty range
				state.current_input_row++;
				state.initialized_row = false;
				input.SetResult(cxx::OperatorResult::HAVE_MORE_OUTPUT);
				return;
			}
			auto vector = output.GetVector(0);
			auto data = vector.GetDataMutable<timestamp_tz_t>();
			const auto capacity = output.GetCapacity();
			idx_t size = 0;
			while (!state.Finished(state.current_state) && size < capacity) {
				data[size++] = state.current_state;
				state.current_state = ICUDateFunc::Add(calendar, state.current_state, state.increment);
			}
			if (size == 0) {
				// move to next row
				state.current_input_row++;
				state.initialized_row = false;
				continue;
			}
			vector.SetSize(size);
			input.SetResult(cxx::OperatorResult::HAVE_MORE_OUTPUT);
			return;
		}
	}

	template <bool GENERATE_SERIES>
	static void AddFunction(const ICURegistration &reg, const string &name) {
		auto op = cxx::Operator::Create(reg.factory);
		op.SetInitLocalCallback(InitLocal).SetExecuteCallback(Execute<GENERATE_SERIES>);

		auto function = cxx::TableFunction::Create(reg.factory);
		function.SetName(name).SetBindCallback(Bind<GENERATE_SERIES>);
		function.SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
		function.GetSignature()
		    .AddParameter("start", reg.Type("TIMESTAMPTZ"), cxx::FunctionParameterKind::POSITIONAL_ONLY)
		    .AddParameter("stop", reg.Type("TIMESTAMPTZ"), cxx::FunctionParameterKind::POSITIONAL_ONLY)
		    .AddParameter("step", reg.Type("INTERVAL"), cxx::FunctionParameterKind::POSITIONAL_ONLY);
		function.SetOperator(op, cxx::TableOperatorInput::ARGUMENTS);
		reg.extension.Register(function);
	}
};

void RegisterICUTableRangeFunctions(const ICURegistration &reg) {
	ICUTableRange::AddFunction<false>(reg, "range");
	// generate_series: similar to range, but inclusive instead of exclusive bounds on the RHS
	ICUTableRange::AddFunction<true>(reg, "generate_series");
}

} // namespace duckdb_icu
