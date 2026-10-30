#include "duckdb/common/query_context.hpp"

#include "duckdb/main/client_context.hpp"

namespace duckdb {

QueryContext::QueryContext() : context(nullptr) {
}

QueryContext::QueryContext(optional_ptr<ClientContext> context_p) : context(context_p) {
	if (context) {
		services = context->Services();
	}
}

QueryContext::QueryContext(ClientContext &context_p) : context(&context_p), services(context_p.Services()) {
}

} // namespace duckdb
