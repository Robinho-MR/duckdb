//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/secure_context.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/main/query_result_stream.hpp"

namespace duckdb {
class ClientContext;
class Connection;
class DuckDB;
class DatabaseInstance;

//! A SecureContext is a separate ClientContext in which the contents of a secure view are evaluated. The caller can
//! only observe the (filtered) rows that the view returns: the profiler, the query log of the connection and the
//! session settings are not shared. The ClientContext belongs to the same DatabaseInstance, so everything that is
//! owned by the instance is shared with the caller - catalogs, secrets, caches, logs and extensions.
//! Errors are sanitized when they cross the boundary - they can contain data of rows that the caller cannot see.
class SecureContext {
public:
	explicit SecureContext(ClientContext &caller);
	~SecureContext();

public:
	//! Prepare the query in the secure context and return the schema of its result
	void Describe(const string &query, vector<LogicalType> &types, vector<Identifier> &names);
	//! Run the query in the secure context, streaming the result
	unique_ptr<QueryResultStream<>> Execute(const string &query);
	//! Interrupt the query that is running in the secure context
	void Interrupt();

	//! Throw an error that describes `error` without revealing its contents. `what` names the object being accessed
	[[noreturn]] static void ThrowSanitizedError(const ErrorData &error, const string &what);

private:
	shared_ptr<DuckDB> db;
	unique_ptr<Connection> connection;
};

} // namespace duckdb
