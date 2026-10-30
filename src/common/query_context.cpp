#include "duckdb/common/query_context.hpp"

#include "duckdb/main/client_context.hpp"

namespace duckdb {

QueryContext::QueryContext() {
}

QueryContext::QueryContext(optional_ptr<ClientContext> context) {
	if (context) {
		services = context->Services();
	}
}

QueryContext::QueryContext(ClientContext &context) : services(context.Services()) {
}

QueryContext::QueryContext(shared_ptr<ExecutionServices> services_p) : services(std::move(services_p)) {
}

} // namespace duckdb
