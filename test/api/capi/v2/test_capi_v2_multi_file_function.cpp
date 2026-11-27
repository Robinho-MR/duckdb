#include "test_capi_v2.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// V2 multi-file function tests: how the schemas of several files are sampled and combined. The single-file function
// names its columns after the file it reads ("a.kv" has the column a), so the files carry no content. Callbacks
// avoid Catch assertions: a REQUIRE would throw through the C callback boundary into the engine. Their observations
// are latched into file-scope statics and asserted after the query.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

namespace fs = std::filesystem;

struct KvBind {
	std::string stem;
	std::vector<std::string> columns;
};

void DeleteKvBind(void *ptr) {
	delete static_cast<KvBind *>(ptr);
}

std::atomic<int> binds_with_expected {0};
std::atomic<int> binds_with_expected_data {0};
std::atomic<int> combine_calls {0};
std::atomic<idx_t> combine_files {0};
std::atomic<idx_t> combine_by_name_columns {0};
std::atomic<int> oob_rc_count {0};

void ResetObserved() {
	binds_with_expected = 0;
	binds_with_expected_data = 0;
	combine_calls = 0;
	combine_files = 0;
	combine_by_name_columns = 0;
	oob_rc_count = 0;
}

void KvBindCb(duckdb_v2_function_bind_info_handle info, duckdb_v2_table_function_bind_info_handle result,
              duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	duckdb_v2_value_handle file = nullptr;
	if (duckdb_v2_function_bind_get_arg_value(info, 0, &file, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	const char *path = nullptr;
	idx_t path_length = 0;
	auto rc = duckdb_v2_value_get_varchar(file, &path, &path_length, err);
	std::string stem = rc == DUCKDB_V2_ERROR_NONE ? fs::path(std::string(path, path_length)).stem().string() : "";
	duckdb_v2_value_destroy(&file);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}

	auto bind = new KvBind {stem, {}};
	duckdb_v2_opaque bind_data = {bind, DeleteKvBind, nullptr};
	if (duckdb_v2_function_bind_set_bind_data(info, &bind_data, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}

	idx_t expected_count = 0;
	if (duckdb_v2_table_function_bind_get_expected_column_count(result, &expected_count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	void *expected_data = nullptr;
	if (duckdb_v2_table_function_bind_get_expected_bind_data(result, &expected_data, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	if (expected_count == 0) {
		bind->columns.push_back(stem);
	} else {
		binds_with_expected++;
		if (expected_data) {
			binds_with_expected_data++;
		}
		for (idx_t i = 0; i < expected_count; i++) {
			const char *name = nullptr;
			idx_t name_length = 0;
			if (duckdb_v2_table_function_bind_get_expected_column_name(result, i, &name, &name_length, err) !=
			    DUCKDB_V2_ERROR_NONE) {
				return;
			}
			bind->columns.emplace_back(name, name_length);
		}
		duckdb_v2_logical_type_handle type = nullptr;
		if (duckdb_v2_table_function_bind_get_expected_column_type(result, 0, &type, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		duckdb_v2_logical_type_destroy(&type);
		// an index past the expected columns is refused
		const char *name = nullptr;
		idx_t name_length = 0;
		if (duckdb_v2_table_function_bind_get_expected_column_name(result, expected_count, &name, &name_length,
		                                                           nullptr) != DUCKDB_V2_ERROR_NONE) {
			oob_rc_count++;
		}
		if (duckdb_v2_table_function_bind_get_expected_column_type(result, expected_count, &type, nullptr) !=
		    DUCKDB_V2_ERROR_NONE) {
			oob_rc_count++;
		}
	}

	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	duckdb_v2_logical_type_handle bigint = nullptr;
	if (duckdb_v2_factory_create_type_from_id(factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, nullptr, nullptr, nullptr, 0,
	                                          &bigint, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	for (auto &column : bind->columns) {
		if (duckdb_v2_table_function_bind_add_result_column(result, column.data(), column.size(), bigint, err) !=
		    DUCKDB_V2_ERROR_NONE) {
			break;
		}
	}
	duckdb_v2_logical_type_destroy(&bigint);
}

struct KvScan {
	//! The threads scanning a file share this state: the one that sets it produces the row of the file
	std::atomic<bool> done {false};
};

void DeleteKvScan(void *ptr) {
	delete static_cast<KvScan *>(ptr);
}

void KvInitGlobalCb(duckdb_v2_data_source_init_global_info_handle info, duckdb_v2_context_handle,
                    duckdb_v2_error_info_handle *err) {
	duckdb_v2_opaque state = {new KvScan {}, DeleteKvScan, nullptr};
	duckdb_v2_data_source_init_global_set_global_state(info, &state, err);
}

// One row per file: 1 for the column named after the file, 0 for the others
void KvExecCb(duckdb_v2_data_source_exec_info_handle info, duckdb_v2_context_handle, duckdb_v2_error_info_handle *err) {
	void *bind_ptr = nullptr;
	void *global_ptr = nullptr;
	duckdb_v2_data_chunk_handle chunk = nullptr;
	if (duckdb_v2_data_source_exec_get_bind_data(info, &bind_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_data_source_exec_get_global_state(info, &global_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_data_source_exec_get_output_chunk(info, &chunk, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &bind = *static_cast<KvBind *>(bind_ptr);
	auto &scan = *static_cast<KvScan *>(global_ptr);
	const bool emit = !scan.done.exchange(true);
	// the chunk holds a vector per column the query uses
	idx_t column_count = 0;
	if (duckdb_v2_data_source_exec_get_column_count(info, &column_count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	for (idx_t i = 0; i < column_count; i++) {
		duckdb_v2_vector_handle vec = nullptr;
		void *raw = nullptr;
		idx_t column = 0;
		if (duckdb_v2_data_source_exec_get_column_index(info, i, &column, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_data_chunk_get_vector(chunk, i, &vec, err) != DUCKDB_V2_ERROR_NONE ||
		    duckdb_v2_vector_get_data_mutable(vec, &raw, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		if (emit) {
			static_cast<int64_t *>(raw)[0] = bind.columns[column] == bind.stem ? 1 : 0;
		}
		if (duckdb_v2_vector_set_size(vec, emit ? 1 : 0, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
	}
}

// Combines the columns of the sampled files in the order of the files
void KvCombineCb(duckdb_v2_table_function_combine_schema_info_handle info, duckdb_v2_context_handle context,
                 duckdb_v2_error_info_handle *err) {
	idx_t file_count = 0;
	idx_t by_name_columns = 0;
	if (duckdb_v2_table_function_combine_schema_get_file_count(info, &file_count, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_table_function_combine_schema_get_combined_column_count(info, &by_name_columns, err) !=
	        DUCKDB_V2_ERROR_NONE) {
		return;
	}
	combine_calls++;
	combine_files = file_count;
	combine_by_name_columns = by_name_columns;

	// an index past the last file or combined column is refused
	void *oob_data = nullptr;
	if (duckdb_v2_table_function_combine_schema_get_bind_data(info, file_count, &oob_data, nullptr) !=
	    DUCKDB_V2_ERROR_NONE) {
		oob_rc_count++;
	}
	const char *oob_name = nullptr;
	idx_t oob_name_length = 0;
	if (duckdb_v2_table_function_combine_schema_get_combined_column_name(
	        info, by_name_columns, &oob_name, &oob_name_length, nullptr) != DUCKDB_V2_ERROR_NONE) {
		oob_rc_count++;
	}
	duckdb_v2_logical_type_handle oob_type = nullptr;
	if (duckdb_v2_table_function_combine_schema_get_combined_column_type(info, by_name_columns, &oob_type, nullptr) !=
	    DUCKDB_V2_ERROR_NONE) {
		oob_rc_count++;
	}

	auto combined = new KvBind {"", {}};
	duckdb_v2_opaque bind_data = {combined, DeleteKvBind, nullptr};
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	duckdb_v2_logical_type_handle bigint = nullptr;
	if (duckdb_v2_factory_create_type_from_id(factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT, nullptr, nullptr, nullptr, 0,
	                                          &bigint, err) != DUCKDB_V2_ERROR_NONE) {
		delete combined;
		return;
	}
	for (idx_t i = 0; i < file_count; i++) {
		void *file_data = nullptr;
		if (duckdb_v2_table_function_combine_schema_get_bind_data(info, i, &file_data, err) != DUCKDB_V2_ERROR_NONE) {
			break;
		}
		auto &stem = static_cast<KvBind *>(file_data)->stem;
		if (std::find(combined->columns.begin(), combined->columns.end(), stem) != combined->columns.end()) {
			continue;
		}
		combined->columns.push_back(stem);
		if (duckdb_v2_table_function_combine_schema_add_result_column(info, stem.data(), stem.size(), bigint, err) !=
		    DUCKDB_V2_ERROR_NONE) {
			break;
		}
	}
	duckdb_v2_logical_type_destroy(&bigint);
	duckdb_v2_table_function_combine_schema_set_bind_data(info, &bind_data, err);
}

class KvFiles {
public:
	KvFiles() {
		static std::atomic<int> counter {0};
		// processes running the tests concurrently share the temporary directory
		std::random_device random;
		directory = fs::temp_directory_path() / ("capi_v2_kv_" + std::to_string(counter++) + "_" +
		                                         std::to_string(random()) + std::to_string(random()));
		fs::create_directories(directory);
		for (auto name : {"a.kv", "b.kv", "c.kv"}) {
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

int64_t MultiFileQueryI64(duckdb_v2_connection_handle conn, const std::string &sql) {
	duckdb_v2_result_handle result = nullptr;
	duckdb_v2_error_info_handle err = nullptr;
	auto rc = Query(conn, sql, &result, &err);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		const char *text = nullptr;
		idx_t text_length = 0;
		duckdb_v2_error_info_get_text(err, &text, &text_length);
		FAIL(std::string(text, text_length));
	}
	auto chunk = StepChunk(result);
	REQUIRE(chunk != nullptr);
	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
	duckdb_v2_vector_view view {};
	duckdb_v2_vector_get_view(vec, &view, nullptr);
	auto out = static_cast<const int64_t *>(view.data)[SelAt(view.sel, 0)];
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&result);
	return out;
}

// Registers c_kv, a multi-file function over a single-file function that names its column after its file
void RegisterKv(EnvFixture &fx, bool combine, idx_t sample_files) {
	auto varchar = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	duckdb_v2_table_function_handle single = nullptr;
	REQUIRE(duckdb_v2_table_function_create(fx.factory, &single, nullptr) == DUCKDB_V2_ERROR_NONE);
	std::string_view single_name = "c_kv_single";
	REQUIRE(duckdb_v2_table_function_set_name(single, single_name.data(), single_name.size(), nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	duckdb_v2_function_signature_handle sig = nullptr;
	REQUIRE(duckdb_v2_table_function_get_signature(single, &sig, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, "file", 4, varchar, nullptr,
	                                                   DUCKDB_V2_FUNCTION_PARAMETER_KIND_STANDARD,
	                                                   nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_bind_callback(single, KvBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_data_source_handle source = nullptr;
	REQUIRE(duckdb_v2_data_source_create(fx.factory, &source, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_data_source_set_init_global_callback(source, KvInitGlobalCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_data_source_set_exec_callback(source, KvExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_data_source(single, source, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_data_source_destroy(&source);
	// a multi-file function needs the columns a query uses, and its virtual columns, to be told apart
	REQUIRE(duckdb_v2_table_function_set_projection_pushdown(single, true, nullptr) == DUCKDB_V2_ERROR_NONE);
	if (combine) {
		REQUIRE(duckdb_v2_table_function_set_combine_schema_callback(single, KvCombineCb, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
	}
	REQUIRE(duckdb_v2_connection_register_table_function(fx.conn, single, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&single);
	duckdb_v2_logical_type_destroy(&varchar);

	duckdb_v2_multi_file_function_handle multi = nullptr;
	REQUIRE(duckdb_v2_multi_file_function_create(fx.factory, &multi, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_multi_file_function_set_name(multi, "c_kv", 4, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_multi_file_function_set_single_file_function(multi, single_name.data(), single_name.size(),
	                                                               nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_multi_file_function_set_file_extension(multi, "kv", 2, nullptr) == DUCKDB_V2_ERROR_NONE);
	if (sample_files > 1) {
		REQUIRE(duckdb_v2_multi_file_function_set_maximum_sample_files(multi, sample_files, nullptr) ==
		        DUCKDB_V2_ERROR_NONE);
	}
	REQUIRE(duckdb_v2_connection_register_multi_file_function(fx.conn, multi, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_multi_file_function_destroy(&multi);
}

} // namespace

TEST_CASE("V2 multi-file: the other files are bound against the schema of the first by default",
          "[capi_v2][multi_file_function]") {
	EnvFixture fx;
	RegisterKv(fx, false, 1);
	KvFiles files;
	ResetObserved();

	// only the first file is sampled: the columns of the other files are not part of the result
	REQUIRE(MultiFileQueryI64(fx.conn, "SELECT count(*) FROM c_kv('" + files.Glob() + "')") == 3);
	REQUIRE(MultiFileQueryI64(fx.conn, "SELECT sum(a)::BIGINT FROM c_kv('" + files.Glob() + "')") == 1);
	REQUIRE(binds_with_expected >= 2);
	REQUIRE(binds_with_expected_data == binds_with_expected);
	REQUIRE(oob_rc_count >= 4);
	REQUIRE(combine_calls == 0);
}

TEST_CASE("V2 multi-file: the combine schema callback declares the schema of the scan",
          "[capi_v2][multi_file_function]") {
	EnvFixture fx;
	RegisterKv(fx, true, 8);
	KvFiles files;
	ResetObserved();

	REQUIRE(MultiFileQueryI64(fx.conn, "SELECT sum(a + b + c)::BIGINT FROM c_kv('" + files.Glob() + "')") == 3);
	REQUIRE(MultiFileQueryI64(fx.conn, "SELECT sum(c)::BIGINT FROM c_kv('" + files.Glob() + "')") == 1);
	REQUIRE(combine_calls >= 1);
	REQUIRE(combine_files == 3);
	REQUIRE(combine_by_name_columns == 3);
	REQUIRE(binds_with_expected >= 3);
	REQUIRE(binds_with_expected_data == binds_with_expected);
	// the out of bounds accessors were refused in the combine callback and the binds
	REQUIRE(oob_rc_count >= 3);
}

TEST_CASE("V2 multi-file: sampling setters and argument validation", "[capi_v2][multi_file_function]") {
	EnvFixture fx;
	duckdb_v2_multi_file_function_handle multi = nullptr;
	REQUIRE(duckdb_v2_multi_file_function_create(fx.factory, &multi, nullptr) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(duckdb_v2_multi_file_function_set_maximum_sample_files(multi, 0, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_multi_file_function_set_maximum_sample_files(nullptr, 4, nullptr) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_multi_file_function_set_maximum_sample_files(multi, 4, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_multi_file_function_set_sample_files_parameter(multi, "maximum_sample_files", 20, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_multi_file_function_set_sample_files_parameter(multi, nullptr, 3, nullptr) !=
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_multi_file_function_set_sample_files_parameter(nullptr, "x", 1, nullptr) != DUCKDB_V2_ERROR_NONE);
	duckdb_v2_multi_file_function_destroy(&multi);

	duckdb_v2_table_function_handle function = nullptr;
	REQUIRE(duckdb_v2_table_function_create(fx.factory, &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_combine_schema_callback(nullptr, KvCombineCb, nullptr) !=
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_set_combine_schema_callback(function, KvCombineCb, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	// clearing the callback is allowed
	REQUIRE(duckdb_v2_table_function_set_combine_schema_callback(function, nullptr, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_table_function_destroy(&function);

	// null handles and out pointers of the accessors
	idx_t count = 0;
	void *data = nullptr;
	REQUIRE(duckdb_v2_table_function_bind_get_expected_column_count(nullptr, &count, nullptr) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_bind_get_expected_bind_data(nullptr, &data, nullptr) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_combine_schema_get_file_count(nullptr, &count, nullptr) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_combine_schema_get_user_data(nullptr, &data, nullptr) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_combine_schema_get_bind_data(nullptr, 0, &data, nullptr) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_table_function_combine_schema_set_bind_data(nullptr, nullptr, nullptr) != DUCKDB_V2_ERROR_NONE);
}

} // namespace test_capi_v2
