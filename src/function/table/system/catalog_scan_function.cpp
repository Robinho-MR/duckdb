#include "duckdb/function/table/catalog_scan_function.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace duckdb {

static unique_ptr<QueryNode> ScanFunctionNode(TableFunction function) {
	auto ref = make_uniq<TableFunctionRef>();
	ref->function = make_uniq<FunctionExpression>(Identifier(function.name), vector<unique_ptr<ParsedExpression>>());
	ref->bind_info = make_shared_ptr<CatalogScanRefInfo>(std::move(function));

	auto node = make_uniq<SelectNode>();
	node->select_list.push_back(make_uniq<StarExpression>());
	node->from_table = std::move(ref);
	return std::move(node);
}

unique_ptr<TableRef> CatalogScanFunctions::BindReplace(ClientContext &context, CatalogType type,
                                                       const TableFunction &legacy) {
	vector<TableFunction> scans;
	bool has_remainder = false;
	auto &meta_transaction = MetaTransaction::Get(context);
	for (auto &database : DatabaseManager::Get(context).GetDatabases(context)) {
		if (database->GetVisibility() == AttachVisibility::HIDDEN) {
			continue; // same skip as Catalog::GetAllSchemas
		}
		auto &catalog = database->GetCatalog();
		if (!catalog.UsesScanFunction(type)) {
			has_remainder = true;
			continue;
		}
		meta_transaction.UseDatabase(database);
		scans.push_back(catalog.GetScanFunction(context, type));
	}
	if (scans.empty()) {
		return nullptr;
	}
	if (has_remainder) {
		// the legacy function scans the catalogs that do not use a scan function
		TableFunction remainder = legacy;
		remainder.bind_replace = nullptr;
		remainder.function_info = make_shared_ptr<CatalogScanRemainder>();
		scans.push_back(std::move(remainder));
	}

	unique_ptr<QueryNode> result;
	if (scans.size() == 1) {
		result = ScanFunctionNode(std::move(scans[0]));
	} else {
		auto union_node = make_uniq<SetOperationNode>();
		union_node->setop_type = SetOperationType::UNION;
		union_node->setop_all = true;
		for (auto &scan : scans) {
			union_node->children.push_back(ScanFunctionNode(std::move(scan)));
		}
		result = std::move(union_node);
	}
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(result);
	return make_uniq<SubqueryRef>(std::move(statement));
}

} // namespace duckdb
