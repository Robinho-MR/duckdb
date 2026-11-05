#include "duckdb/main/secure_context.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/extension_manager.hpp"
#include "duckdb/main/prepared_statement.hpp"
#include "duckdb/main/query_result.hpp"

namespace duckdb {

//! Global settings that restrict what the secure context may access: it must never be more permissive than the caller
static const char *const INHERITED_GLOBAL_SETTINGS[] = {"allowed_directories",
                                                        "allowed_paths",
                                                        "enable_external_access",
                                                        "disabled_filesystems",
                                                        "allow_community_extensions",
                                                        "allow_extensions_metadata_mismatch",
                                                        "autoload_known_extensions",
                                                        "autoinstall_known_extensions",
                                                        "autoinstall_extension_repository",
                                                        "custom_extension_repository",
                                                        "extension_directory"};

//! Session settings that determine the semantics of the query
static const char *const INHERITED_SESSION_SETTINGS[] = {"Calendar",
                                                         "TimeZone",
                                                         "default_collation",
                                                         "default_null_order",
                                                         "default_order",
                                                         "ieee_floating_point_ops",
                                                         "preserve_identifier_case"};

static void InheritGlobalSettings(DBConfig &target, DatabaseInstance &caller) {
	for (auto name : INHERITED_GLOBAL_SETTINGS) {
		Value value;
		if (caller.TryGetCurrentSetting(Identifier(name), value)) {
			target.SetOptionByName(Identifier(name), value);
		}
	}
	// persistent secrets are shared state between instances, the secure context only has its own
	target.SetOptionByName(Identifier("allow_persistent_secrets"), Value::BOOLEAN(false));
}

static void LoadExtensions(DuckDB &secure_db, DatabaseInstance &caller) {
	auto &secure_instance = *secure_db.instance;
	for (auto &extension : caller.GetExtensionManager().GetExtensions()) {
		if (secure_instance.ExtensionIsLoaded(extension)) {
			continue;
		}
		if (ExtensionHelper::LoadExtension(secure_db, extension) == ExtensionLoadResult::LOADED_EXTENSION) {
			continue;
		}
		auto info = caller.GetExtensionManager().GetExtensionInfo(extension);
		if (info && info->install_info && info->install_info->mode == ExtensionInstallMode::STATICALLY_LINKED) {
			// registered by the application that embeds DuckDB: there is nothing to load it from
			continue;
		}
		ExtensionHelper::LoadExternalExtension(secure_instance, *secure_instance.config.file_system, {extension});
	}
}

SecureContext::SecureContext(ClientContext &caller) {
	auto &caller_instance = *caller.db;
	DBConfig config;
	// the core options are inherited: these configure the resources that are shared (e.g. memory and threads)
	config.options = caller_instance.config.options;
	config.options.database_path.clear();
	config.options.access_mode = AccessMode::READ_ONLY;
	config.options.enable_block_allocator = false;
	config.linked_extensions = caller_instance.config.linked_extensions;
	// memory is tracked in the buffer pool of the caller
	config.buffer_pool = caller_instance.config.buffer_pool;
	InheritGlobalSettings(config, caller_instance);

	auto instance = make_shared_ptr<DatabaseInstance>();
	instance->InitializeSecure(caller_instance, &config);
	db = make_shared_ptr<DuckDB>(*instance);
	if (instance->config.options.load_extensions) {
		ExtensionHelper::LoadAllExtensions(*db);
	}
	LoadExtensions(*db, caller_instance);
	instance->GetDatabaseManager().FinalizeStartup();

	// the catalogs are shared so that the view is resolved against the data of the caller
	auto &secure_manager = instance->GetDatabaseManager();
	for (auto &attached : caller_instance.GetDatabaseManager().GetDatabases()) {
		if (attached->IsSystem() || attached->IsTemporary()) {
			continue;
		}
		secure_manager.ShareDatabase(std::move(attached));
	}
	auto default_database = DatabaseManager::TryGetDefaultDatabase(caller);
	if (!IsInvalidCatalog(default_database)) {
		secure_manager.SetDefaultDatabase(default_database);
	}

	connection = make_uniq<Connection>(*db);
	connection->context->client_data->is_secure_context = true;
	// the temporary objects of the caller are shared as well: these were created by the caller itself
	connection->context->client_data->temporary_objects = caller.client_data->temporary_objects;
	for (auto name : INHERITED_SESSION_SETTINGS) {
		Value value;
		Value secure_value;
		if (!caller.TryGetCurrentSetting(Identifier(name), value) ||
		    !connection->context->TryGetCurrentSetting(Identifier(name), secure_value)) {
			continue;
		}
		auto result = connection->Query("SET " + string(name) + " = " + value.ToSQLString());
		if (result->HasError()) {
			result->ThrowError();
		}
	}
}

SecureContext::~SecureContext() {
	connection.reset();
	db.reset();
}

void SecureContext::ThrowSanitizedError(const ErrorData &error, const string &what) {
	switch (error.Type()) {
	case ExceptionType::INTERRUPT:
		throw InterruptException();
	case ExceptionType::OUT_OF_MEMORY:
		throw OutOfMemoryException("Out of memory while reading from %s", what);
	default:
		break;
	}
	// the message can contain values of rows that the caller is not allowed to see
	throw InvalidInputException("Failed to read from %s: the error is not disclosed outside of the secure context",
	                            what);
}

void SecureContext::Describe(const string &query, vector<LogicalType> &types, vector<Identifier> &names) {
	auto prepared = connection->Prepare(query);
	if (prepared->HasError()) {
		ThrowSanitizedError(prepared->GetErrorObject(), "a secure view");
	}
	types = prepared->GetTypes();
	names = prepared->GetNames();
}

unique_ptr<QueryResultStream<>> SecureContext::Execute(const string &query) {
	auto handle = connection->Submit(query);
	if (handle->HasError()) {
		ThrowSanitizedError(handle->GetErrorObject(), "a secure view");
	}
	return make_uniq<QueryResultStream<>>(std::move(handle));
}

void SecureContext::Interrupt() {
	connection->Interrupt();
}

} // namespace duckdb
