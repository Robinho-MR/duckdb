//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/table/catalog_scan_function.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/tableref.hpp"

namespace duckdb {
class Catalog;

//! Attached to a TableFunctionRef (bind_info) to make the binder use this function instead of a catalog lookup.
struct CatalogScanRefInfo : public TableFunctionInfo {
	explicit CatalogScanRefInfo(TableFunction function_p) : function(std::move(function_p)) {
	}

	TableFunction function;
};

//! function_info of a scan function handed out by a catalog: which catalog it scans.
struct CatalogScanTarget : public TableFunctionInfo {
	explicit CatalogScanTarget(string catalog_name_p) : catalog_name(std::move(catalog_name_p)) {
	}

	string catalog_name;
};

//! function_info of a legacy list function when it only scans the catalogs that do not use a scan function.
struct CatalogScanRemainder : public TableFunctionInfo {};

class CatalogScanFunctions {
public:
	//! Builds the TableRef a list function (e.g. duckdb_indexes) is replaced with: a UNION ALL of the scan
	//! functions of the catalogs that use one for `type`, plus `legacy` restricted to the remaining catalogs.
	//! Returns nullptr when no catalog uses a scan function, in which case the legacy function runs as-is.
	static unique_ptr<TableRef> BindReplace(ClientContext &context, CatalogType type, const TableFunction &legacy);
	//! The scan function of the DuckCatalog for indexes.
	static TableFunction DuckIndexesScan(const string &catalog_name);
};

} // namespace duckdb
