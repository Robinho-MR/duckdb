#include "catch.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <thread>

using namespace duckdb;

#ifdef __linux__

namespace {

void CreateSnapshotDatabase(Connection &con) {
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE small AS SELECT range AS i FROM range(10)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE big AS SELECT range AS i, range::VARCHAR || 'x' AS s FROM range(1000000)"));
	REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
}

void VerifyBigTable(Connection &con, const string &prefix) {
	auto result = con.Query("SELECT COUNT(*), SUM(i), SUM(LENGTH(s)) FROM " + prefix + "big");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {1000000}));
	REQUIRE(CHECK_COLUMN(result, 1, {Value::HUGEINT(499999500000)}));
	REQUIRE(CHECK_COLUMN(result, 2, {Value::HUGEINT(6888890)}));
}

idx_t FreeBlocks(Connection &con) {
	auto result = con.Query("SELECT free_blocks FROM pragma_database_size() WHERE database_name = current_database()");
	REQUIRE_NO_FAIL(*result);
	return result->Collection().GetValue(0, 0).GetValue<idx_t>();
}

void ChurnBlocks(Connection &con) {
	// free the blocks of "big" and write new data, which re-uses free blocks when allowed
	REQUIRE_NO_FAIL(con.Query("DROP TABLE big"));
	REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	for (idx_t i = 0; i < 3; i++) {
		REQUIRE_NO_FAIL(con.Query("CREATE OR REPLACE TABLE churn AS SELECT range * 7 AS i, 'yyyyyyyyyy' || range AS s "
		                          "FROM range(1000000)"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	}
}

} // namespace

TEST_CASE("Snapshot readers can attach while a writer is active", "[storage][snapshot]") {
	auto path = TestCreatePath("snapshot_read.db");
	DeleteDatabase(path);

	DuckDB writer_db(path);
	Connection writer(writer_db);
	CreateSnapshotDatabase(writer);
	// committed, but only in the WAL
	REQUIRE_NO_FAIL(writer.Query("INSERT INTO small VALUES (100)"));

	DuckDB reader_db(nullptr);
	Connection reader(reader_db);
	// a snapshot attach must be read-only
	REQUIRE_FAIL(reader.Query("ATTACH '" + path + "' AS s (SNAPSHOT)"));
	REQUIRE_NO_FAIL(reader.Query("ATTACH '" + path + "' AS s (READ_ONLY, SNAPSHOT)"));

	// the snapshot contains the last checkpoint and the commits in the WAL
	auto result = reader.Query("SELECT COUNT(*), MAX(i) FROM s.small");
	REQUIRE(CHECK_COLUMN(result, 0, {11}));
	REQUIRE(CHECK_COLUMN(result, 1, {100}));
	VerifyBigTable(reader, "s.");
	REQUIRE_FAIL(reader.Query("INSERT INTO s.small VALUES (1)"));

	// the writer keeps working and checkpointing
	REQUIRE_NO_FAIL(writer.Query("INSERT INTO small VALUES (101)"));
	REQUIRE_NO_FAIL(writer.Query("CHECKPOINT"));
	result = writer.Query("SELECT COUNT(*) FROM small");
	REQUIRE(CHECK_COLUMN(result, 0, {12}));

	// a new snapshot reader sees the new checkpoint
	REQUIRE_NO_FAIL(reader.Query("ATTACH '" + path + "' AS s2 (READ_ONLY, SNAPSHOT)"));
	result = reader.Query("SELECT COUNT(*) FROM s2.small");
	REQUIRE(CHECK_COLUMN(result, 0, {12}));
	result = reader.Query("SELECT COUNT(*) FROM s.small");
	REQUIRE(CHECK_COLUMN(result, 0, {11}));
}

TEST_CASE("Snapshot readers replay commits made after the last checkpoint", "[storage][snapshot]") {
	auto path = TestCreatePath("snapshot_wal.db");
	DeleteDatabase(path);

	DuckDB writer_db(path);
	Connection writer(writer_db);
	CreateSnapshotDatabase(writer);
	REQUIRE_NO_FAIL(writer.Query("INSERT INTO small VALUES (100)"));
	REQUIRE_NO_FAIL(writer.Query("CHECKPOINT"));

	// commits that are only in the WAL: a small insert, an optimistically written insert, a new table and a delete
	REQUIRE_NO_FAIL(writer.Query("INSERT INTO small VALUES (101)"));
	REQUIRE_NO_FAIL(writer.Query("INSERT INTO big SELECT range AS i, 'z' AS s FROM range(1000000, 1500000)"));
	REQUIRE_NO_FAIL(writer.Query("CREATE TABLE wal_only AS SELECT 42 AS x"));
	REQUIRE_NO_FAIL(writer.Query("DELETE FROM small WHERE i = 0"));
	// uncommitted changes are not visible
	Connection uncommitted(writer_db);
	REQUIRE_NO_FAIL(uncommitted.Query("BEGIN"));
	REQUIRE_NO_FAIL(uncommitted.Query("INSERT INTO small VALUES (999)"));

	DuckDB reader_db(nullptr);
	Connection reader(reader_db);
	REQUIRE_NO_FAIL(reader.Query("ATTACH '" + path + "' AS s (READ_ONLY, SNAPSHOT)"));
	auto result = reader.Query("SELECT COUNT(*), MIN(i), MAX(i) FROM s.small");
	REQUIRE(CHECK_COLUMN(result, 0, {11}));
	REQUIRE(CHECK_COLUMN(result, 1, {1}));
	REQUIRE(CHECK_COLUMN(result, 2, {101}));
	result = reader.Query("SELECT COUNT(*), SUM(i) FROM s.big");
	REQUIRE(CHECK_COLUMN(result, 0, {1500000}));
	REQUIRE(CHECK_COLUMN(result, 1, {Value::HUGEINT(1124999250000)}));
	result = reader.Query("SELECT x FROM s.wal_only");
	REQUIRE(CHECK_COLUMN(result, 0, {42}));

	// the writer keeps committing: the attached reader keeps its view, a new reader sees the new commits
	REQUIRE_NO_FAIL(uncommitted.Query("COMMIT"));
	REQUIRE_NO_FAIL(writer.Query("DROP TABLE wal_only"));
	REQUIRE_NO_FAIL(writer.Query("DELETE FROM big WHERE i >= 1000000"));
	REQUIRE_NO_FAIL(reader.Query("ATTACH '" + path + "' AS s2 (READ_ONLY, SNAPSHOT)"));
	result = reader.Query("SELECT COUNT(*), MAX(i) FROM s2.small");
	REQUIRE(CHECK_COLUMN(result, 0, {12}));
	REQUIRE(CHECK_COLUMN(result, 1, {999}));
	VerifyBigTable(reader, "s2.");
	REQUIRE_FAIL(reader.Query("SELECT * FROM s2.wal_only"));

	result = reader.Query("SELECT COUNT(*), MAX(i) FROM s.small");
	REQUIRE(CHECK_COLUMN(result, 0, {11}));
	REQUIRE(CHECK_COLUMN(result, 1, {101}));
	result = reader.Query("SELECT COUNT(*) FROM s.big");
	REQUIRE(CHECK_COLUMN(result, 0, {1500000}));

	// the writer checkpoints and churns blocks: both readers keep their view
	REQUIRE_NO_FAIL(writer.Query("CHECKPOINT"));
	ChurnBlocks(writer);
	result = reader.Query("SELECT COUNT(*), SUM(i) FROM s.big");
	REQUIRE(CHECK_COLUMN(result, 0, {1500000}));
	REQUIRE(CHECK_COLUMN(result, 1, {Value::HUGEINT(1124999250000)}));
	VerifyBigTable(reader, "s2.");
}

TEST_CASE("Snapshot readers attached before and after a checkpoint with WAL content", "[storage][snapshot]") {
	auto path = TestCreatePath("snapshot_wal_checkpoint.db");
	DeleteDatabase(path);

	DuckDB writer_db(path);
	Connection writer(writer_db);
	CreateSnapshotDatabase(writer);

	DuckDB reader_db(nullptr);
	Connection reader(reader_db);
	for (idx_t round = 0; round < 3; round++) {
		// WAL content before the checkpoint
		REQUIRE_NO_FAIL(writer.Query("INSERT INTO small VALUES (" + to_string(100 + round * 2) + ")"));
		auto before = "b" + to_string(round);
		REQUIRE_NO_FAIL(reader.Query("ATTACH '" + path + "' AS " + before + " (READ_ONLY, SNAPSHOT)"));
		REQUIRE_NO_FAIL(writer.Query("CHECKPOINT"));
		// WAL content after the checkpoint
		REQUIRE_NO_FAIL(writer.Query("INSERT INTO small VALUES (" + to_string(101 + round * 2) + ")"));
		auto after = "a" + to_string(round);
		REQUIRE_NO_FAIL(reader.Query("ATTACH '" + path + "' AS " + after + " (READ_ONLY, SNAPSHOT)"));

		auto result = reader.Query("SELECT COUNT(*), MAX(i) FROM " + before + ".small");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(NumericCast<int64_t>(11 + round * 2))}));
		REQUIRE(CHECK_COLUMN(result, 1, {Value::BIGINT(NumericCast<int64_t>(100 + round * 2))}));
		result = reader.Query("SELECT COUNT(*), MAX(i) FROM " + after + ".small");
		REQUIRE(CHECK_COLUMN(result, 0, {Value::BIGINT(NumericCast<int64_t>(12 + round * 2))}));
		REQUIRE(CHECK_COLUMN(result, 1, {Value::BIGINT(NumericCast<int64_t>(101 + round * 2))}));
		VerifyBigTable(reader, after + ".");
	}
}

TEST_CASE("Writers do not re-use blocks of attached snapshot readers", "[storage][snapshot]") {
	auto path = TestCreatePath("snapshot_reuse.db");
	DeleteDatabase(path);

	DuckDB writer_db(path);
	Connection writer(writer_db);
	CreateSnapshotDatabase(writer);
	auto initial_free_blocks = FreeBlocks(writer);

	DuckDB reader_db(nullptr);
	Connection reader(reader_db);
	REQUIRE_NO_FAIL(reader.Query("ATTACH '" + path + "' AS s (READ_ONLY, SNAPSHOT)"));

	// the blocks of "big" are freed and other data is written, but the blocks are held for the reader
	ChurnBlocks(writer);
	REQUIRE(FreeBlocks(writer) <= initial_free_blocks);
	// the reader only reads the data blocks of "big" now
	VerifyBigTable(reader, "s.");

	// once the reader is gone, the next checkpoint releases the blocks
	REQUIRE_NO_FAIL(reader.Query("DETACH s"));
	REQUIRE_NO_FAIL(writer.Query("INSERT INTO small VALUES (1)"));
	REQUIRE_NO_FAIL(writer.Query("CHECKPOINT"));
	REQUIRE(FreeBlocks(writer) > initial_free_blocks);
	auto result = writer.Query("SELECT COUNT(*), SUM(i) FROM churn");
	REQUIRE(CHECK_COLUMN(result, 0, {1000000}));
	REQUIRE(CHECK_COLUMN(result, 1, {Value::HUGEINT(3499996500000)}));
}

TEST_CASE("A restarted writer does not re-use blocks of attached snapshot readers", "[storage][snapshot]") {
	auto path = TestCreatePath("snapshot_restart.db");
	DeleteDatabase(path);

	auto writer_db = make_uniq<DuckDB>(path);
	auto writer = make_uniq<Connection>(*writer_db);
	CreateSnapshotDatabase(*writer);

	DuckDB reader_db(nullptr);
	Connection reader(reader_db);
	REQUIRE_NO_FAIL(reader.Query("ATTACH '" + path + "' AS s (READ_ONLY, SNAPSHOT)"));

	// the first writer frees the blocks of "big" and exits, a new writer then writes other data
	REQUIRE_NO_FAIL(writer->Query("DROP TABLE big"));
	REQUIRE_NO_FAIL(writer->Query("CHECKPOINT"));
	writer.reset();
	writer_db.reset();

	writer_db = make_uniq<DuckDB>(path);
	writer = make_uniq<Connection>(*writer_db);
	REQUIRE(FreeBlocks(*writer) == 0);
	for (idx_t i = 0; i < 3; i++) {
		REQUIRE_NO_FAIL(writer->Query("CREATE OR REPLACE TABLE churn AS SELECT range * 7 AS i, 'yyyyyyyyyy' || range "
		                              "AS s FROM range(1000000)"));
		REQUIRE_NO_FAIL(writer->Query("CHECKPOINT"));
	}
	VerifyBigTable(reader, "s.");
}

TEST_CASE("Snapshot readers with a regular writer and reader", "[storage][snapshot]") {
	auto path = TestCreatePath("snapshot_modes.db");
	DeleteDatabase(path);
	{
		DuckDB db(path);
		Connection con(db);
		CreateSnapshotDatabase(con);
	}

	// snapshot readers can attach without a writer, also next to each other
	DuckDB reader_db(nullptr);
	Connection reader(reader_db);
	REQUIRE_NO_FAIL(reader.Query("ATTACH '" + path + "' AS s1 (READ_ONLY, SNAPSHOT)"));
	REQUIRE_NO_FAIL(reader.Query("ATTACH '" + path + "' AS s2 (READ_ONLY, SNAPSHOT TRUE)"));
	VerifyBigTable(reader, "s1.");
	VerifyBigTable(reader, "s2.");
	REQUIRE_NO_FAIL(reader.Query("DETACH s2"));

	// a writer can start while snapshot readers are attached
	DuckDB writer_db(path);
	Connection writer(writer_db);
	REQUIRE_NO_FAIL(writer.Query("INSERT INTO small VALUES (100)"));
	REQUIRE_NO_FAIL(writer.Query("CHECKPOINT"));
	VerifyBigTable(reader, "s1.");

	// SNAPSHOT FALSE is a regular attach
	REQUIRE_NO_FAIL(reader.Query("ATTACH '" + path + ".other' AS other (SNAPSHOT FALSE)"));
}

TEST_CASE("Snapshot readers attach while a writer commits and checkpoints", "[storage][snapshot]") {
	auto path = TestCreatePath("snapshot_concurrent.db");
	DeleteDatabase(path);

	DuckDB writer_db(path);
	Connection writer(writer_db);
	REQUIRE_NO_FAIL(writer.Query("CREATE TABLE seq (i BIGINT)"));
	REQUIRE_NO_FAIL(writer.Query("CHECKPOINT"));

	// the writer commits consecutive values, so every snapshot must contain a gap-free prefix
	std::atomic<bool> stop {false};
	std::atomic<bool> writer_failed {false};
	std::thread writer_thread([&]() {
		Connection con(writer_db);
		for (int64_t i = 0; !stop; i++) {
			if (con.Query("INSERT INTO seq VALUES (" + to_string(i) + ")")->HasError()) {
				writer_failed = true;
				return;
			}
			if (i % 25 == 24 && con.Query("CHECKPOINT")->HasError()) {
				writer_failed = true;
				return;
			}
		}
	});

	// no assertion may throw while the writer thread runs
	DuckDB reader_db(nullptr);
	Connection reader(reader_db);
	int64_t previous_count = 0;
	string error;
	for (idx_t attempt = 0; attempt < 50 && error.empty(); attempt++) {
		auto result = reader.Query("ATTACH '" + path + "' AS s (READ_ONLY, SNAPSHOT)");
		if (result->HasError()) {
			error = result->GetError();
			break;
		}
		result = reader.Query("SELECT COUNT(*), COALESCE(MAX(i) + 1, 0), COUNT(DISTINCT i) FROM s.seq");
		if (result->HasError()) {
			error = result->GetError();
			break;
		}
		auto count = result->Collection().GetValue(0, 0).GetValue<int64_t>();
		if (result->Collection().GetValue(1, 0).GetValue<int64_t>() != count ||
		    result->Collection().GetValue(2, 0).GetValue<int64_t>() != count || count < previous_count) {
			error = "inconsistent snapshot: " + result->ToString();
		}
		previous_count = count;
		result = reader.Query("DETACH s");
		if (result->HasError()) {
			error = result->GetError();
		}
	}
	stop = true;
	writer_thread.join();
	INFO(error);
	REQUIRE(error.empty());
	REQUIRE(!writer_failed);
	REQUIRE(previous_count > 0);
}

TEST_CASE("Byte-range locks can be taken without waiting", "[storage][snapshot]") {
	auto path = TestCreatePath("snapshot_try_lock.db");
	DeleteDatabase(path);
	auto fs = FileSystem::CreateLocal();
	auto flags = FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE;
	auto first = fs->OpenFile(path, flags);
	auto second = fs->OpenFile(path, flags);
	if (!first->SupportsRangeLocks()) {
		return;
	}

	// a held read lock blocks a write lock, but not another read lock
	first->LockRange(0, 8, FileLockType::READ_LOCK);
	REQUIRE(!second->TryLockRange(0, 8, FileLockType::WRITE_LOCK));
	REQUIRE(second->TryLockRange(0, 8, FileLockType::READ_LOCK));
	second->UnlockRange(0, 8);
	first->UnlockRange(0, 8);

	// a held write lock blocks both, until it is released
	REQUIRE(second->TryLockRange(0, 8, FileLockType::WRITE_LOCK));
	REQUIRE(!first->TryLockRange(0, 8, FileLockType::READ_LOCK));
	REQUIRE(!first->TryLockRange(4, 8, FileLockType::WRITE_LOCK));
	REQUIRE(first->TryLockRange(8, 8, FileLockType::WRITE_LOCK));
	second->UnlockRange(0, 8);
	REQUIRE(first->TryLockRange(0, 8, FileLockType::READ_LOCK));
}

#endif
