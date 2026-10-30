//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/execution_services.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/helper.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/main/setting_info.hpp"

namespace duckdb {

class ClientContext;
class DatabaseInstance;
class FileSystem;
class Logger;
class MetricsManager;
class QueryProfiler;
class Value;

//! ExecutionServices bundles the services that operations need to do their work: reading settings, logging,
//! recording metrics and accessing the file system.
//! It can be created from either a ClientContext or a DatabaseInstance, and is cheap to copy.
class ExecutionServices {
public:
	ExecutionServices(ClientContext &context); // NOLINT: allow implicit construction
	ExecutionServices(DatabaseInstance &db);   // NOLINT: allow implicit construction

public:
	//! The database instance the services belong to
	DUCKDB_API DatabaseInstance &GetDatabase() const;
	//! The client context backing the services (if any) - only present for services created from a ClientContext
	DUCKDB_API optional_ptr<ClientContext> TryGetClientContext() const;

	//! Equivalent to CURRENT_SETTING(key) SQL function.
	DUCKDB_API SettingLookupResult TryGetCurrentSetting(const Identifier &key, Value &result) const;
	//! Typed variant, leaves result untouched if not found
	template <class TYPE>
	SettingLookupResult TryGetCurrentSetting(const Identifier &key, TYPE &result) const {
		Value output;
		auto lookup_result = TryGetCurrentSetting(key, output);
		if (lookup_result) {
			result = output.GetValue<TYPE>();
		}
		return lookup_result;
	}

	//! The logger: connection-scoped when backed by a ClientContext, otherwise the global logger
	DUCKDB_API Logger &GetLogger() const;
	//! The file system: client-specific when backed by a ClientContext, otherwise the database file system
	DUCKDB_API FileSystem &GetFileSystem() const;
	//! The database-wide metrics manager
	DUCKDB_API MetricsManager &GetMetricsManager() const;
	//! The query profiler of the current connection - only present for services created from a ClientContext
	DUCKDB_API optional_ptr<QueryProfiler> TryGetQueryProfiler() const;

private:
	reference<DatabaseInstance> db;
	optional_ptr<ClientContext> context;
};

} // namespace duckdb
