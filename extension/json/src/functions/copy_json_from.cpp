// COPY ... FROM for the json, ndjson, jsonl, geojson and geojsonl formats. The columns come from the target table, so
// the file is read like read_json with those columns, as records.

#include "json_scan.hpp"

namespace duckdb {
namespace json {

//! The state of a thread reading the file: the batch of the file it is on
struct JSONCopyFromLocalState {
	JSONScanLocalState scan;
	bool claimed = false;
};

//! The options that are flags (or take any value, like records)
static bool IsBooleanOption(const string &name) {
	static const char *const OPTIONS[] = {"ignore_errors",
	                                      "auto_detect",
	                                      "geojson",
	                                      "array",
	                                      "records",
	                                      "columns",
	                                      "case_insensitive_keys",
	                                      "convert_strings_to_integers"};
	for (auto option : OPTIONS) {
		if (name == option) {
			return true;
		}
	}
	return false;
}

static void CopyFromBind(CopyFunction::CopyFromBindInput &input) {
	auto &context = input.GetContext();
	JSONScanData data;
	auto &options = data.options;
	options.type = JSONScanType::READ_JSON;
	options.format = JSONFormat::AUTO_DETECT;
	options.record_type = JSONRecordType::RECORDS;
	options.auto_detect = false;
	for (idx_t i = 0; i < input.GetColumnCount(); i++) {
		options.name_list.push_back(input.GetColumnName(i));
		options.sql_type_list.push_back(input.GetColumnType(i));
	}
	for (idx_t i = 0; i < input.GetOptionCount(); i++) {
		auto name = StringUtil::Lower(input.GetOptionName(i));
		auto value = input.GetOptionValue(i);
		if (value.GetLogicalType().GetTypeId() == LogicalTypeId::BOOLEAN && !IsBooleanOption(name)) {
			// a bare option (e.g. "dateformat") is a flag: only boolean options can be given like that
			throw BinderException("COPY parameter %s expects a single argument", name);
		}
		if (!JSONScan::ParseOption(context, name, value, options)) {
			throw NotImplementedException("Unimplemented option %s", name);
		}
	}
	if (options.auto_detect_specified && !options.auto_detect && !options.format_specified) {
		options.format = JSONFormat::NEWLINE_DELIMITED;
	}
	data.file_path = input.GetFilePath();
	data.file = Value::Create(context.GetFactory(), varchar_t(data.file_path));

	vector<LogicalType> return_types;
	vector<string> names;
	JSONScan::BindSchema(context, data, return_types, names);
	JSONScan::FinalizeBind(data);
	input.SetCardinality(data.estimated_cardinality.value_or(42), false);
	input.SetBindData<JSONScanData>(std::move(data));
}

static void CopyFromInitGlobal(CopyFunction::CopyFromInitGlobalInput &input) {
	auto &data = input.GetBindData<JSONScanData>();
	auto reader = make_uniq<JSONReader>(data.options, data.file_path, data.format, data.options.record_type,
	                                    data.skip_feature_collection_prefix);
	reader->Open(input.GetContext(), *data.file, true);
	input.SetMaxThreads(std::max<idx_t>(reader->BatchCount(), 1));
	// every column of the target table is read
	input.SetGlobalState<JSONScanGlobalState>(data, std::move(reader), data.key_names, data.feature_columns);
}

static void CopyFromInitLocal(CopyFunction::CopyFromInitLocalInput &input) {
	input.SetLocalState<JSONCopyFromLocalState>();
}

static void CopyFromExec(CopyFunction::CopyFromExecInput &input) {
	auto &gstate = input.GetGlobalState<JSONScanGlobalState>();
	auto &lstate = input.GetLocalState<JSONCopyFromLocalState>();
	auto chunk = input.GetOutputChunk();
	vector<Vector> output;
	for (idx_t i = 0; i < chunk.GetVectorCount(); i++) {
		output.push_back(chunk.GetVector(i));
	}
	while (true) {
		if (!lstate.claimed) {
			if (!gstate.reader->ClaimBatch(lstate.scan.scan_state)) {
				return;
			}
			lstate.claimed = true;
		}
		if (JSONScanToVectors(input.GetContext(), gstate, lstate.scan, output) != 0) {
			return;
		}
		// the batch is exhausted: claim the next one
		lstate.claimed = false;
	}
}

static void CopyFromProgress(CopyFunction::CopyFromProgressInput &input) {
	input.SetProgress(input.GetGlobalState<JSONScanGlobalState>().reader->GetProgress());
}

void SetJSONCopyFromCallbacks(CopyFunction &function, bool geojson) {
	// GeoJSON is recognised from the file name, like read_json does
	(void)geojson;
	function.SetCopyFromBindCallback(CopyFromBind);
	function.SetCopyFromInitGlobalCallback(CopyFromInitGlobal);
	function.SetCopyFromInitLocalCallback(CopyFromInitLocal);
	function.SetCopyFromExecCallback(CopyFromExec);
	function.SetCopyFromProgressCallback(CopyFromProgress);
}

} // namespace json
} // namespace duckdb
