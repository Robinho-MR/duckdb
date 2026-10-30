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

//! A SecureContext is a separate DatabaseInstance and ClientContext in which the contents of a secure view are
//! evaluated. Nothing is shared with the calling context by default: the secure context has its own secrets, caches,
//! logs, profiler and extensions, and the caller can only observe the (filtered) rows that the view returns.
//!
//! What is shared is an explicit decision:
//!  * the task scheduler and the buffer pool, so that threads and memory are accounted for once
//!  * the attached databases, so that the view can be resolved against the same catalogs as the caller
//!  * the settings listed in secure_context.cpp, which restrict the secure context the same way as the caller or
//!    that determine the semantics of the query
//!  * the names of the loaded extensions - the extensions are loaded again, they do not share any state
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
