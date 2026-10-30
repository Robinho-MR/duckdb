#include "duckdb/function/table/range.hpp"

#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/secure_context.hpp"
#include "duckdb/planner/bound_expression_sql_exporter.hpp"
#include "duckdb/planner/expression/expression_barrier.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

namespace {

struct SecureViewScanBindData : public TableFunctionData {
	//! The qualified name of the view, rendered as SQL
	string view_reference;
	vector<Identifier> names;
	//! The predicates of the caller that are evaluated by the secure context, rendered as SQL
	vector<string> filters;

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<SecureViewScanBindData>();
		return view_reference == other.view_reference && filters == other.filters;
	}
	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<SecureViewScanBindData>();
		result->view_reference = view_reference;
		result->names = names;
		result->filters = filters;
		return std::move(result);
	}
};

struct SecureViewScanGlobalState : public GlobalTableFunctionState {
	~SecureViewScanGlobalState() override {
		if (result) {
			// the scan ended before the view was read completely (e.g. LIMIT or an error)
			secure_context->Interrupt();
		}
	}

	//! The secure context stays alive for as long as the scan: it owns the data that the result refers to
	unique_ptr<SecureContext> secure_context;
	unique_ptr<QueryResultStream<>> result;
	idx_t column_count = 0;
};

string ViewReference(TableFunctionBindInput &input) {
	string result;
	for (idx_t i = 0; i < 3; i++) {
		if (input.inputs[i].IsNull()) {
			throw BinderException("Cannot use NULL as function argument");
		}
		if (i > 0) {
			result += ".";
		}
		result += SQLQuotedIdentifier::ToString(input.inputs[i].GetValue<string>());
	}
	auto unit = input.named_parameters.find("at_unit");
	auto value = input.named_parameters.find("at_value");
	if (unit != input.named_parameters.end() && !unit->second.IsNull() && value != input.named_parameters.end()) {
		result += " AT (" + SQLQuotedIdentifier::ToString(unit->second.GetValue<string>()) + " => " +
		          value->second.ToSQLString() + ")";
	}
	return result;
}

unique_ptr<FunctionData> SecureViewScanBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<SecureViewScanBindData>();
	result->view_reference = ViewReference(input);
	// the view is bound in the secure context - the caller only learns the schema that the view returns
	SecureContext secure_context(context);
	secure_context.Describe("SELECT * FROM " + result->view_reference, return_types, names);
	result->names = names;
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> SecureViewScanInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<SecureViewScanBindData>();
	auto result = make_uniq<SecureViewScanGlobalState>();
	string projection;
	for (auto &column_id : input.column_ids) {
		D_ASSERT(column_id < bind_data.names.size());
		if (!projection.empty()) {
			projection += ", ";
		}
		projection += SQLQuotedIdentifier::ToString(bind_data.names[column_id]);
		result->column_count++;
	}
	if (projection.empty()) {
		// only the number of rows is needed
		projection = SQLQuotedIdentifier::ToString(bind_data.names[0]);
	}
	string query = "SELECT " + projection + " FROM " + bind_data.view_reference;
	if (!bind_data.filters.empty()) {
		query += " WHERE (" + StringUtil::Join(bind_data.filters, ") AND (") + ")";
	}
	result->secure_context = make_uniq<SecureContext>(context);
	result->result = result->secure_context->Execute(query);
	return std::move(result);
}

//! Hand the predicates of the caller to the secure context, so that the view can prune with them. The secure context
//! applies its own protection to them: it evaluates them against the view in the same way as any other caller. A
//! predicate that remains in the plan of the caller is evaluated on the rows that the view returned
void SecureViewScanPushdownFilters(ClientContext &context, LogicalGet &get, FunctionData *bind_data_p,
                                   vector<unique_ptr<Expression>> &filters) {
	auto &bind_data = bind_data_p->Cast<SecureViewScanBindData>();
	BoundExpressionSQLExportContext export_context;
	export_context.client_context = &context;
	export_context.resolve_binding = [&](const ColumnBinding &binding) -> optional<ResolvedSQLColumnReference> {
		auto &column_ids = get.GetColumnIds();
		if (binding.table_index != get.table_index || binding.column_index.GetIndex() >= column_ids.size()) {
			return {};
		}
		auto &column_id = column_ids[binding.column_index.GetIndex()];
		if (!column_id.HasPrimaryIndex() || column_id.IsVirtualColumn() || column_id.HasChildren()) {
			return {};
		}
		auto index = column_id.GetPrimaryIndex();
		return ResolvedSQLColumnReference {{bind_data.names[index]}, get.returned_types[index]};
	};
	for (idx_t i = 0; i < filters.size(); i++) {
		auto &filter = *filters[i];
		if (filter.IsVolatile() || filter.HasSubquery() || filter.HasParameter() ||
		    ExpressionBarrier::Required(filter) || ExpressionBarrier::Contains(filter)) {
			continue;
		}
		auto exported = BoundExpressionSQLExporter::Export(filter, export_context);
		if (exported.HasError()) {
			continue;
		}
		bind_data.filters.push_back(exported.GetValue()->ToString());
		filters.erase_at(i);
		i--;
	}
}

InsertionOrderPreservingMap<string> SecureViewScanToString(TableFunctionToStringInput &input) {
	InsertionOrderPreservingMap<string> result;
	auto &bind_data = input.bind_data->Cast<SecureViewScanBindData>();
	result["View"] = bind_data.view_reference;
	if (!bind_data.filters.empty()) {
		result["Filters"] = StringUtil::Join(bind_data.filters, "\n");
	}
	return result;
}

void SecureViewScanFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<SecureViewScanGlobalState>();
	if (!state.result) {
		return;
	}
	unique_ptr<DataChunk> chunk;
	try {
		chunk = state.result->Fetch();
	} catch (std::exception &ex) {
		SecureContext::ThrowSanitizedError(ErrorData(ex), "a secure view");
	}
	if (!chunk) {
		if (state.result->HasError()) {
			SecureContext::ThrowSanitizedError(state.result->GetErrorObject(), "a secure view");
		}
		state.result.reset();
		return;
	}
	if (state.column_count == 0) {
		output.SetChildCardinality(chunk->size());
		return;
	}
	output.Reference(*chunk);
}

} // namespace

void SecureViewScanTableFunction::RegisterFunction(BuiltinFunctions &set) {
	TableFunction scan("__secure_view_scan", {}, SecureViewScanFunction, SecureViewScanBind, SecureViewScanInit);
	scan.GetSignature()
	    .AddPositionalOnly("catalog", LogicalType::VARCHAR)
	    .AddPositionalOnly("schema", LogicalType::VARCHAR)
	    .AddPositionalOnly("view", LogicalType::VARCHAR);
	scan.GetSignature().AddKeywordOnly("at_unit", LogicalType::VARCHAR, Value(LogicalType::VARCHAR));
	scan.GetSignature().AddKeywordOnly("at_value", LogicalType::ANY, Value());
	scan.projection_pushdown = true;
	scan.pushdown_complex_filter = SecureViewScanPushdownFilters;
	scan.to_string = SecureViewScanToString;
	set.AddFunction(scan);
}

} // namespace duckdb
