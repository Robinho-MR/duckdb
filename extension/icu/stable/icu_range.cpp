#include "icu_stable_areas.hpp"
#include "icu_stable_common.hpp"
#include "icu_bridge_range.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <vector>

namespace icu_stable {

namespace {

//! The direction and bounds of a range
struct RangeSpec {
	int64_t start = 0;
	int64_t end = 0;
	cxx::interval_t step {0, 0, 0};
	bool inclusive = false;
	bool ascending = true;
	//! The range is known to hold no values
	bool empty = false;

	//! Whether the value lies beyond the end bound
	bool Finished(int64_t value) const {
		if (ascending) {
			return inclusive ? value > end : value >= end;
		}
		return inclusive ? value < end : value <= end;
	}
};

//! Validates the arguments of a range. Throws for infinite bounds, a zero step or a step of mixed signs.
RangeSpec MakeSpec(int64_t start, int64_t end, cxx::interval_t step, bool inclusive) {
	RangeSpec spec;
	spec.start = start;
	spec.end = end;
	spec.step = step;
	spec.inclusive = inclusive;
	const bool is_positive = step.months > 0 || step.days > 0 || step.micros > 0;
	const bool is_negative = step.months < 0 || step.days < 0 || step.micros < 0;
	if (!is_positive && !is_negative) {
		throw cxx::Exception(DUCKDB_V2_ERROR_QUERY_BINDER, "interval cannot be 0!");
	}
	if (!IsFinite(start) || !IsFinite(end)) {
		throw cxx::Exception(DUCKDB_V2_ERROR_QUERY_BINDER, "RANGE with infinite bounds is not supported");
	}
	if (is_positive && is_negative) {
		throw cxx::Exception(DUCKDB_V2_ERROR_QUERY_BINDER,
		                     "RANGE with composite interval that has mixed signs is not supported");
	}
	spec.ascending = is_positive;
	spec.empty = is_positive ? start > end : start < end;
	return spec;
}

int64_t Step(icu_bridge::Calendar &calendar, const RangeSpec &spec, int64_t value) {
	int64_t result;
	if (!icu_bridge::RangeAddInterval(calendar, value, spec.step.months, spec.step.days, spec.step.micros, result)) {
		ThrowConversion("ICU date overflows timestamp range");
	}
	return result;
}

//----------------------------------------------------------------------------------------------------------------------
// Table functions
//----------------------------------------------------------------------------------------------------------------------
struct TableBindData {
	Settings settings;
	RangeSpec spec;
	//! A NULL argument makes the range empty
	bool has_null = false;
};

struct TableGlobalState {
	TableGlobalState(int64_t start, int64_t end) : start(start), end(end), current(start) {
	}
	int64_t start;
	int64_t end;
	//! The next value to generate; only touched by the single scanning thread, read for progress
	std::atomic<int64_t> current;
};

template <bool INCLUSIVE>
void TableBind(cxx::TableFunction::BindInput &input) {
	input.AddResultColumn(INCLUSIVE ? "generate_series" : "range",
	                      input.GetContext().GetFactory().ParseType("TIMESTAMP WITH TIME ZONE"));
	TableBindData data;
	data.settings = ReadSettings(input.GetContext());
	cxx::Value values[3] = {input.GetConstantArgument(0), input.GetConstantArgument(1), input.GetConstantArgument(2)};
	for (auto &value : values) {
		if (value.IsNull()) {
			data.has_null = true;
		}
	}
	idx_t cardinality = 0;
	if (!data.has_null) {
		const auto start = values[0].Get<cxx::timestamp_tz_t>().micros;
		const auto end = values[1].Get<cxx::timestamp_tz_t>().micros;
		const auto step = values[2].Get<cxx::interval_t>();
		data.spec = MakeSpec(start, end, step, INCLUSIVE);
		// Estimate the cardinality using microseconds
		int64_t delta;
		if (!step.months && !step.days && step.micros && !__builtin_sub_overflow(end, start, &delta)) {
			cardinality = static_cast<idx_t>(delta / step.micros);
		}
	}
	input.SetCardinality(cardinality, false);
	input.SetBindData<TableBindData>(std::move(data));
}

void TableInit(cxx::TableFunction::InitGlobalInput &input) {
	auto &data = input.GetBindData<TableBindData>();
	input.SetGlobalState<TableGlobalState>(data.spec.start, data.spec.end);
}

void TableProgress(cxx::TableFunction::ProgressInput &input) {
	auto &state = input.GetGlobalState<TableGlobalState>();
	const auto start = static_cast<double>(state.start);
	const auto range = static_cast<double>(state.end) - start;
	if (range == 0) {
		input.SetProgress(0);
		return;
	}
	const auto fraction = (static_cast<double>(state.current.load(std::memory_order_relaxed)) - start) / range;
	input.SetProgress(std::max(std::min(fraction, 1.0), 0.0));
}

void TableExec(cxx::TableFunction::ExecInput &input) {
	auto &data = input.GetBindData<TableBindData>();
	auto &state = input.GetGlobalState<TableGlobalState>();
	auto chunk = input.GetOutputChunk();
	auto out = chunk.GetVector(0);
	if (data.has_null || data.spec.empty) {
		out.SetSize(0);
		return;
	}
	auto &calendar = SettingsCalendar(data.settings);
	auto *values = out.GetDataMutable<cxx::timestamp_tz_t>();
	const auto capacity = chunk.GetCapacity();
	auto current = state.current.load(std::memory_order_relaxed);
	idx_t count = 0;
	while (count < capacity && !data.spec.Finished(current)) {
		values[count++].micros = current;
		current = Step(calendar, data.spec, current);
	}
	state.current.store(current, std::memory_order_relaxed);
	out.SetSize(count);
}

template <bool INCLUSIVE>
void RegisterTable(cxx::Extension &extension, cxx::Factory &factory, const char *name) {
	const auto timestamp_tz = factory.ParseType("TIMESTAMP WITH TIME ZONE");
	auto function = cxx::TableFunction::Create(factory);
	function.SetName(name)
	    .SetBindCallback(TableBind<INCLUSIVE>)
	    .SetInitGlobalCallback(TableInit)
	    .SetExecCallback(TableExec)
	    .SetProgressCallback(TableProgress);
	function.GetSignature()
	    .AddParameter("start", timestamp_tz, cxx::FunctionParameterKind::POSITIONAL_ONLY)
	    .AddParameter("stop", timestamp_tz, cxx::FunctionParameterKind::POSITIONAL_ONLY)
	    .AddParameter("step", factory.ParseType("INTERVAL"), cxx::FunctionParameterKind::POSITIONAL_ONLY);
	extension.Register(function);
}

//----------------------------------------------------------------------------------------------------------------------
// Scalar list functions
//----------------------------------------------------------------------------------------------------------------------
struct ScalarBindData {
	Settings settings;

	bool operator==(const ScalarBindData &other) const {
		return settings == other.settings;
	}
};

void ScalarBind(cxx::ScalarFunction::BindInput &input) {
	input.SetBindData<ScalarBindData>(ScalarBindData {ReadSettings(input.GetContext())});
}

template <bool INCLUSIVE>
void ScalarExec(cxx::ScalarFunction::ExecInput &input) {
	auto &bind = input.GetBindData<ScalarBindData>();
	auto &calendar = SettingsCalendar(bind.settings);
	const auto row_count = input.GetRowCount();
	const auto starts = input.GetArg(0).GetView();
	const auto ends = input.GetArg(1).GetView();
	const auto steps = input.GetArg(2).GetView();

	// First generate all values, as the size of the child vector must be known up front
	std::vector<int64_t> values;
	std::vector<cxx::list_entry_t> entries(row_count);
	std::vector<bool> valid(row_count, true);
	for (idx_t i = 0; i < row_count; i++) {
		if (!starts.IsValid(i) || !ends.IsValid(i) || !steps.IsValid(i)) {
			valid[i] = false;
			continue;
		}
		const auto start = starts.Data<cxx::timestamp_tz_t>()[starts.SelAt(i)].micros;
		const auto end = ends.Data<cxx::timestamp_tz_t>()[ends.SelAt(i)].micros;
		const auto step = steps.Data<cxx::interval_t>()[steps.SelAt(i)];
		entries[i].offset = values.size();
		const bool is_positive = step.months > 0 || step.days > 0 || step.micros > 0;
		const bool is_negative = step.months < 0 || step.days < 0 || step.micros < 0;
		if (!is_positive && !is_negative) {
			// a zero interval gives an empty list
			continue;
		}
		if (!IsFinite(start) || !IsFinite(end)) {
			throw cxx::InvalidInputException("Interval infinite bounds not supported");
		}
		if (is_positive && is_negative) {
			throw cxx::InvalidInputException("Interval with mix of negative/positive entries not supported");
		}
		auto spec = MakeSpec(start, end, step, INCLUSIVE);
		if (spec.empty) {
			continue;
		}
		int64_t current = start;
		while (!spec.Finished(current)) {
			values.push_back(current);
			if (values.size() - entries[i].offset > std::numeric_limits<uint32_t>::max()) {
				throw cxx::InvalidInputException("Lists larger than 2^32 elements are not supported");
			}
			current = Step(calendar, spec, current);
		}
		entries[i].length = values.size() - entries[i].offset;
	}

	auto result = input.GetResult();
	result.SetSize(row_count);
	auto child = result.GetChild(0);
	child.SetSize(values.size());
	if (!values.empty()) {
		auto *child_data = child.GetDataMutable<cxx::timestamp_tz_t>();
		for (idx_t i = 0; i < values.size(); i++) {
			child_data[i].micros = values[i];
		}
	}
	auto *list_data = result.GetDataMutable<cxx::list_entry_t>();
	for (idx_t i = 0; i < row_count; i++) {
		list_data[i] = entries[i];
		if (!valid[i]) {
			result.SetNull(i);
		}
	}
}

template <bool INCLUSIVE>
void RegisterScalar(cxx::Extension &extension, cxx::Factory &factory, const char *name) {
	auto function = cxx::ScalarFunction::Create(factory);
	function.SetName(name)
	    .SetBindCallback(ScalarBind)
	    .SetExecCallback(ScalarExec<INCLUSIVE>)
	    .SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
	const auto timestamp_tz = factory.ParseType("TIMESTAMP WITH TIME ZONE");
	function.GetSignature()
	    .AddParameter("start", timestamp_tz)
	    .AddParameter("stop", timestamp_tz)
	    .AddParameter("step", factory.ParseType("INTERVAL"))
	    .SetReturnType(factory.ParseType("TIMESTAMP WITH TIME ZONE[]"));
	extension.Register(function);
}

} // namespace

void RegisterRangeFunctions(cxx::Extension &extension, cxx::Factory &factory) {
	RegisterTable<false>(extension, factory, "range");
	RegisterTable<true>(extension, factory, "generate_series");
	RegisterScalar<false>(extension, factory, "range");
	RegisterScalar<true>(extension, factory, "generate_series");
}

} // namespace icu_stable
