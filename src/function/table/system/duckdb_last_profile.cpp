#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/query_profiler.hpp"

namespace duckdb {

struct DuckDBLastProfileData : public GlobalTableFunctionState {
	bool finished = false;
};

struct DuckDBLastProfileBindData : public TableFunctionData {
	explicit DuckDBLastProfileBindData(string format_p) : format(std::move(format_p)) {
	}

	string format;
};

static unique_ptr<FunctionData> DuckDBLastProfileBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("profile");
	return_types.emplace_back(LogicalType::VARCHAR);

	string format = "default";
	if (!input.inputs.empty()) {
		if (input.inputs[0].IsNull()) {
			throw InvalidInputException("duckdb_last_profile: the format cannot be NULL");
		}
		format = input.inputs[0].GetValue<string>();
	}
	return make_uniq<DuckDBLastProfileBindData>(std::move(format));
}

static unique_ptr<GlobalTableFunctionState> DuckDBLastProfileInit(ClientContext &context,
                                                                  TableFunctionInitInput &input) {
	return make_uniq<DuckDBLastProfileData>();
}

static void DuckDBLastProfileFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBLastProfileData>();
	if (data.finished) {
		return;
	}
	data.finished = true;
	auto &bind_data = data_p.bind_data->Cast<DuckDBLastProfileBindData>();
	auto &profiler = QueryProfiler::Get(context).GetPreviousProfile();
	auto profile = profiler.ToString(ProfilerPrintFormat(bind_data.format));
	output.data[0].Append(Value(std::move(profile)));
}

void DuckDBLastProfileFun::RegisterFunction(BuiltinFunctions &set) {
	TableFunctionSet last_profile_set("duckdb_last_profile");
	// reads the profile of the previous query, whatever it is
	TableFunction no_format({}, DuckDBLastProfileFunction, DuckDBLastProfileBind, DuckDBLastProfileInit);
	no_format.stability = FunctionStability::VOLATILE;
	last_profile_set.AddFunction(no_format);

	TableFunction with_format({LogicalType::VARCHAR}, DuckDBLastProfileFunction, DuckDBLastProfileBind,
	                          DuckDBLastProfileInit);
	with_format.stability = FunctionStability::VOLATILE;
	last_profile_set.AddFunction(with_format);

	set.AddFunction(std::move(last_profile_set));
}

} // namespace duckdb
