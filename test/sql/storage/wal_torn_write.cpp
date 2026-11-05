#include "catch.hpp"
#include "duckdb/common/checksum.hpp"
#include "duckdb/common/file_system.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/local_file_system.hpp"

using namespace duckdb;

static idx_t GetWALFileSize(FileSystem &fs, const string &path) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	return fs.GetFileSize(*handle);
}

static void TruncateWAL(FileSystem &fs, const string &path, idx_t new_size) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE);
	fs.Truncate(*handle, new_size);
}

static void CopyFile(FileSystem &fs, const string &source, const string &target) {
	auto source_handle = fs.OpenFile(source, FileFlags::FILE_FLAGS_READ);
	auto file_size = source_handle->GetFileSize();
	auto contents = duckdb::unique_ptr<data_t[]>(new data_t[file_size]);
	source_handle->Read(QueryContext(), contents.get(), file_size, 0);

	auto target_handle = fs.OpenFile(target, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
	target_handle->Write(QueryContext(), contents.get(), file_size, 0);
}

static pair<idx_t, idx_t> GetWALCommitBoundaries(FileSystem &fs, const string &path) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	auto wal_size = handle->GetFileSize();
	auto wal_contents = duckdb::unique_ptr<data_t[]>(new data_t[wal_size]);
	handle->Read(QueryContext(), wal_contents.get(), wal_size, 0);

	for (idx_t start = 0; start + 2 * sizeof(uint64_t) <= wal_size; start++) {
		idx_t offset = start;
		vector<idx_t> entry_ends;
		while (offset + 2 * sizeof(uint64_t) <= wal_size) {
			uint64_t entry_size;
			uint64_t stored_checksum;
			memcpy(&entry_size, wal_contents.get() + offset, sizeof(uint64_t));
			memcpy(&stored_checksum, wal_contents.get() + offset + sizeof(uint64_t), sizeof(uint64_t));
			offset += 2 * sizeof(uint64_t);
			if (entry_size > wal_size - offset) {
				break;
			}
			// Checksum requires aligned input, but this scan tries every possible byte offset.
			auto entry = duckdb::unique_ptr<data_t[]>(new data_t[entry_size]);
			memcpy(entry.get(), wal_contents.get() + offset, entry_size);
			if (Checksum(entry.get(), entry_size) != stored_checksum) {
				break;
			}
			offset += entry_size;
			entry_ends.push_back(offset);
		}
		if (entry_ends.size() >= 4) {
			// CREATE_TABLE A, WAL_FLUSH, CREATE_TABLE B, WAL_FLUSH
			return {entry_ends[1], entry_ends[3]};
		}
	}
	throw InternalException("Could not find expected WAL entries in torn WAL write test");
}

TEST_CASE("Test torn WAL writes", "[storage][.]") {
	auto config = GetTestConfig();
	duckdb::unique_ptr<QueryResult> result;
	auto storage_database = TestCreatePath("storage_test");
	auto storage_wal = storage_database + ".wal";
	auto source_database = TestCreatePath("storage_test_source");
	auto source_wal = source_database + ".wal";

	LocalFileSystem lfs;
	config->options.checkpoint_wal_size = idx_t(-1);
	config->options.checkpoint_on_shutdown = false;
	config->options.abort_on_wal_failure = false;
	idx_t wal_size_one_table;
	idx_t wal_size_two_table;
	// obtain the size of the WAL when writing one table, and then when writing two tables
	DeleteDatabase(storage_database);
	DeleteDatabase(source_database);
	{
		DuckDB db(source_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE A (a INTEGER);"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE B (a INTEGER);"));
	}
	auto commit_boundaries = GetWALCommitBoundaries(lfs, source_wal);
	wal_size_one_table = commit_boundaries.first;
	wal_size_two_table = commit_boundaries.second;

	// now for all sizes in between these two sizes we have a torn write
	// try all of the possible sizes and truncate the WAL
	for (idx_t i = wal_size_one_table + 1; i + 1 < wal_size_two_table; i++) {
		DeleteDatabase(storage_database);
		CopyFile(lfs, source_database, storage_database);
		CopyFile(lfs, source_wal, storage_wal);
		TruncateWAL(lfs, storage_wal, i);
		{
			// reload and make sure table A is there, and table B is not there
			DuckDB db(storage_database, config.get());
			Connection con(db);
			REQUIRE_NO_FAIL(con.Query("FROM A"));
			REQUIRE_FAIL(con.Query("FROM B"));
		}
	}
	DeleteDatabase(storage_database);
	DeleteDatabase(source_database);
}

TEST_CASE("Test torn WAL writes followed by successful commits", "[storage][.]") {
	auto config = GetTestConfig();
	duckdb::unique_ptr<QueryResult> result;
	auto storage_database = TestCreatePath("storage_test");
	auto storage_wal = storage_database + ".wal";

	LocalFileSystem lfs;
	config->options.checkpoint_wal_size = idx_t(-1);
	config->options.checkpoint_on_shutdown = false;
	config->options.abort_on_wal_failure = false;
	idx_t wal_size_one_table;
	// obtain the size of the WAL when writing one table, and then when writing two tables
	DeleteDatabase(storage_database);
	{
		DuckDB db(storage_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE A (a INTEGER);"));
		wal_size_one_table = GetWALFileSize(lfs, storage_wal);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE B (a INTEGER);"));
	}
	DeleteDatabase(storage_database);

	DeleteDatabase(storage_database);
	{
		DuckDB db(storage_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE A (a INTEGER);"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE B (a INTEGER);"));
	}
	TruncateWAL(lfs, storage_wal, wal_size_one_table + 17);
	{
		// reload and make sure table A is there, and table B is not there
		DuckDB db(storage_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("FROM A"));
		REQUIRE_FAIL(con.Query("FROM B"));

		// create table C
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE C (a INTEGER);"));
	}
	{
		// reload and make sure table A and C are there, and table B is not
		DuckDB db(storage_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("FROM A"));
		REQUIRE_FAIL(con.Query("FROM B"));
		REQUIRE_NO_FAIL(con.Query("FROM C"));
	}
	DeleteDatabase(storage_database);
}

static void FlipWALByte(FileSystem &fs, const string &path, idx_t byte_pos) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_READ);
	auto wal_size = handle->GetFileSize();
	auto wal_contents = duckdb::unique_ptr<data_t[]>(new data_t[wal_size]);

	handle->Read(QueryContext(), wal_contents.get(), wal_size, 0);
	wal_contents[byte_pos]++;

	handle->Write(QueryContext(), wal_contents.get(), wal_size, 0);
}

TEST_CASE("Test WAL checksums", "[storage][.]") {
	auto config = GetTestConfig();
	duckdb::unique_ptr<QueryResult> result;
	auto storage_database = TestCreatePath("wal_checksum");
	auto storage_wal = storage_database + ".wal";
	auto source_database = TestCreatePath("wal_checksum_source");
	auto source_wal = source_database + ".wal";

	LocalFileSystem lfs;
	config->options.checkpoint_wal_size = idx_t(-1);
	config->options.checkpoint_on_shutdown = false;
	config->options.abort_on_wal_failure = false;
	idx_t wal_size_one_table;
	idx_t wal_size_two_table;
	// obtain the size of the WAL when writing one table, and then when writing two tables
	DeleteDatabase(storage_database);
	DeleteDatabase(source_database);
	{
		DuckDB db(source_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE A (a INTEGER);"));
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE B (a INTEGER);"));
	}
	auto commit_boundaries = GetWALCommitBoundaries(lfs, source_wal);
	wal_size_one_table = commit_boundaries.first;
	wal_size_two_table = commit_boundaries.second;

	// now for all sizes in between these two sizes we have a torn write
	// try all of the possible sizes and truncate the WAL
	for (idx_t i = wal_size_one_table + 1; i + 1 < wal_size_two_table; i++) {
		DeleteDatabase(storage_database);
		CopyFile(lfs, source_database, storage_database);
		CopyFile(lfs, source_wal, storage_wal);
		FlipWALByte(lfs, storage_wal, i);
		{
			// flipping a byte in the checksum leads to a DataCorruptionException
			// flipping a byte in the size of a WAL entry leads to a torn write
			// we succeed on either of these cases here
			try {
				DuckDB db(storage_database, config.get());
				Connection con(db);
				REQUIRE_NO_FAIL(con.Query("FROM A"));
				REQUIRE_FAIL(con.Query("FROM B"));
			} catch (std::exception &ex) {
				ErrorData error(ex);
				if (error.Type() == ExceptionType::DATA_CORRUPTION) {
					REQUIRE(1 == 1);
				} else {
					throw;
				}
			}
		}
	}
	DeleteDatabase(storage_database);
	DeleteDatabase(source_database);
}

//! Change the WAL type of the n-th INSERT_TUPLE entry into an unused value and fix up its checksum
static void CorruptInsertEntryType(FileSystem &fs, const string &path, idx_t insert_nr) {
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_READ);
	auto wal_size = handle->GetFileSize();
	auto wal_contents = duckdb::unique_ptr<data_t[]>(new data_t[wal_size]);
	handle->Read(QueryContext(), wal_contents.get(), wal_size, 0);

	// find the chain of framed [size][checksum][payload] entries by trying every start offset
	for (idx_t start = 0; start + 2 * sizeof(uint64_t) <= wal_size; start++) {
		idx_t offset = start;
		idx_t inserts_seen = 0;
		idx_t valid_entries = 0;
		while (offset + 2 * sizeof(uint64_t) <= wal_size) {
			uint64_t entry_size;
			uint64_t stored_checksum;
			memcpy(&entry_size, wal_contents.get() + offset, sizeof(uint64_t));
			memcpy(&stored_checksum, wal_contents.get() + offset + sizeof(uint64_t), sizeof(uint64_t));
			auto payload_offset = offset + 2 * sizeof(uint64_t);
			if (entry_size < 3 || entry_size > wal_size - payload_offset) {
				break;
			}
			auto entry = duckdb::unique_ptr<data_t[]>(new data_t[entry_size]);
			memcpy(entry.get(), wal_contents.get() + payload_offset, entry_size);
			if (Checksum(entry.get(), entry_size) != stored_checksum) {
				break;
			}
			valid_entries++;
			// the payload starts with the wal_type property: field id 100 (little endian) followed by the type
			bool is_insert = entry[0] == 100 && entry[1] == 0 && entry[2] == 26; // WALType::INSERT_TUPLE
			if (is_insert && valid_entries >= 2 && inserts_seen++ == insert_nr) {
				entry[2] = 7; // unused WALType
				uint64_t new_checksum = Checksum(entry.get(), entry_size);
				handle->Write(QueryContext(), entry.get(), entry_size, payload_offset);
				handle->Write(QueryContext(), &new_checksum, sizeof(uint64_t), offset + sizeof(uint64_t));
				return;
			}
			offset = payload_offset + entry_size;
		}
	}
	throw InternalException("Could not find the INSERT_TUPLE WAL entry");
}

TEST_CASE("Test WAL entry with an unknown entry type", "[storage][.]") {
	auto config = GetTestConfig();
	auto storage_database = TestCreatePath("wal_unknown_entry_type");
	auto storage_wal = storage_database + ".wal";

	LocalFileSystem lfs;
	config->options.checkpoint_wal_size = idx_t(-1);
	config->options.checkpoint_on_shutdown = false;
	config->options.abort_on_wal_failure = false;
	DeleteDatabase(storage_database);
	{
		DuckDB db(storage_database, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INTEGER);"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (111111);"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (222222);"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES (333333);"));
	}
	CorruptInsertEntryType(lfs, storage_wal, 1);
	// a committed entry with a valid checksum but an unknown type is corruption, not a torn write
	REQUIRE_THROWS_WITH(DuckDB(storage_database, config.get()), Catch::Contains("Invalid WAL entry type 7"));
	REQUIRE(lfs.FileExists(storage_wal));
	DeleteDatabase(storage_database);
}
