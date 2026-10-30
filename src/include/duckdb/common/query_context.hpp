//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/query_context.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/main/execution_services.hpp"

namespace duckdb {

class ClientContext;

//! The QueryContext wraps an optional client context.
//! It makes query-related information available to operations.
class QueryContext {
public:
	QueryContext();
	QueryContext(optional_ptr<ClientContext> context); // NOLINT: allow implicit construction
	QueryContext(ClientContext &context);              // NOLINT: allow implicit construction

public:
	bool Valid() const {
		return context != nullptr;
	}
	optional_ptr<ClientContext> GetClientContext() const {
		return context;
	}
	//! The execution services backing this context (null if the context is not valid)
	optional_ptr<const ExecutionServices> GetServices() const {
		return services ? &*services : nullptr;
	}

private:
	optional_ptr<ClientContext> context;
	optional<ExecutionServices> services;
};

} // namespace duckdb
