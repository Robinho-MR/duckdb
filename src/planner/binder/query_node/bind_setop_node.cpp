#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression_map.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/collate_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_binder/order_binder.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/planner/query_node/bound_set_operation_node.hpp"
#include "duckdb/planner/expression_binder/select_bind_state.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/common/enum_util.hpp"

namespace duckdb {

struct SetOpAliasGatherer {
public:
	explicit SetOpAliasGatherer(SelectBindState &bind_state_p) : bind_state(bind_state_p) {
	}

	void GatherAliases(BoundStatement &stmt, const vector<idx_t> &reorder_idx);
	void GatherSetOpAliases(SetOperationType setop_type, const vector<Identifier> &names,
	                        vector<BoundStatement> &bound_children, const vector<idx_t> &reorder_idx);

private:
	SelectBindState &bind_state;
};

void SetOpAliasGatherer::GatherAliases(BoundStatement &stmt, const vector<idx_t> &reorder_idx) {
	if (stmt.extra_info.setop_type != SetOperationType::NONE) {
		GatherSetOpAliases(stmt.extra_info.setop_type, stmt.names, stmt.extra_info.bound_children, reorder_idx);
		return;
	}

	// query node
	auto &select_names = stmt.names;
	// fill the alias lists with the names
	D_ASSERT(reorder_idx.size() == select_names.size());
	for (idx_t i = 0; i < select_names.size(); i++) {
		auto &name = select_names[i];
		// first check if the alias is already in there
		auto entry = bind_state.alias_map.FindExact(name);

		idx_t index = reorder_idx[i];

		if (entry == bind_state.alias_map.end()) {
			// the alias is not in there yet, just assign it
			bind_state.alias_map.Insert(name, index);
		}
	}
	// check if the expression matches one of the expressions in the original expression list
	auto &select_list = stmt.extra_info.original_expressions;
	for (idx_t i = 0; i < select_list.size(); i++) {
		auto &expr = select_list[i];
		idx_t index = reorder_idx[i];
		// now check if the node is already in the set of expressions
		auto expr_entry = bind_state.projection_map.find(*expr);
		if (expr_entry != bind_state.projection_map.end()) {
			// the node is in there
			// repeat the same as with the alias: if there is an ambiguity we insert "-1"
			if (expr_entry->second != index) {
				bind_state.projection_map[*expr] = DConstants::INVALID_INDEX;
			}
		} else {
			// not in there yet, just place it in there
			bind_state.projection_map[*expr] = index;
		}
	}
}

void SetOpAliasGatherer::GatherSetOpAliases(SetOperationType setop_type, const vector<Identifier> &stmt_names,
                                            vector<BoundStatement> &bound_children, const vector<idx_t> &reorder_idx) {
	// create new reorder index
	if (setop_type == SetOperationType::UNION_BY_NAME) {
		auto &setop_names = stmt_names;
		// for UNION BY NAME - create a new re-order index
		IdentifierLookupMap<idx_t> reorder_map;
		for (idx_t col_idx = 0; col_idx < setop_names.size(); ++col_idx) {
			reorder_map.Insert(setop_names[col_idx], reorder_idx[col_idx]);
		}

		// use new reorder index
		for (auto &child : bound_children) {
			vector<idx_t> new_reorder_idx;
			auto &child_names = child.names;
			for (idx_t col_idx = 0; col_idx < child_names.size(); col_idx++) {
				auto &col_name = child_names[col_idx];
				auto entry = reorder_map.find(col_name);
				if (entry == reorder_map.end()) {
					throw InternalException("SetOp - Column name not found in reorder_map in UNION BY NAME");
				}
				new_reorder_idx.push_back(entry->second);
			}
			GatherAliases(child, new_reorder_idx);
		}
	} else {
		for (auto &child : bound_children) {
			GatherAliases(child, reorder_idx);
		}
	}
}

static void GatherAliases(BoundSetOperationNode &root, vector<BoundStatement> &child_statements,
                          SelectBindState &bind_state) {
	SetOpAliasGatherer gatherer(bind_state);
	vector<idx_t> reorder_idx;
	for (idx_t i = 0; i < root.names.size(); i++) {
		reorder_idx.push_back(i);
	}
	gatherer.GatherSetOpAliases(root.setop_type, root.names, child_statements, reorder_idx);
}

void Binder::BuildUnionByNameInfo(BoundSetOperationNode &result) {
	D_ASSERT(result.setop_type == SetOperationType::UNION_BY_NAME);
	// map every column of every child to a column of the result: columns are matched by exact name first, and
	// otherwise case-insensitively if the name matches exactly one result column
	vector<vector<idx_t>> child_to_result;
	IdentifierLookupMap<idx_t> result_map;
	D_ASSERT(result.names.empty());
	for (auto &child : result.bound_children) {
		auto &child_names = child.names;
		IdentifierLookupSet child_name_set;
		for (auto &col_name : child_names) {
			if (!child_name_set.Insert(col_name)) {
				throw BinderException(
				    "UNION (ALL) BY NAME operation doesn't support duplicate names in the SELECT list - "
				    "the name %s occurs multiple times",
				    col_name);
			}
		}
		vector<idx_t> mapping(child_names.size(), DConstants::INVALID_INDEX);
		unordered_set<idx_t> used_result_columns;
		for (idx_t i = 0; i < child_names.size(); ++i) {
			auto entry = result_map.FindExact(child_names[i]);
			if (entry != result_map.end()) {
				mapping[i] = entry->second;
				used_result_columns.insert(entry->second);
			}
		}
		for (idx_t i = 0; i < child_names.size(); ++i) {
			if (mapping[i] != DConstants::INVALID_INDEX) {
				continue;
			}
			auto &col_name = child_names[i];
			auto matches = result_map.GetCaseInsensitiveMatches(col_name);
			if (matches.size() > 1) {
				throw BinderException("UNION (ALL) BY NAME column %s is ambiguous: multiple columns only differ in "
				                      "case - use the exact column name",
				                      col_name);
			}
			if (matches.size() == 1) {
				auto result_idx = result_map.FindExact(matches[0])->second;
				if (used_result_columns.find(result_idx) == used_result_columns.end()) {
					mapping[i] = result_idx;
					used_result_columns.insert(result_idx);
					continue;
				}
			}
			// column is not yet present in the result
			mapping[i] = result.names.size();
			used_result_columns.insert(result.names.size());
			result_map.Insert(col_name, result.names.size());
			result.names.emplace_back(col_name);
		}
		child_to_result.push_back(std::move(mapping));
	}
	// the names of every child must resolve to the result column they are mapped to
	for (idx_t child_idx = 0; child_idx < child_to_result.size(); child_idx++) {
		auto &child_names = result.bound_children[child_idx].names;
		for (idx_t i = 0; i < child_names.size(); i++) {
			auto entry = result_map.find(child_names[i]);
			if (entry == result_map.end() || entry->second != child_to_result[child_idx][i]) {
				throw BinderException("UNION (ALL) BY NAME column %s is ambiguous: multiple columns only differ in "
				                      "case - use the exact column name",
				                      child_names[i]);
			}
		}
	}
	vector<vector<optional_idx>> node_name_maps;
	for (auto &mapping : child_to_result) {
		vector<optional_idx> result_to_child(result.names.size());
		for (idx_t i = 0; i < mapping.size(); i++) {
			result_to_child[mapping[i]] = i;
		}
		node_name_maps.push_back(std::move(result_to_child));
	}

	idx_t new_size = result.names.size();
	bool need_reorder = false;

	// construct the return type of each of the columns
	for (idx_t i = 0; i < new_size; ++i) {
		LogicalType result_type(LogicalTypeId::INVALID);
		for (idx_t child_idx = 0; child_idx < result.bound_children.size(); ++child_idx) {
			auto &child_types = result.bound_children[child_idx].types;
			auto &child_name_map = node_name_maps[child_idx];
			// check if the column exists in this child node
			auto entry = child_name_map[i];
			if (!entry.IsValid()) {
				need_reorder = true;
			} else {
				auto col_idx_in_child = ProjectionIndex(entry.GetIndex());
				auto &child_col_type = child_types[col_idx_in_child];
				// the child exists in this node - compute the type
				if (result_type.id() == LogicalTypeId::INVALID) {
					result_type = child_col_type;
				} else {
					result_type = LogicalType::ForceMaxLogicalType(context, result_type, child_col_type);
				}
				if (i != col_idx_in_child) {
					// the column exists - but the children are out-of-order, so we need to re-order anyway
					need_reorder = true;
				}
			}
		}
		// compute the final type for each column
		if (!CanContainNulls()) {
			if (ExpressionBinder::ContainsNullType(result_type)) {
				result_type = ExpressionBinder::ExchangeNullType(result_type);
			}
		}
		result.types.push_back(result_type);
	}

	if (!need_reorder) {
		// if all columns in the children of the set-operations are identical we don't need to re-order at all
		// skip adding expressions entirely
		return;
	}
	// If reorder is required, generate the expressions for each node
	vector<vector<unique_ptr<Expression>>> reorder_expressions;
	reorder_expressions.resize(result.bound_children.size());
	for (idx_t i = 0; i < new_size; ++i) {
		for (idx_t child_idx = 0; child_idx < result.bound_children.size(); ++child_idx) {
			auto &child = result.bound_children[child_idx];
			auto &child_name_map = node_name_maps[child_idx];
			// check if the column exists in this child node
			auto entry = child_name_map[i];
			unique_ptr<Expression> expr;
			if (!entry.IsValid()) {
				// the column does not exist - push a `NULL`
				expr = make_uniq<BoundConstantExpression>(Value(result.types[i]));
			} else {
				// the column exists - reference it
				auto col_idx_in_child = ProjectionIndex(entry.GetIndex());
				auto &child_col_type = child.types[col_idx_in_child];
				auto root_idx = child.plan->GetRootIndex();
				expr = make_uniq<BoundColumnRefExpression>(child_col_type, ColumnBinding(root_idx, col_idx_in_child));
			}
			reorder_expressions[child_idx].push_back(std::move(expr));
		}
	}
	// now push projections for each node
	for (idx_t child_idx = 0; child_idx < result.bound_children.size(); ++child_idx) {
		auto &child = result.bound_children[child_idx];
		auto &child_reorder_expressions = reorder_expressions[child_idx];
		// if we have re-order expressions push a projection
		vector<LogicalType> child_types;
		for (auto &expr : child_reorder_expressions) {
			child_types.push_back(expr->GetReturnType());
		}
		auto child_projection =
		    make_uniq<LogicalProjection>(GenerateTableIndex(), std::move(child_reorder_expressions));
		child_projection->children.push_back(std::move(child.plan));
		child.plan = std::move(child_projection);
		child.types = std::move(child_types);
	}
}

static void GatherSetOpBinders(vector<BoundStatement> &children, vector<shared_ptr<Binder>> &binders,
                               vector<reference<Binder>> &result) {
	for (auto &child_binder : binders) {
		result.push_back(*child_binder);
	}
	for (auto &child_node : children) {
		GatherSetOpBinders(child_node.extra_info.bound_children, child_node.extra_info.child_binders, result);
	}
}

// whether an ORDER BY expression of a set operation can be resolved against the select lists of its leaves
static bool SetOpProvidesOrderExpression(const ParsedExpression &expr, const QueryNode &node) {
	if (node.type == QueryNodeType::SET_OPERATION_NODE) {
		for (auto &child : node.Cast<SetOperationNode>().children) {
			if (SetOpProvidesOrderExpression(expr, *child)) {
				return true;
			}
		}
		return false;
	}
	if (node.type != QueryNodeType::SELECT_NODE) {
		return true;
	}
	for (auto &select_expr : node.Cast<SelectNode>().select_list) {
		if (select_expr->Equals(expr)) {
			return true;
		}
	}
	return false;
}

static bool OrderExpressionIsSimple(const ParsedExpression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF:
	case ExpressionClass::POSITIONAL_REFERENCE:
	case ExpressionClass::CONSTANT:
	case ExpressionClass::PARAMETER:
	case ExpressionClass::STAR:
		return true;
	case ExpressionClass::COLLATE:
		return OrderExpressionIsSimple(expr.Cast<CollateExpression>().Child());
	default:
		return false;
	}
}

// ORDER BY expressions over the columns of a set operation (e.g. "ORDER BY col IS NULL, col") cannot be pushed into
// every child; they are evaluated over the result instead by binding SELECT * FROM (set operation) ORDER BY ...
static bool SetOpNeedsOrderWrap(const SetOperationNode &statement) {
	for (auto &modifier : statement.modifiers) {
		if (modifier->type != ResultModifierType::ORDER_MODIFIER) {
			continue;
		}
		for (auto &order : modifier->Cast<OrderModifier>().orders) {
			if (!OrderExpressionIsSimple(*order.expression) &&
			    !SetOpProvidesOrderExpression(*order.expression, statement)) {
				return true;
			}
		}
	}
	return false;
}

static unique_ptr<SelectNode> WrapSetOpForOrdering(const SetOperationNode &statement) {
	auto inner = statement.Copy();
	auto &inner_setop = inner->Cast<SetOperationNode>();
	auto outer = make_uniq<SelectNode>();
	vector<unique_ptr<ResultModifier>> inner_modifiers;
	for (auto &modifier : inner_setop.modifiers) {
		if (modifier->type == ResultModifierType::DISTINCT_MODIFIER) {
			inner_modifiers.push_back(std::move(modifier));
		} else {
			outer->modifiers.push_back(std::move(modifier));
		}
	}
	inner_setop.modifiers = std::move(inner_modifiers);
	auto subquery = make_uniq<SelectStatement>();
	subquery->node = std::move(inner);
	outer->select_list.push_back(make_uniq<StarExpression>());
	outer->from_table = make_uniq<SubqueryRef>(std::move(subquery));
	return outer;
}

BoundStatement Binder::BindNode(SetOperationNode &statement) {
	if (SetOpNeedsOrderWrap(statement)) {
		auto wrapped = WrapSetOpForOrdering(statement);
		return BindNode(*wrapped);
	}
	BoundSetOperationNode result;
	result.setop_type = statement.setop_type;
	result.setop_all = statement.setop_all;

	// first recursively visit the set operations
	// all children have an independent BindContext and Binder
	result.setop_index = GenerateTableIndex();
	if (statement.children.size() < 2) {
		throw InternalException("Set Operations must have at least 2 children");
	}
	if (statement.children.size() != 2 && statement.setop_type != SetOperationType::UNION &&
	    statement.setop_type != SetOperationType::UNION_BY_NAME) {
		throw InternalException("Set Operation type must have exactly 2 children - except for UNION/UNION_BY_NAME");
	}
	for (auto &child : statement.children) {
		auto child_binder = Binder::CreateBinder(context, this);
		child_binder->SetCanContainNulls(true);
		auto child_node = child_binder->BindNode(*child);
		MoveCorrelatedExpressions(*child_binder);
		result.bound_children.push_back(std::move(child_node));
		result.child_binders.push_back(std::move(child_binder));
	}

	if (result.setop_type == SetOperationType::UNION_BY_NAME) {
		// UNION BY NAME - merge the columns from all sides
		BuildUnionByNameInfo(result);
	} else {
		// UNION ALL BY POSITION - the columns of both sides must match exactly
		result.names = result.bound_children[0].names;
		auto result_columns = result.bound_children[0].types.size();
		for (idx_t i = 1; i < result.bound_children.size(); ++i) {
			if (result.bound_children[i].types.size() != result_columns) {
				throw BinderException("Set operations can only apply to expressions with the "
				                      "same number of result columns");
			}
		}

		// figure out the types of the setop result by picking the max of both
		for (idx_t i = 0; i < result_columns; i++) {
			auto result_type = result.bound_children[0].types[i];
			for (idx_t child_idx = 1; child_idx < result.bound_children.size(); ++child_idx) {
				auto &child_types = result.bound_children[child_idx].types;
				result_type = LogicalType::ForceMaxLogicalType(context, result_type, child_types[i]);
			}
			if (!CanContainNulls()) {
				if (ExpressionBinder::ContainsNullType(result_type)) {
					result_type = ExpressionBinder::ExchangeNullType(result_type);
				}
			}
			result.types.push_back(result_type);
		}
	}

	if (!statement.setop_all) {
		statement.modifiers.insert(statement.modifiers.begin(), make_uniq<DistinctModifier>());
		if (statement.setop_type == SetOperationType::UNION ||
		    statement.setop_type == SetOperationType::UNION_BY_NAME) {
			result.setop_all = true;
		}
	}

	SelectBindState bind_state;
	if (!statement.modifiers.empty()) {
		// handle the ORDER BY/DISTINCT clauses
		vector<reference<Binder>> binders;
		GatherSetOpBinders(result.bound_children, result.child_binders, binders);
		GatherAliases(result, result.bound_children, bind_state);

		// now we perform the actual resolution of the ORDER BY/DISTINCT expressions
		OrderBinder order_binder(binders, bind_state);
		PrepareModifiers(order_binder, statement, result);
	}

	// finally bind the types of the ORDER/DISTINCT clause expressions
	BindModifiers(result, result.setop_index, result.names, result.types, bind_state);

	BoundStatement result_statement;
	result_statement.types = result.types;
	result_statement.names = result.names;
	result_statement.plan = CreatePlan(result);
	result_statement.extra_info.setop_type = statement.setop_type;
	result_statement.extra_info.bound_children = std::move(result.bound_children);
	result_statement.extra_info.child_binders = std::move(result.child_binders);
	return result_statement;
}

} // namespace duckdb
