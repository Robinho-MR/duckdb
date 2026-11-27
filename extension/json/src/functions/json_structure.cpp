// json_structure and the json_group_structure macro

#include "json_executors.hpp"
#include "json_schema_inference.hpp"

namespace duckdb {
namespace json {

namespace {

//===--------------------------------------------------------------------===//
// Structure nodes
//===--------------------------------------------------------------------===//
//! A structure node represents a subtree of the output schema.
struct StructureNode {
	explicit StructureNode(yyjson_mut_val &value_p) : value(&value_p) {
	}

public:
	bool MergeType(LogicalTypeId type_p, yyjson_mut_doc *doc);

public:
	//! The inferred schema for the subtree associated with this structure node.
	yyjson_mut_val *value;
	//! Merged input type; INVALID before discovery, ignored once incompatible.
	LogicalTypeId type = LogicalTypeId::INVALID;
	//! Conflicting types at this schema position forced the output to "JSON".
	bool incompatible = false;

	//! Shared schema node for all array elements.
	StructureNode *array_child = nullptr;
	//! Schema nodes for object fields, indexed by key.
	json_key_map_t<StructureNode *> key_map;

private:
	void SetType(LogicalTypeId type_p);
	void MarkIncompatible(yyjson_mut_doc *doc);
};

//! The name of the types that a JSON value can have
const char *StructureTypeName(LogicalTypeId type) {
	switch (type) {
	case LogicalTypeId::SQLNULL:
		return JSONCommon::TYPE_STRING_NULL;
	case LogicalTypeId::BOOLEAN:
		return "BOOLEAN";
	case LogicalTypeId::BIGINT:
		return "BIGINT";
	case LogicalTypeId::UBIGINT:
		return "UBIGINT";
	case LogicalTypeId::HUGEINT:
		return "HUGEINT";
	case LogicalTypeId::DOUBLE:
		return "DOUBLE";
	case LogicalTypeId::VARCHAR:
		return "VARCHAR";
	default:
		throw InternalException("Unexpected type in json_structure");
	}
}

bool StructureNode::MergeType(LogicalTypeId type_p, yyjson_mut_doc *doc) {
	D_ASSERT(type_p != LogicalTypeId::INVALID);
	if (incompatible) {
		return false;
	}
	if (type == LogicalTypeId::INVALID || type == LogicalTypeId::SQLNULL) {
		SetType(type_p);
		return true;
	}
	if (type_p == LogicalTypeId::SQLNULL || type == type_p) {
		return true;
	}
	if (JSONSchemaInference::IsNumericType(type) && JSONSchemaInference::IsNumericType(type_p)) {
		SetType(JSONSchemaInference::MaxNumericType(type, type_p));
		return true;
	}
	MarkIncompatible(doc);
	return false;
}

void StructureNode::SetType(LogicalTypeId type_p) {
	D_ASSERT(!incompatible && type_p != LogicalTypeId::INVALID);
	if (type == type_p) {
		return;
	}
	type = type_p;
	if (type == LogicalTypeId::LIST) {
		yyjson_mut_set_arr(value);
	} else {
		const auto type_name = type == LogicalTypeId::STRUCT ? "JSON" : StructureTypeName(type);
		yyjson_mut_set_str(value, type_name);
	}
}

void StructureNode::MarkIncompatible(yyjson_mut_doc *doc) {
	auto fallback = yyjson_mut_str(doc, "JSON");
	if (!fallback) {
		throw InternalException("Failed to allocate JSON structure value");
	}
	incompatible = true;
	array_child = nullptr;
	key_map.clear();
	// Replace the payload while preserving the link to the next sibling.
	value->tag = fallback->tag;
	value->uni = fallback->uni;
}

//===--------------------------------------------------------------------===//
// Node creation and lookup
//===--------------------------------------------------------------------===//
using NodeStorage = vector<unique_ptr<StructureNode>>;

StructureNode &CreateStructureNode(NodeStorage &nodes, yyjson_mut_doc *doc) {
	auto value = yyjson_mut_str(doc, JSONCommon::TYPE_STRING_NULL);
	if (!value) {
		throw InternalException("Failed to allocate JSON structure value");
	}
	nodes.push_back(make_uniq<StructureNode>(*value));
	return *nodes.back();
}

StructureNode &GetOrCreateArrayChild(NodeStorage &nodes, StructureNode &node, yyjson_mut_doc *doc) {
	D_ASSERT(node.type == LogicalTypeId::LIST && !node.incompatible);
	// All array elements contribute to one shared schema node.
	if (!node.array_child) {
		node.array_child = &CreateStructureNode(nodes, doc);
		yyjson_mut_arr_append(node.value, node.array_child->value);
	}
	return *node.array_child;
}

StructureNode &GetOrCreateObjectChild(NodeStorage &nodes, StructureNode &node, yyjson_val &key, yyjson_mut_doc *doc) {
	D_ASSERT(node.type == LogicalTypeId::STRUCT && !node.incompatible);
	const JSONKey lookup_key {unsafe_yyjson_get_str(&key), unsafe_yyjson_get_len(&key)};
	const auto entry = node.key_map.find(lookup_key);
	if (entry != node.key_map.end()) {
		return *entry->second;
	}

	auto output_key = yyjson_mut_strncpy(doc, lookup_key.ptr, lookup_key.len);
	if (!output_key) {
		throw InternalException("Failed to allocate JSON structure key");
	}
	auto &child = CreateStructureNode(nodes, doc);
	// Empty objects keep the JSON fallback until their first field.
	if (node.key_map.empty()) {
		yyjson_mut_set_obj(node.value);
	}
	yyjson_mut_obj_add(node.value, output_key, child.value);
	const JSONKey persistent_key {yyjson_mut_get_str(output_key), lookup_key.len};
	node.key_map.emplace(persistent_key, &child);
	return child;
}

//===--------------------------------------------------------------------===//
// Traversal
//===--------------------------------------------------------------------===//
//! Each frame tracks one input value; multiple frames can reuse the same structure node.
struct Frame {
	Frame(yyjson_val *val_p, StructureNode &node_p) : val(val_p), node(&node_p) {
	}
	//! The input JSON value being processed.
	yyjson_val *val;
	//! The schema node to which this input value contributes.
	StructureNode *node;
	//! Once initialized, the input value's container type determines the active iterator.
	union {
		yyjson_arr_iter arr_it;
		yyjson_obj_iter obj_it;
	};
	bool initialized = false;
};

std::optional<Frame> HandleArrayFrame(NodeStorage &nodes, Frame &frame, yyjson_mut_doc *doc) {
	D_ASSERT(yyjson_is_arr(frame.val));
	auto &node = *frame.node;
	if (node.incompatible) {
		return std::nullopt;
	}

	if (!frame.initialized) {
		if (!node.MergeType(LogicalTypeId::LIST, doc)) {
			return std::nullopt;
		}

		frame.arr_it = yyjson_arr_iter_with(frame.val);
		frame.initialized = true;
	}
	auto &child = GetOrCreateArrayChild(nodes, node, doc);
	if (child.incompatible) {
		return std::nullopt;
	}

	// Process scalar values without creating child frames.
	while (auto next = yyjson_arr_iter_next(&frame.arr_it)) {
		if (yyjson_is_ctn(next)) {
			return Frame(next, child);
		}
		if (!child.MergeType(JSONCommon::ValTypeToLogicalTypeId(next), doc)) {
			return std::nullopt;
		}
	}
	return std::nullopt;
}

std::optional<Frame> HandleObjectFrame(NodeStorage &nodes, Frame &frame, yyjson_mut_doc *doc) {
	D_ASSERT(yyjson_is_obj(frame.val));
	auto &node = *frame.node;
	if (node.incompatible) {
		return std::nullopt;
	}

	if (!frame.initialized) {
		if (!node.MergeType(LogicalTypeId::STRUCT, doc)) {
			return std::nullopt;
		}

		frame.obj_it = yyjson_obj_iter_with(frame.val);
		frame.initialized = true;
	}

	// Process scalar values without creating child frames.
	while (auto key = yyjson_obj_iter_next(&frame.obj_it)) {
		auto &child = GetOrCreateObjectChild(nodes, node, *key, doc);
		if (child.incompatible) {
			continue;
		}
		const auto val = yyjson_obj_iter_get_val(key);
		if (yyjson_is_ctn(val)) {
			return Frame(val, child);
		}
		child.MergeType(JSONCommon::ValTypeToLogicalTypeId(val), doc);
	}
	return std::nullopt;
}

void HandleValueFrame(Frame &frame, yyjson_mut_doc *doc) {
	const auto val = frame.val;
	D_ASSERT(!yyjson_is_arr(val) && !yyjson_is_obj(val));
	frame.node->MergeType(JSONCommon::ValTypeToLogicalTypeId(val), doc);
}

yyjson_mut_val *ExtractStructure(yyjson_val *val, yyjson_mut_doc *doc) {
	NodeStorage nodes;
	vector<Frame> stack;
	stack.reserve(64);

	auto &root = CreateStructureNode(nodes, doc);
	stack.emplace_back(val, root);

	while (!stack.empty()) {
		auto &frame = stack.back();

		switch (yyjson_get_tag(frame.val)) {
		case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE: {
			auto child = HandleArrayFrame(nodes, frame, doc);
			if (child) {
				stack.push_back(*child);
			} else {
				stack.pop_back();
			}
			break;
		}
		case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE: {
			auto child = HandleObjectFrame(nodes, frame, doc);
			if (child) {
				stack.push_back(*child);
			} else {
				stack.pop_back();
			}
			break;
		}
		default: {
			HandleValueFrame(frame, doc);
			stack.pop_back();
			break;
		}
		}
	}
	return root.value;
}

//===--------------------------------------------------------------------===//
// SQL function
//===--------------------------------------------------------------------===//
std::optional<string_view> JSONStructureFunction(yyjson_val *val, yyjson_alc *alc) {
	auto doc = yyjson_mut_doc_new(alc);
	if (!doc) {
		throw InternalException("Failed to allocate JSON structure document");
	}
	return JSONCommon::WriteVal<yyjson_mut_val>(ExtractStructure(val, doc), alc);
}

void StructureFunction(ScalarFunction::ExecInput &input) {
	JSONExecutors::UnaryExecute<string_view>(input, JSONStructureFunction);
}

} // namespace

void RegisterJSONStructure(Extension &extension, Context &context) {
	auto &factory = context.GetFactory();
	auto json = JSONType(factory);
	for (const bool json_input : {false, true}) {
		auto input = json_input ? JSONType(factory) : factory.CreateType(LogicalTypeId::VARCHAR);
		RegisterJSONReadFunction(extension, context, {"json_structure"}, JSONReadKind::UNARY, input, json,
		                         StructureFunction, FunctionFallibility::FALLIBLE);
	}

	// json_group_structure(x)
	auto group_structure = cxx::MacroFunction::CreateScalar(factory);
	group_structure.SetName("json_group_structure")
	    .SetBody("json_structure(json_group_array(x))->0")
	    .WithSignature(
	        [&](cxx::FunctionSignature &sig) { sig.AddParameter("x", factory.CreateType(LogicalTypeId::ANY)); });
	extension.Register(group_structure);
}

} // namespace json
} // namespace duckdb
