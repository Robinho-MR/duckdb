// COPY ... FROM for the json, ndjson, jsonl, geojson and geojsonl formats. The columns come from the target table, so
// the file is read like read_json with those columns, as records.

#include "json_scan.hpp"

#include <algorithm>
#include <mutex>

namespace duckdb {
namespace json {

//! The bind data: the scan of the first file, and every file the path expanded to
struct JSONCopyFromBindData {
	JSONScanData data;
	vector<string> files;
};

//! The files being read one after the other, and the vectors of the columns each thread reads
struct JSONCopyFromGlobalState {
	JSONCopyFromGlobalState(Context &context, const JSONCopyFromBindData &bind_data_p, vector<string> names_p,
	                        vector<JSONFeatureColumn> feature_columns_p)
	    : bind_data(bind_data_p), names(std::move(names_p)), feature_columns(std::move(feature_columns_p)) {
		// the first file is opened here, so that errors surface at the start of the scan
		file = OpenFile(context, 0);
		next_file = 1;
	}

	//! Opens the file at `index`
	std::shared_ptr<JSONScanGlobalState> OpenFile(Context &context, idx_t index) {
		auto &data = bind_data.data;
		auto path = bind_data.files[index];
		auto file = Value::Create(context.GetFactory(), varchar_t(path));
		// the format of every file is detected on its own, unless it was given
		auto format = data.options.format;
		auto skip_prefix = data.skip_feature_collection_prefix;
		if (format == JSONFormat::AUTO_DETECT && index != 0) {
			auto record_type = data.options.record_type;
			JSONReader::DetectFormat(context, file, path, data.options, format, record_type, skip_prefix);
		} else if (format == JSONFormat::AUTO_DETECT) {
			format = data.format;
		}
		auto reader = make_uniq<JSONReader>(data.options, path, format, data.options.record_type, skip_prefix);
		reader->Open(context, file, true);
		return std::make_shared<JSONScanGlobalState>(data, std::move(reader), names, feature_columns);
	}

	//! Claims the next batch, moving on to the next file when the current one is exhausted
	bool ClaimBatch(Context &context, JSONReaderScanState &state, std::shared_ptr<JSONScanGlobalState> &current) {
		std::lock_guard<std::mutex> guard(lock);
		while (true) {
			if (!file) {
				if (next_file >= bind_data.files.size()) {
					return false;
				}
				file = OpenFile(context, next_file++);
			}
			if (file->reader->ClaimBatch(state)) {
				current = file;
				return true;
			}
			file.reset();
		}
	}

	double GetProgress() {
		std::lock_guard<std::mutex> guard(lock);
		auto total = static_cast<double>(bind_data.files.size());
		double done = next_file > 0 ? static_cast<double>(next_file - 1) : 0;
		return std::min(100.0, 100.0 * (done + (file ? file->reader->GetProgress() / 100.0 : 1.0)) / total);
	}

	const JSONCopyFromBindData &bind_data;
	vector<string> names;
	vector<JSONFeatureColumn> feature_columns;
	std::mutex lock;
	std::shared_ptr<JSONScanGlobalState> file;
	idx_t next_file = 0;
};

//! The state of a thread reading the file: the batch of the file it is on
struct JSONCopyFromLocalState {
	JSONScanLocalState scan;
	std::shared_ptr<JSONScanGlobalState> current;
	bool claimed = false;
};

//! The files a COPY ... FROM path stands for: the files matching a glob, in sorted order
static vector<string> ExpandCopyPath(Context &context, const string &path) {
	if (path.find_first_of("*?[") == string::npos) {
		return {path};
	}
	auto fs = context.GetFileSystem();
	auto files = fs.Glob(path);
	if (files.empty()) {
		throw IOException("No files found that match the pattern \"%s\"", path);
	}
	std::sort(files.begin(), files.end());
	return files;
}

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
	JSONCopyFromBindData bind_data;
	auto &data = bind_data.data;
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
		// a bare option (e.g. "dateformat") is a flag: only boolean options can be given like that
		const auto type_id = value.GetLogicalType().GetTypeId();
		// a parenthesized list of values is a tuple: the options take one value
		const bool bare =
		    (type_id == LogicalTypeId::BOOLEAN && !IsBooleanOption(name)) || (type_id == LogicalTypeId::TUPLE);
		bool known;
		try {
			known = JSONScan::ParseOption(context, name, value, options);
		} catch (Error &) {
			if (bare) {
				throw BinderException("COPY parameter %s expects a single argument", name);
			}
			throw;
		}
		if (!known) {
			throw BinderException("Unsupported option for COPY FROM: %s", name);
		}
		if (bare) {
			throw BinderException("COPY parameter %s expects a single argument", name);
		}
	}
	if (options.auto_detect_specified && !options.auto_detect && !options.format_specified) {
		options.format = JSONFormat::NEWLINE_DELIMITED;
	}
	bind_data.files = ExpandCopyPath(context, input.GetFilePath());
	data.file_path = bind_data.files[0];
	data.file = Value::Create(context.GetFactory(), varchar_t(data.file_path));

	vector<LogicalType> return_types;
	vector<string> names;
	JSONScan::BindSchema(context, data, return_types, names);
	JSONScan::FinalizeBind(data);
	input.SetCardinality(data.estimated_cardinality.value_or(42), false);
	input.SetBindData<JSONCopyFromBindData>(std::move(bind_data));
}

static void CopyFromInitGlobal(CopyFunction::CopyFromInitGlobalInput &input) {
	auto &bind_data = input.GetBindData<JSONCopyFromBindData>();
	auto &data = bind_data.data;
	// every column of the target table is read
	input.SetGlobalState<JSONCopyFromGlobalState>(input.GetContext(), bind_data, data.key_names, data.feature_columns);
	input.SetMaxThreads(std::max<idx_t>(bind_data.files.size(), 1));
}

static void CopyFromInitLocal(CopyFunction::CopyFromInitLocalInput &input) {
	input.SetLocalState<JSONCopyFromLocalState>();
}

static void CopyFromExec(CopyFunction::CopyFromExecInput &input) {
	auto &gstate = input.GetGlobalState<JSONCopyFromGlobalState>();
	auto &lstate = input.GetLocalState<JSONCopyFromLocalState>();
	auto chunk = input.GetOutputChunk();
	vector<Vector> output;
	for (idx_t i = 0; i < chunk.GetVectorCount(); i++) {
		output.push_back(chunk.GetVector(i));
	}
	while (true) {
		if (!lstate.claimed) {
			if (!gstate.ClaimBatch(input.GetContext(), lstate.scan.scan_state, lstate.current)) {
				return;
			}
			lstate.claimed = true;
		}
		if (JSONScanToVectors(input.GetContext(), *lstate.current, lstate.scan, output) != 0) {
			return;
		}
		// the batch is exhausted: claim the next one
		lstate.claimed = false;
	}
}

static void CopyFromProgress(CopyFunction::CopyFromProgressInput &input) {
	input.SetProgress(input.GetGlobalState<JSONCopyFromGlobalState>().GetProgress());
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
