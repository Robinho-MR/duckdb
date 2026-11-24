#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/query_profiler.hpp"

namespace duckdb {

struct DuckDBProfilingInfoData : public GlobalTableFunctionState {
	vector<QueryProfileRecord> records;
	idx_t offset = 0;
};

static unique_ptr<FunctionData> DuckDBProfilingInfoBind(ClientContext &context, TableFunctionBindInput &input,
                                                        vector<LogicalType> &return_types, vector<Identifier> &names) {
	names.emplace_back("query");
	return_types.emplace_back(LogicalType::VARCHAR);

	names.emplace_back("profile");
	return_types.emplace_back(LogicalType::VARCHAR);
	return nullptr;
}

static unique_ptr<GlobalTableFunctionState> DuckDBProfilingInfoInit(ClientContext &context,
                                                                    TableFunctionInitInput &input) {
	auto result = make_uniq<DuckDBProfilingInfoData>();
	auto &profiler = QueryProfiler::Get(context);
	// profiles collected before profiling was disabled are stale
	if (profiler.IsEnabled()) {
		result->records = profiler.GetPreviousProfiles();
	}
	return std::move(result);
}

static void DuckDBProfilingInfoFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<DuckDBProfilingInfoData>();
	idx_t count = 0;
	while (data.offset < data.records.size() && count < STANDARD_VECTOR_SIZE) {
		auto &record = data.records[data.offset++];
		output.data[0].Append(Value(record.query));
		output.data[1].Append(Value(record.profile));
		count++;
	}
}

void DuckDBProfilingInfoFun::RegisterFunction(BuiltinFunctions &set) {
	TableFunction profiling_info_fun("duckdb_profiling_info", {}, DuckDBProfilingInfoFunction, DuckDBProfilingInfoBind,
	                                 DuckDBProfilingInfoInit);
	// reads the profiles of previous queries
	profiling_info_fun.stability = FunctionStability::VOLATILE;
	set.AddFunction(std::move(profiling_info_fun));
}

} // namespace duckdb
