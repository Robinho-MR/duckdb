#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: MultiFileFunction schema sampling. A single-file function whose columns are named by the
// file it reads ("a_b.kv" has the columns a and b) is wrapped, to test how the schemas of several files are
// combined: the number of sampled files, the combine schema callback, and the expected schema of the binds.
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;
namespace fs = std::filesystem;

struct KvBind {
	//! The columns the file has
	std::vector<std::string> file_columns;
	//! The columns the call produces
	std::vector<std::string> columns;

	bool operator==(const KvBind &other) const {
		return file_columns == other.file_columns && columns == other.columns;
	}
};

struct KvScan {
	bool done = false;
};

// What the callbacks saw, latched for the test to assert on afterwards. A callback must not use REQUIRE: it would
// throw through the callback boundary into the engine.
std::atomic<int> binds_without_expected {0};
std::atomic<int> binds_with_expected {0};
std::atomic<int> binds_with_expected_data {0};
std::atomic<int> combine_calls {0};
std::atomic<idx_t> combine_files {0};
std::atomic<idx_t> combine_by_name_columns {0};
std::atomic<bool> combine_union_by_name {false};

void ResetObserved() {
	binds_without_expected = 0;
	binds_with_expected = 0;
	binds_with_expected_data = 0;
	combine_calls = 0;
	combine_files = 0;
	combine_by_name_columns = 0;
	combine_union_by_name = false;
}

std::vector<std::string> ColumnsOfFile(const std::string &path) {
	auto name = fs::path(path).stem().string();
	std::vector<std::string> columns;
	size_t start = 0;
	while (start <= name.size()) {
		auto end = name.find('_', start);
		if (end == std::string::npos) {
			end = name.size();
		}
		columns.push_back(name.substr(start, end - start));
		start = end + 1;
	}
	return columns;
}

void KvBindCallback(TableFunction::BindInput &input) {
	auto file = input.GetConstantArgument(0);
	KvBind bind;
	bind.file_columns = ColumnsOfFile(GetFilePath(file));
	if (input.GetExpectedColumnCount() > 0) {
		binds_with_expected++;
		if (input.TryGetExpectedBindData<KvBind>()) {
			binds_with_expected_data++;
		}
		for (idx_t i = 0; i < input.GetExpectedColumnCount(); i++) {
			bind.columns.push_back(input.GetExpectedColumnName(i));
		}
	} else {
		binds_without_expected++;
		bind.columns = bind.file_columns;
	}
	auto bigint = input.GetContext().GetFactory().ParseType("BIGINT");
	for (auto &column : bind.columns) {
		input.AddResultColumn(column, bigint);
	}
	input.SetBindData<KvBind>(std::move(bind));
}

void KvInitGlobal(TableFunction::InitGlobalInput &input) {
	input.SetGlobalState<KvScan>();
}

// One row per file: 1 for the columns the file has, 0 for the others
void KvExec(TableFunction::ExecInput &input) {
	const auto &bind = input.GetBindData<KvBind>();
	auto &scan = input.GetGlobalState<KvScan>();
	auto chunk = input.GetOutputChunk();
	if (scan.done) {
		chunk.GetVector(0).SetSize(0);
		return;
	}
	scan.done = true;
	for (idx_t i = 0; i < input.GetColumnCount(); i++) {
		auto vec = chunk.GetVector(i);
		auto &column = bind.columns[input.GetColumnIndex(i)];
		auto has = std::find(bind.file_columns.begin(), bind.file_columns.end(), column);
		vec.GetDataMutable<int64_t>()[0] = has != bind.file_columns.end() ? 1 : 0;
		vec.SetSize(1);
	}
}

// Combines the columns of the sampled files, sorted by name
void KvCombine(TableFunction::CombineSchemaInput &input) {
	combine_calls++;
	combine_files = input.GetFileCount();
	combine_union_by_name = input.GetUnionByName();
	combine_by_name_columns = input.GetCombinedColumnCount();
	std::vector<std::string> columns;
	for (idx_t i = 0; i < input.GetFileCount(); i++) {
		for (auto &column : input.GetBindData<KvBind>(i).file_columns) {
			if (std::find(columns.begin(), columns.end(), column) == columns.end()) {
				columns.push_back(column);
			}
		}
	}
	std::sort(columns.begin(), columns.end());
	auto bigint = input.GetContext().GetFactory().ParseType("BIGINT");
	for (auto &column : columns) {
		input.AddResultColumn(column, bigint);
	}
	input.SetBindData<KvBind>(KvBind {{}, columns});
}

//! A directory of the files a.kv, b.kv and c_d.kv that is removed again
class KvFiles {
public:
	KvFiles() {
		static std::atomic<int> counter {0};
		// processes running the tests concurrently share the temporary directory
		std::random_device random;
		directory = fs::temp_directory_path() / ("cpp_api_kv_" + std::to_string(counter++) + "_" +
		                                         std::to_string(random()) + std::to_string(random()));
		fs::create_directories(directory);
		for (auto name : {"a.kv", "b.kv", "c_d.kv"}) {
			std::ofstream(directory / name) << "x";
		}
	}
	~KvFiles() {
		std::error_code ignored;
		fs::remove_all(directory, ignored);
	}
	std::string Glob() const {
		return (directory / "*.kv").string();
	}

private:
	fs::path directory;
};

void RegisterKv(Connection &conn, bool combine, idx_t sample_files, bool sample_parameter) {
	auto &factory = conn.GetFactory();
	auto single = TableFunction::Create(factory);
	single.SetName("cpp_kv_single");
	single.WithSignature([&](FunctionSignature &sig) {
		sig.AddParameter("file", factory.CreateType(LogicalTypeId::ANY), FunctionParameterKind::POSITIONAL_ONLY);
		sig.AddParameter("sample", factory.ParseType("BIGINT"), Value::CreateNull(factory, factory.ParseType("BIGINT")),
		                 FunctionParameterKind::NAMED_ONLY);
	});
	single.SetBindCallback(KvBindCallback)
	    .SetInitGlobalCallback(KvInitGlobal)
	    .SetExecCallback(KvExec)
	    .SetProjectionPushdown(true);
	if (combine) {
		single.SetCombineSchemaCallback(KvCombine);
	}
	conn.Register(single);

	auto multi = MultiFileFunction::Create(factory);
	multi.SetName("cpp_kv").SetSingleFileFunction("cpp_kv_single").SetFileExtension("kv");
	if (sample_files > 1) {
		multi.SetMaximumSampleFiles(sample_files);
	}
	if (sample_parameter) {
		multi.SetSampleFilesParameter("sample");
	}
	conn.Register(multi);
}

std::vector<int64_t> CollectKvRow(QueryResult result, idx_t columns) {
	std::vector<int64_t> out;
	while (auto chunk = result.FetchChunk()) {
		for (idx_t row = 0; row < chunk.GetRowCount(); row++) {
			for (idx_t c = 0; c < columns; c++) {
				auto view = chunk.GetVector(c).GetView();
				REQUIRE(view.IsValid(row));
				out.push_back(view.Data<int64_t>()[view.SelAt(row)]);
			}
		}
	}
	return out;
}

} // namespace

TEST_CASE("Stable C++API: multi-file function binds the other files against the first by default",
          "[cpp_api][multi_file]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterKv(conn, false, 1, false);
	KvFiles files;
	ResetObserved();

	// only the first file is sampled: the columns of the other files are not part of the result
	REQUIRE(CollectKvRow(conn.Execute("SELECT a FROM cpp_kv('" + files.Glob() + "') ORDER BY a"), 1) ==
	        std::vector<int64_t> {0, 0, 1});
	REQUIRE(binds_without_expected >= 1);
	REQUIRE(binds_with_expected >= 2);
	REQUIRE(binds_with_expected_data == binds_with_expected);
	REQUIRE(combine_calls == 0);
}

TEST_CASE("Stable C++API: multi-file function combines the schemas of the sampled files", "[cpp_api][multi_file]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterKv(conn, true, 8, false);
	KvFiles files;
	ResetObserved();

	// the combined columns, sorted by the callback, are the schema every file is read with
	REQUIRE(CollectKvRow(conn.Execute("SELECT a, b, c, d FROM cpp_kv('" + files.Glob() + "') ORDER BY a DESC, b DESC"),
	                     4) == std::vector<int64_t> {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1});
	REQUIRE(combine_calls == 1);
	REQUIRE(combine_files == 3);
	REQUIRE(combine_by_name_columns == 4);
	REQUIRE_FALSE(combine_union_by_name);
	// the files are read with the combined schema and the bind data of the callback
	REQUIRE(binds_with_expected >= 3);
	REQUIRE(binds_with_expected_data == binds_with_expected);

	ResetObserved();
	REQUIRE(CollectKvRow(conn.Execute("SELECT a FROM cpp_kv('" + files.Glob() + "', union_by_name = true) ORDER BY a"),
	                     1) == std::vector<int64_t> {0, 0, 1});
	REQUIRE(combine_calls == 1);
	REQUIRE(combine_files == 3);
	REQUIRE(combine_union_by_name);
}

TEST_CASE("Stable C++API: multi-file function samples the files its parameter names", "[cpp_api][multi_file]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterKv(conn, true, 1, true);
	KvFiles files;

	// without the parameter only the first file is sampled: there is nothing to combine
	ResetObserved();
	REQUIRE(CollectKvRow(conn.Execute("SELECT * FROM cpp_kv('" + files.Glob() + "')"), 1).size() == 3);
	REQUIRE(combine_calls == 0);

	ResetObserved();
	REQUIRE(CollectKvRow(conn.Execute("SELECT b FROM cpp_kv('" + files.Glob() + "', sample = 2) ORDER BY b"), 1) ==
	        std::vector<int64_t> {0, 0, 1});
	REQUIRE(combine_calls == 1);
	REQUIRE(combine_files == 2);

	ResetObserved();
	REQUIRE(CollectKvRow(conn.Execute("SELECT d FROM cpp_kv('" + files.Glob() + "', sample = -1) ORDER BY d"), 1) ==
	        std::vector<int64_t> {0, 0, 1});
	REQUIRE(combine_calls == 1);
	REQUIRE(combine_files == 3);
}

TEST_CASE("Stable C++API: multi-file function refusals", "[cpp_api][multi_file]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto multi = MultiFileFunction::Create(conn.GetFactory());
	REQUIRE_THROWS_MATCHES(multi.SetMaximumSampleFiles(0), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));

	// no expected schema outside of a multi-file scan
	auto single = TableFunction::Create(conn.GetFactory());
	single.SetName("cpp_kv_direct");
	single.WithSignature([&](FunctionSignature &sig) {
		sig.AddParameter("file", conn.GetFactory().CreateType(LogicalTypeId::ANY),
		                 FunctionParameterKind::POSITIONAL_ONLY);
	});
	single.SetBindCallback(KvBindCallback).SetInitGlobalCallback(KvInitGlobal).SetExecCallback(KvExec);
	conn.Register(single);
	ResetObserved();
	REQUIRE(CollectKvRow(conn.Execute("SELECT * FROM cpp_kv_direct('x_y.kv')"), 2) == std::vector<int64_t> {1, 1});
	REQUIRE(binds_without_expected == 1);
	REQUIRE(binds_with_expected == 0);
}
