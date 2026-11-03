//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/runtime_services.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/helper.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/main/setting_info.hpp"

namespace duckdb {

class ClientConfig;
class BufferManager;
class ClientContext;
class DatabaseInstance;
class FileOpener;
class FileSystem;
class Logger;
class MetricsManager;
class QueryProfiler;
class RegisteredStateManager;
class TaskScheduler;
class Value;

//! RuntimeServices owns the services that operations need to do their work: reading settings, logging, recording
//! metrics and accessing the file system.
//! A ClientContext owns one (see ClientContext::Services) that holds all of its connection-specific services.
//! The services can be shared by related contexts (see Connection::CreateChild): they then use the same client
//! config, logger, registered states, file system and client buffer manager. The services do not refer to one
//! context: operations that need a ClientContext get it from the context that runs them. The context that created
//! the services (the home context) is kept alive by the contexts sharing them, as the file opener and the profiler
//! of the services refer to it. Query profilers are per context: the services hold the profiler of the home context,
//! which also receives the metrics reported through the services (e.g. by I/O and memory allocations).
//! A DatabaseInstance can create one (see DatabaseInstance::Services) that only provides database-wide services.
class RuntimeServices {
public:
	//! Creates database-wide services
	//! The database must outlive the services
	DUCKDB_API explicit RuntimeServices(DatabaseInstance &db);
	DUCKDB_API ~RuntimeServices();

	//! Turns these into the services of the given client context: creates the client config, the connection logger,
	//! the query profiler, the file opener, the client file system and the client buffer manager.
	//! The file opener and the query profiler refer to the context, which must outlive the services.
	void InitializeClient(ClientContext &context);

public:
	//! The database instance the services belong to
	DUCKDB_API DatabaseInstance &GetDatabase() const;
	//! Null-safe variant for optional services
	DUCKDB_API static optional_ptr<QueryProfiler> TryGetQueryProfiler(optional_ptr<RuntimeServices> services);
	//! Whether or not these are the services of a client
	DUCKDB_API bool IsClient() const;

	//! The client config - only present for the services of a ClientContext
	DUCKDB_API optional_ptr<ClientConfig> TryGetClientConfig() const;
	DUCKDB_API ClientConfig &GetClientConfig() const;

	//! Looks up the current value of a setting: the client's value if the services belong to a client, otherwise the
	//! database's value. Settings that are computed from the state of a client (e.g. the search path) are not
	//! available from the services: their default value is returned.
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
	//! Returns the value of the current setting set by the user - if the user has set it.
	DUCKDB_API SettingLookupResult TryGetCurrentUserSetting(idx_t setting_index, Value &result) const;

	//! The logger: connection-scoped for the services of a ClientContext, otherwise the global logger
	DUCKDB_API Logger &GetLogger() const;
	//! Re-creates the connection logger (e.g. after the logging configuration changed)
	DUCKDB_API void SetLogger(shared_ptr<Logger> new_logger);
	DUCKDB_API const shared_ptr<Logger> &GetLoggerPtr() const;
	//! The file system: client-specific for the services of a ClientContext, otherwise the database file system
	DUCKDB_API FileSystem &GetFileSystem() const;
	//! The file opener of the client - only present for the services of a ClientContext
	DUCKDB_API optional_ptr<FileOpener> TryGetFileOpener() const;
	//! The buffer manager: client-specific for the services of a ClientContext, otherwise the database one
	DUCKDB_API BufferManager &GetBufferManager() const;
	//! The database-wide task scheduler
	DUCKDB_API TaskScheduler &GetTaskScheduler() const;
	//! The database-wide metrics manager
	DUCKDB_API MetricsManager &GetMetricsManager() const;
	//! The states (e.g. caches) registered by extensions - only present for the services of a ClientContext
	DUCKDB_API RegisteredStateManager &GetRegisteredState() const;
	//! The query profiler of the client - only present for the services of a ClientContext
	DUCKDB_API optional_ptr<QueryProfiler> TryGetQueryProfiler() const;
	DUCKDB_API QueryProfiler &GetQueryProfiler() const;
	DUCKDB_API const shared_ptr<QueryProfiler> &GetQueryProfilerPtr() const;

private:
	DatabaseInstance &db;
	unique_ptr<ClientConfig> config;
	shared_ptr<Logger> logger;
	shared_ptr<QueryProfiler> profiler;
	unique_ptr<RegisteredStateManager> registered_state;
	unique_ptr<FileOpener> file_opener;
	unique_ptr<FileSystem> client_file_system;
	unique_ptr<BufferManager> client_buffer_manager;
};

} // namespace duckdb
