#include "duckdb/main/execution_services.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/profiler/metrics_manager.hpp"
#include "duckdb/main/query_profiler.hpp"

namespace duckdb {

ExecutionServices::ExecutionServices(ClientContext &context_p)
    : db(DatabaseInstance::GetDatabase(context_p)), context(&context_p) {
}

ExecutionServices::ExecutionServices(DatabaseInstance &db_p) : db(db_p) {
}

DatabaseInstance &ExecutionServices::GetDatabase() const {
	return db.get();
}

optional_ptr<ClientContext> ExecutionServices::TryGetClientContext() const {
	return context;
}

SettingLookupResult ExecutionServices::TryGetCurrentSetting(const Identifier &key, Value &result) const {
	if (context) {
		return context->TryGetCurrentSetting(key, result);
	}
	return db.get().TryGetCurrentSetting(key, result);
}

Logger &ExecutionServices::GetLogger() const {
	if (context) {
		return Logger::Get(*context);
	}
	return Logger::Get(db.get());
}

FileSystem &ExecutionServices::GetFileSystem() const {
	auto client_context = context;
	if (client_context) {
		return FileSystem::GetFileSystem(*client_context);
	}
	return FileSystem::GetFileSystem(db.get());
}

MetricsManager &ExecutionServices::GetMetricsManager() const {
	return db.get().GetMetricsManager();
}

optional_ptr<QueryProfiler> ExecutionServices::TryGetQueryProfiler() const {
	auto client_context = context;
	if (!client_context) {
		return nullptr;
	}
	return &QueryProfiler::Get(*client_context);
}

ExecutionServices ClientContext::Services() {
	return ExecutionServices(*this);
}

ExecutionServices DatabaseInstance::Services() {
	return ExecutionServices(*this);
}

} // namespace duckdb
