#include "duckdb/common/bind_helpers.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/expression_binder/relation_binder.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/main/config.hpp"

#include <numeric>

namespace duckdb {

Value ConvertVectorToValue(vector<Value> set) {
	if (set.empty()) {
		return Value::LIST(LogicalType::BOOLEAN, std::move(set));
	}
	return Value::LIST(std::move(set));
}

idx_t ParseBytesArg(Context &context, const Identifier &name, const Value &arg) {
	if (arg.IsNull()) {
		throw BinderException("NULL is not supported as a valid option for COPY option \"%s\"",
		                      StringUtil::Upper(name.GetIdentifierName()));
	}
	optional_idx result;
	if (arg.type().id() == LogicalTypeId::VARCHAR) {
		result = DBConfig::ParseMemoryLimit(arg.ToString());
	} else {
		auto cast_arg = arg.TryCastAs(context, LogicalType::UBIGINT);
		if (!cast_arg) {
			throw BinderException("Unable to parse bytes from \"%s\" for copy option \"%s\" ", arg.ToString(),
			                      StringUtil::Upper(name.GetIdentifierName()));
		}
		auto bytes = cast_arg->GetValue<idx_t>();
		if (bytes != DConstants::INVALID_INDEX) {
			result = bytes;
		}
	}
	if (!result.IsValid()) {
		// e.g. '-1' or 'none', which ParseMemoryLimit parses as unlimited
		throw BinderException("Copy option \"%s\" must be a valid size, not \"%s\"",
		                      StringUtil::Upper(name.GetIdentifierName()), arg.ToString());
	}
	return result.GetIndex();
}

//! Resolve a column name of an option against the column names (exact match, otherwise a unique case-insensitive match)
static idx_t ResolveOptionColumn(const vector<Identifier> &names, const string &column_name,
                                 const Identifier &option_name) {
	idx_t index;
	auto match = ResolveIdentifier(names, Identifier(column_name), index);
	if (match == IdentifierMatch::AMBIGUOUS) {
		throw BinderException("%s column %s is ambiguous: multiple columns only differ in case - use the exact "
		                      "column name",
		                      option_name, column_name.c_str());
	}
	if (match == IdentifierMatch::NOT_FOUND) {
		throw BinderException("%s expected to find %s, but it was not found in the table", option_name,
		                      column_name.c_str());
	}
	return index;
}

vector<bool> ParseColumnList(const vector<Value> &set, const vector<Identifier> &names, const Identifier &option_name) {
	vector<bool> result;

	if (set.empty()) {
		throw BinderException("%s expects a column list or * as parameter", option_name);
	}
	result.resize(names.size(), false);
	for (idx_t i = 0; i < set.size(); i++) {
		auto index = ResolveOptionColumn(names, set[i].ToString(), option_name);
		// mark every column with the resolved name
		for (idx_t name_idx = 0; name_idx < names.size(); name_idx++) {
			if (names[name_idx].GetIdentifierName() == names[index].GetIdentifierName()) {
				result[name_idx] = true;
			}
		}
	}
	return result;
}

vector<bool> ParseColumnList(const Value &value, const vector<Identifier> &names, const Identifier &option_name) {
	vector<bool> result;

	// Only accept a list of arguments
	if (value.type().id() != LogicalTypeId::LIST) {
		// Support a single argument if it's '*'
		if (value.type().id() == LogicalTypeId::VARCHAR && value.GetValue<string>() == "*") {
			result.resize(names.size(), true);
			return result;
		}
		throw BinderException("%s expects a column list or * as parameter", option_name);
	}
	if (value.IsNull()) {
		throw BinderException("%s expects a column list or * as parameter, it can't be a NULL value", option_name);
	}
	auto &children = ListValue::GetChildren(value);
	// accept '*' as single argument
	if (children.size() == 1 && children[0].type().id() == LogicalTypeId::VARCHAR &&
	    children[0].GetValue<string>() == "*") {
		result.resize(names.size(), true);
		return result;
	}
	return ParseColumnList(children, names, option_name);
}

vector<idx_t> ParseColumnsOrdered(const vector<Value> &set, const vector<Identifier> &names,
                                  const Identifier &option_name) {
	vector<idx_t> result;

	if (set.empty()) {
		throw BinderException("%s expects a column list or * as parameter", option_name);
	}
	unordered_set<string> option_names;
	unordered_set<idx_t> resolved_columns;
	for (idx_t i = 0; i < set.size(); i++) {
		auto option_column = set[i].ToString();
		auto index = ResolveOptionColumn(names, option_column, option_name);
		if (!option_names.insert(option_column).second || !resolved_columns.insert(index).second) {
			throw BinderException("%s does now allow duplicate columns (found: %s)", option_name, option_column);
		}
		result.push_back(index);
	}
	return result;
}

vector<idx_t> ParseColumnsOrdered(const Value &value, const vector<Identifier> &names, const Identifier &option_name) {
	vector<idx_t> result;

	// Only accept a list of arguments
	if (value.type().id() != LogicalTypeId::LIST) {
		// Support a single argument if it's '*'
		if (value.type().id() == LogicalTypeId::VARCHAR && value.GetValue<string>() == "*") {
			result.resize(names.size(), 0);
			std::iota(std::begin(result), std::end(result), 0);
			return result;
		}
		throw BinderException("%s expects a column list or * as parameter", option_name);
	}
	auto &children = ListValue::GetChildren(value);
	// accept '*' as single argument
	if (children.size() == 1 && children[0].type().id() == LogicalTypeId::VARCHAR &&
	    children[0].GetValue<string>() == "*") {
		result.resize(names.size(), 0);
		std::iota(std::begin(result), std::end(result), 0);
		return result;
	}
	return ParseColumnsOrdered(children, names, option_name);
}

vector<BoundOrderByNode> BindOrderByNodes(Binder &binder, TableIndex table_index, const Identifier &alias,
                                          const vector<Identifier> &names, const vector<LogicalType> &types,
                                          vector<OrderByNode> &orders) {
	auto child_binder = Binder::CreateBinder(binder.context, &binder);
	child_binder->bind_context.AddGenericBinding(table_index, alias, names, types);
	RelationBinder expr_binder(*child_binder, binder.context, "ORDER_BY");
	vector<BoundOrderByNode> bound_orders;
	for (auto &order : orders) {
		auto expression = expr_binder.Bind(order.expression);
		ExpressionBinder::PushCollation(binder.context, expression, expression->GetReturnType());
		bound_orders.emplace_back(order.type, order.null_order, std::move(expression));
	}
	return bound_orders;
}

vector<BoundOrderByNode> ParseOrderByColumns(Binder &binder, const vector<Value> &set,
                                             const BoundStatement &bound_statement, const Identifier &option_name) {
	// Parse
	vector<string> order_by_strings;
	for (auto &value : set) {
		order_by_strings.push_back(value.ToString());
	}
	const auto order_by_clause = StringUtil::Join(order_by_strings, ", ");
	auto parsed_orders = Parser(binder.context).ParseOrderList(order_by_clause);

	// Bind
	auto &config = DBConfig::GetConfig(binder.context);
	for (auto &parsed_order : parsed_orders) {
		parsed_order.type = config.ResolveOrder(binder.context, parsed_order.type);
		parsed_order.null_order = config.ResolveNullOrder(binder.context, parsed_order.type, parsed_order.null_order);
	}
	auto bound_orders = BindOrderByNodes(binder, binder.GenerateTableIndex(), "__copy_input", bound_statement.names,
	                                     bound_statement.types, parsed_orders);

	// Convert BoundColumnRefExpression to BoundReferenceExpression
	vector<Value> name_set;
	case_insensitive_map_t<pair<idx_t, vector<reference<unique_ptr<Expression>>>>> name_map;
	for (auto &bound_order : bound_orders) {
		ExpressionIterator::VisitExpressionClassMutable(
		    bound_order.expression, ExpressionClass::BOUND_COLUMN_REF, [&](unique_ptr<Expression> &child) {
			    auto [it, inserted] = name_map.emplace(make_pair(
			        child->ToString(), make_pair(name_set.size(), vector<reference<unique_ptr<Expression>>>())));
			    it->second.second.push_back(child);
			    if (inserted) {
				    // Ensure we only add unique values
				    name_set.emplace_back(child->ToString());
			    }
		    });
	}
	const auto indices = ParseColumnsOrdered(name_set, bound_statement.names, option_name);
	D_ASSERT(name_set.size() == indices.size());
	for (const auto &name_value : name_set) {
		auto name = name_value.ToString();
		const auto &[idx, expressions] = name_map[name];
		for (auto &expr : expressions) {
			expr.get() =
			    make_uniq<BoundReferenceExpression>(Identifier(name), expr.get()->GetReturnType(), indices[idx]);
		}
	}

	return bound_orders;
}

} // namespace duckdb
