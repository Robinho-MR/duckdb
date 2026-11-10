//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/database_file_system.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/opener_file_system.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/main/http/http_util.hpp"

namespace duckdb {
class DatabaseInstance;

class DatabaseFileSystem : public OpenerFileSystem {
public:
	explicit DatabaseFileSystem(DatabaseInstance &db_p) : db(db_p), context(db_p.Services()) {
	}

	FileSystem &GetFileSystem() const override {
		auto &config = DBConfig::GetConfig(db);
		return *config.file_system;
	}
	Context &GetContext() const override {
		return *context;
	}

private:
	DatabaseInstance &db;
	shared_ptr<Context> context;
};

class LocalDatabaseFileSystem : public OpenerFileSystem {
public:
	explicit LocalDatabaseFileSystem(DatabaseInstance &db_p);

	FileSystem &GetFileSystem() const override;
	Context &GetContext() const override {
		return *context;
	}

private:
	DatabaseInstance &db;
	unique_ptr<FileSystem> owned_file_system;
	FileSystem &local_fs;
	shared_ptr<Context> context;
};

} // namespace duckdb
