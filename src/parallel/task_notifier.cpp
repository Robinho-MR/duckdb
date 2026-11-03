#include "duckdb/parallel/task_notifier.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/runtime_services.hpp"

namespace duckdb {

TaskNotifier::TaskNotifier(optional_ptr<RuntimeServices> services_p) : services(services_p) {
	if (services && services->IsClient()) {
		services->GetRegisteredState().ForEachState([&](ClientContextState &state) { state.OnTaskStart(*services); });
	}
}

TaskNotifier::~TaskNotifier() {
	if (services && services->IsClient()) {
		services->GetRegisteredState().ForEachState([&](ClientContextState &state) { state.OnTaskStop(*services); });
	}
}

} // namespace duckdb
