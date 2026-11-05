#include "duckdb/main/secure_context.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/prepared_statement.hpp"
#include "duckdb/main/query_result.hpp"

namespace duckdb {

//! Session settings that determine the semantics of the query
static const char *const INHERITED_SESSION_SETTINGS[] = {"Calendar",
                                                         "TimeZone",
                                                         "default_collation",
                                                         "default_null_order",
                                                         "default_order",
                                                         "ieee_floating_point_ops",
                                                         "preserve_identifier_case"};

SecureContext::SecureContext(ClientContext &caller) {
	db = make_shared_ptr<DuckDB>(*caller.db);
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
