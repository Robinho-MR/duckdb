// json_each and json_tree: table in-out functions over a JSON document (and an optional path), whose rows an operator
// produces from the arguments.

#include "json_functions.hpp"

#include <atomic>

namespace duckdb {
namespace json {

using cxx::Operator;
using cxx::TableFunction;

namespace {

enum class JSONTableInOutType { EACH, TREE };

constexpr idx_t JSON_COLUMN_OFFSET = 0;
constexpr idx_t ROOT_COLUMN_OFFSET = 1;
constexpr idx_t JSON_COLUMN_ID = TableFunction::VIRTUAL_COLUMN_START + JSON_COLUMN_OFFSET;
constexpr idx_t ROOT_COLUMN_ID = TableFunction::VIRTUAL_COLUMN_START + ROOT_COLUMN_OFFSET;
constexpr idx_t INVALID_COLUMN = static_cast<idx_t>(-1);

void JSONTableInOutBind(TableFunction::BindInput &input) {
	auto &factory = input.GetContext().GetFactory();
	const auto varchar = factory.CreateType(LogicalTypeId::VARCHAR);
	const auto json = JSONType(factory);
	const auto ubigint = factory.CreateType(LogicalTypeId::UBIGINT);
	input.AddResultColumn("key", varchar);
	input.AddResultColumn("value", json);
	input.AddResultColumn("type", varchar);
	input.AddResultColumn("atom", json);
	input.AddResultColumn("id", ubigint);
	input.AddResultColumn("parent", ubigint);
	input.AddResultColumn("fullkey", varchar);
	input.AddResultColumn("path", varchar);

	input.AddVirtualColumn(JSON_COLUMN_ID, "json", json);
	input.AddVirtualColumn(ROOT_COLUMN_ID, "root", varchar);
	input.AddVirtualColumn(TableFunction::COLUMN_IDENTIFIER_EMPTY, "", factory.CreateType(LogicalTypeId::BOOLEAN));
	input.AddVirtualColumn(TableFunction::COLUMN_IDENTIFIER_ROW_ID, "rowid", factory.CreateType(LogicalTypeId::BIGINT));
}

struct JSONTableInOutGlobalState {
	explicit JSONTableInOutGlobalState(Operator::InitGlobalInput &input);

	//! The position in the output chunk of each column, or INVALID_COLUMN if the query does not use it
	idx_t key_column_index = INVALID_COLUMN;
	idx_t value_column_index = INVALID_COLUMN;
	idx_t type_column_index = INVALID_COLUMN;
	idx_t atom_column_index = INVALID_COLUMN;
	idx_t id_column_index = INVALID_COLUMN;
	idx_t parent_column_index = INVALID_COLUMN;
	idx_t fullkey_column_index = INVALID_COLUMN;
	idx_t path_column_index = INVALID_COLUMN;
	idx_t json_column_index = INVALID_COLUMN;
	idx_t root_column_index = INVALID_COLUMN;
	idx_t empty_column_index = INVALID_COLUMN;
	idx_t rowid_column_index = INVALID_COLUMN;

	//! Progress when used as a source (i.e. with a single input row): the rows that will be produced, and the rows that
	//! were produced
	std::atomic<idx_t> total_rows {0};
	std::atomic<idx_t> produced_rows {0};
};

void JSONTableInOutInitGlobal(Operator::InitGlobalInput &input) {
	// Input rows are independent, so the operator runs on as many threads as there are (no cap)
	input.SetGlobalState<JSONTableInOutGlobalState>(input);
}

JSONTableInOutGlobalState::JSONTableInOutGlobalState(Operator::InitGlobalInput &input) {
	auto &result = *this;
	for (idx_t i = 0; i < input.GetColumnCount(); i++) {
		const auto column = input.GetColumnIndex(i);
		if (column < TableFunction::VIRTUAL_COLUMN_START) {
			switch (column) {
			case 0:
				result.key_column_index = i;
				break;
			case 1:
				result.value_column_index = i;
				break;
			case 2:
				result.type_column_index = i;
				break;
			case 3:
				result.atom_column_index = i;
				break;
			case 4:
				result.id_column_index = i;
				break;
			case 5:
				result.parent_column_index = i;
				break;
			case 6:
				result.fullkey_column_index = i;
				break;
			case 7:
				result.path_column_index = i;
				break;
			default:
				throw NotImplementedException("Column %llu for json_each/json_tree",
				                              static_cast<unsigned long long>(column));
			}
		} else if (column == JSON_COLUMN_ID) {
			result.json_column_index = i;
		} else if (column == ROOT_COLUMN_ID) {
			result.root_column_index = i;
		} else if (column == TableFunction::COLUMN_IDENTIFIER_EMPTY) {
			result.empty_column_index = i;
		} else if (column == TableFunction::COLUMN_IDENTIFIER_ROW_ID) {
			result.rowid_column_index = i;
		} else {
			throw NotImplementedException("Virtual column %llu for json_each/json_tree",
			                              static_cast<unsigned long long>(column));
		}
	}
}

//! Whether an object key can appear unquoted in a JSON path (as in "$.key"). Mirrors SQLite's json_each/json_tree:
//! only keys consisting of an ASCII letter followed by ASCII alphanumerics/underscores go unquoted
bool JSONPathKeyNeedsQuoting(const char *data, const idx_t len) {
	const auto is_alpha = [](char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
	};
	if (len == 0 || !is_alpha(data[0])) {
		return true;
	}
	for (idx_t i = 1; i < len; i++) {
		if (!is_alpha(data[i]) && !(data[i] >= '0' && data[i] <= '9') && data[i] != '_') {
			return true;
		}
	}
	return false;
}

//! Appends ".key" to the path, quoting the key (as in "$.\"a.b\"") when needed so that the resulting path
//! round-trips through JSON path extraction
void AppendObjectPathElement(yyjson_val *vkey, string &path) {
	const auto data = unsafe_yyjson_get_str(vkey);
	const auto len = unsafe_yyjson_get_len(vkey);
	path += '.';
	if (!JSONPathKeyNeedsQuoting(data, len)) {
		path.append(data, len);
		return;
	}
	path += '"';
	for (idx_t i = 0; i < len; i++) {
		if (data[i] == '"' || data[i] == '\\') {
			path += '\\';
		}
		path += data[i];
	}
	path += '"';
}

struct JSONTableInOutRecursionNode {
	JSONTableInOutRecursionNode(string path_p, yyjson_val *parent_val_p)
	    : path(std::move(path_p)), parent_val(parent_val_p), child_index(0) {
	}

	string path;
	yyjson_val *parent_val;
	idx_t child_index;
};

struct JSONTableInOutLocalState {
	string GetPath() const {
		auto result = path;
		for (const auto &ri : recursion_nodes) {
			result += ri.path;
		}
		return result;
	}

	void AddRecursionNode(yyjson_val *val, yyjson_val *vkey, const idx_t arr_index, bool has_arr_index) {
		string str;
		if (vkey) {
			AppendObjectPathElement(vkey, str);
		} else if (has_arr_index) {
			str = "[" + std::to_string(arr_index) + "]";
		}
		recursion_nodes.emplace_back(std::move(str), val);
	}

	JSONAllocator json_allocator;
	//! The rows the current input row is estimated to produce, and the rows it produced so far
	idx_t estimated_rows = 0;
	idx_t produced_rows = 0;

	string path;
	idx_t len = 0;
	yyjson_doc *doc = nullptr;
	bool initialized = false;
	//! The input row currently being processed, and its document (for the "json" virtual column)
	idx_t input_row = 0;
	string json_doc;

	idx_t total_count = 0;
	vector<JSONTableInOutRecursionNode> recursion_nodes;
};

void JSONTableInOutInitLocal(Operator::InitLocalInput &input) {
	input.SetLocalState<JSONTableInOutLocalState>();
}

void JSONTableInOutProgress(Operator::ProgressInput &input) {
	auto gstate_ptr = input.TryGetGlobalState<JSONTableInOutGlobalState>();
	if (!gstate_ptr) {
		input.SetProgress(0);
		return;
	}
	auto &gstate = *gstate_ptr;
	const auto total_rows = gstate.total_rows.load(std::memory_order_relaxed);
	if (total_rows == 0) {
		input.SetProgress(0);
		return;
	}
	const auto produced_rows = std::min<idx_t>(gstate.produced_rows.load(std::memory_order_relaxed), total_rows);
	input.SetProgress(static_cast<double>(produced_rows) / static_cast<double>(total_rows));
}

//! The output vector of a column the query uses, written in order
template <class W>
struct OutputColumn {
	bool enabled = false;
	std::optional<Vector> vector;
	std::optional<W> writer;

	void Init(const Operator::ExecuteInput &input, idx_t index) {
		if (index == INVALID_COLUMN) {
			return;
		}
		enabled = true;
		vector.emplace(input.GetOutputChunk().GetVector(index));
		writer.emplace(*vector, STANDARD_VECTOR_SIZE);
	}
	W &operator*() {
		return *writer;
	}
};

struct JSONTableInOutResult {
	JSONTableInOutResult(const Operator::ExecuteInput &input, const JSONTableInOutGlobalState &gstate) {
		key.Init(input, gstate.key_column_index);
		value.Init(input, gstate.value_column_index);
		type.Init(input, gstate.type_column_index);
		atom.Init(input, gstate.atom_column_index);
		id.Init(input, gstate.id_column_index);
		parent.Init(input, gstate.parent_column_index);
		fullkey.Init(input, gstate.fullkey_column_index);
		path.Init(input, gstate.path_column_index);
		rowid.Init(input, gstate.rowid_column_index);
	}

	template <JSONTableInOutType TYPE>
	void AddRow(JSONTableInOutLocalState &lstate, yyjson_val *vkey, yyjson_val *val) {
		const auto &recursion_nodes = lstate.recursion_nodes;
		const auto arr_el = !recursion_nodes.empty() && unsafe_yyjson_is_arr(recursion_nodes.back().parent_val);
		if (key.enabled) {
			if (vkey) { // Object field
				(*key).WriteValue(string_view(unsafe_yyjson_get_str(vkey), unsafe_yyjson_get_len(vkey)));
			} else if (arr_el) { // Array element
				(*key).WriteValue(std::to_string(recursion_nodes.back().child_index));
			} else { // Other
				(*key).WriteNull();
			}
		}
		if (value.enabled) {
			(*value).WriteValue(JSONCommon::WriteVal(val, lstate.json_allocator.GetYYAlc()));
		}
		if (type.enabled) {
			(*type).WriteValue(JSONCommon::ValTypeToStringView(val));
		}
		if (atom.enabled) {
			auto atom_result = JSONCommon::JSONValue(val, lstate.json_allocator.GetYYAlc());
			if (atom_result.has_value()) {
				(*atom).WriteValue(atom_result.value());
			} else {
				(*atom).WriteNull();
			}
		}
		if (id.enabled) {
			(*id).WriteValue(static_cast<uint64_t>(val - lstate.doc->root));
		}
		if (parent.enabled) {
			if (TYPE == JSONTableInOutType::EACH || recursion_nodes.empty()) {
				(*parent).WriteNull();
			} else {
				(*parent).WriteValue(static_cast<uint64_t>(recursion_nodes.back().parent_val - lstate.doc->root));
			}
		}
		if (fullkey.enabled || path.enabled) {
			auto path_str = lstate.GetPath();
			if (path.enabled) {
				(*path).WriteValue(path_str);
			}
			if (fullkey.enabled) {
				if (vkey) { // Object field
					AppendObjectPathElement(vkey, path_str);
				} else if (arr_el) { // Array element
					path_str += "[" + std::to_string(recursion_nodes.back().child_index) + "]";
				}
				(*fullkey).WriteValue(path_str);
			}
		}
		if (rowid.enabled) {
			(*rowid).WriteValue(static_cast<int64_t>(lstate.total_count++));
		}
		count++;
	}

	idx_t count = 0;
	OutputColumn<StringWriter> key;
	OutputColumn<StringWriter> value;
	OutputColumn<StringWriter> type;
	OutputColumn<StringWriter> atom;
	OutputColumn<FlatWriter<uint64_t>> id;
	OutputColumn<FlatWriter<uint64_t>> parent;
	OutputColumn<StringWriter> fullkey;
	OutputColumn<StringWriter> path;
	OutputColumn<FlatWriter<int64_t>> rowid;
};

template <JSONTableInOutType TYPE>
void InitializeLocalState(Operator::ExecuteInput &input, JSONTableInOutGlobalState &gstate,
                          JSONTableInOutLocalState &lstate, JSONTableInOutResult &result) {
	lstate.total_count = 0;
	lstate.doc = nullptr;
	lstate.path.clear();
	lstate.len = 0;
	lstate.json_doc.clear();
	auto in = input.GetInputChunk();
	auto &factory = input.GetContext().GetFactory();

	// Parse path, default to root if not given
	auto path_value = Value::Create(factory, varchar_t(string_view("$")));
	if (in.GetVectorCount() > 1) {
		const ValueView<varchar_t> path_values(in.GetVector(1));
		if (!path_values.IsValid(lstate.input_row)) {
			return;
		}
		path_value = Value::Create(factory, path_values.Get(lstate.input_row));
	}

	if (JSONReadFunctionData::CheckPath(factory, path_value, lstate.path, lstate.len) ==
	    JSONCommon::JSONPathType::WILDCARD) {
		throw BinderException("Wildcard JSON path not supported in json_each/json_tree");
	}

	if (lstate.path.c_str()[0] != '$') {
		throw BinderException("JSON path must start with '$' for json_each/json_tree");
	}

	// Parse document and get the value at the supplied path
	const ValueView<varchar_t> input_values(in.GetVector(0));
	if (!input_values.IsValid(lstate.input_row)) {
		return;
	}
	auto input_data = input_values.Get(lstate.input_row);
	if (gstate.json_column_index != INVALID_COLUMN) {
		// Copy the document only when the "json" virtual column is selected
		lstate.json_doc = string(input_data.view());
	}
	lstate.doc = JSONCommon::ReadDocument(input_data, JSONCommon::READ_FLAG, lstate.json_allocator.GetYYAlc());
	const auto root = JSONCommon::GetUnsafe(lstate.doc->root, lstate.path.c_str(), lstate.len);

	if (!root) {
		return;
	}

	const auto is_container = unsafe_yyjson_is_arr(root) || unsafe_yyjson_is_obj(root);
	// json_each produces a row per element of the root, json_tree a row per value (the value count is an upper bound)
	if (TYPE == JSONTableInOutType::TREE) {
		lstate.estimated_rows = yyjson_doc_get_val_count(lstate.doc);
	} else {
		lstate.estimated_rows = is_container ? unsafe_yyjson_get_len(root) : 1;
	}
	lstate.produced_rows = 0;
	gstate.total_rows.fetch_add(lstate.estimated_rows, std::memory_order_relaxed);
	if (!is_container || TYPE == JSONTableInOutType::TREE) {
		result.AddRow<TYPE>(lstate, nullptr, root);
	}
	if (is_container) {
		lstate.AddRecursionNode(root, nullptr, 0, false);
	}
}

template <JSONTableInOutType TYPE>
bool JSONTableInOutHandleValue(JSONTableInOutLocalState &lstate, JSONTableInOutResult &result, idx_t &child_index,
                               size_t &idx, yyjson_val *child_key, yyjson_val *child_val) {
	if (idx < child_index) {
		return false; // Continue: Get back to where we left off
	}
	result.AddRow<TYPE>(lstate, child_key, child_val);
	child_index++; // We finished processing the array element
	if (TYPE == JSONTableInOutType::TREE && (unsafe_yyjson_is_arr(child_val) || unsafe_yyjson_is_obj(child_val))) {
		lstate.AddRecursionNode(child_val, child_key, idx, true);
		return true; // Break: We added a recursion node, go depth-first
	}
	if (result.count == STANDARD_VECTOR_SIZE) {
		return true; // Break: Vector is full
	}
	return false; // Continue: Next element
}

//! Gives a constant string column (the "json" and "root" virtual columns) its value in every row
void FillConstantString(Vector vector, idx_t count, string_view value) {
	vector.SetSize(count);
	if (count == 0) {
		return;
	}
	vector.AssignStringUnsafe(0, value);
	auto data = vector.GetDataMutable<varchar_t>();
	for (idx_t i = 1; i < count; i++) {
		data[i] = data[0];
	}
	if (vector.GetView().validity) {
		vector.GetValidityMutable().SetAllValid(count);
	}
}

template <JSONTableInOutType TYPE>
void JSONTableInOutFunction(Operator::ExecuteInput &input) {
	auto &gstate = input.GetGlobalState<JSONTableInOutGlobalState>();
	auto &lstate = input.GetLocalState<JSONTableInOutLocalState>();
	auto in = input.GetInputChunk();
	auto out = input.GetOutputChunk();

	JSONTableInOutResult result(input, gstate);
	if (!lstate.initialized) {
		if (lstate.input_row >= in.GetRowCount()) {
			for (idx_t c = 0; c < input.GetColumnCount(); c++) {
				out.GetVector(c).SetSize(0);
			}
			input.SetResult(Operator::Result::NEED_MORE_INPUT);
			return;
		}
		InitializeLocalState<TYPE>(input, gstate, lstate, result);
		lstate.initialized = true;
	}

	// Traverse the JSON (keeping a stack to avoid recursion and save progress across calls)
	auto &recursion_nodes = lstate.recursion_nodes;
	while (!lstate.recursion_nodes.empty() && result.count != STANDARD_VECTOR_SIZE) {
		auto &parent_val = recursion_nodes.back().parent_val;
		auto &child_index = recursion_nodes.back().child_index;

		size_t idx, max;
		yyjson_val *child_key, *child_val;
		switch (yyjson_get_tag(parent_val)) {
		case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE:
			yyjson_arr_foreach(parent_val, idx, max, child_val) {
				if (JSONTableInOutHandleValue<TYPE>(lstate, result, child_index, idx, nullptr, child_val)) {
					break;
				}
			}
			break;
		case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE:
			yyjson_obj_foreach(parent_val, idx, max, child_key, child_val) {
				if (JSONTableInOutHandleValue<TYPE>(lstate, result, child_index, idx, child_key, child_val)) {
					break;
				}
			}
			break;
		default:
			throw InternalException("Non-object/array JSON added to recursion in json_each/json_tree");
		}
		if (idx == max) {
			lstate.recursion_nodes.pop_back(); // Array/object is done, remove
		}
	}
	const auto count = result.count;
	for (idx_t c = 0; c < input.GetColumnCount(); c++) {
		out.GetVector(c).SetSize(count);
	}
	lstate.produced_rows += count;
	gstate.produced_rows.fetch_add(count, std::memory_order_relaxed);

	// Set constant virtual columns ("json" and "root")
	if (gstate.json_column_index != INVALID_COLUMN) {
		FillConstantString(out.GetVector(gstate.json_column_index), count, lstate.json_doc);
	}
	if (gstate.root_column_index != INVALID_COLUMN) {
		FillConstantString(out.GetVector(gstate.root_column_index), count,
		                   string_view(lstate.path.c_str(), lstate.len));
	}
	if (gstate.empty_column_index != INVALID_COLUMN) {
		auto empty_vector = out.GetVector(gstate.empty_column_index);
		empty_vector.SetSize(count);
	}

	if (count == 0) {
		D_ASSERT(recursion_nodes.empty());
		// the input row is done - correct the estimate to the rows that were actually produced
		if (lstate.estimated_rows > lstate.produced_rows) {
			gstate.total_rows.fetch_sub(lstate.estimated_rows - lstate.produced_rows, std::memory_order_relaxed);
		} else {
			gstate.total_rows.fetch_add(lstate.produced_rows - lstate.estimated_rows, std::memory_order_relaxed);
		}
		lstate.estimated_rows = 0;
		lstate.produced_rows = 0;
		lstate.json_allocator.Reset();
		lstate.doc = nullptr;
		lstate.initialized = false;
		lstate.input_row++;
		if (lstate.input_row < in.GetRowCount()) {
			// start on the next input row with the next output chunk
			input.SetResult(Operator::Result::HAVE_MORE_OUTPUT);
			return;
		}
		lstate.input_row = 0;
		input.SetResult(Operator::Result::NEED_MORE_INPUT);
		return;
	}
	input.SetResult(Operator::Result::HAVE_MORE_OUTPUT);
}

void RegisterJSONTableInOutOverload(Extension &extension, Context &context, Operator &op, const char *name,
                                    const LogicalType &input_type, bool has_path_param) {
	auto &factory = context.GetFactory();
	auto function = TableFunction::Create(factory);
	function.SetName(name);
	function.SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
	function.GetSignature().AddParameter("json", input_type);
	if (has_path_param) {
		function.GetSignature().AddParameter("path", factory.CreateType(LogicalTypeId::VARCHAR));
	}
	function.SetBindCallback(JSONTableInOutBind)
	    .SetProjectionPushdown(true)
	    .SetOperator(op, TableFunction::OperatorInput::ARGUMENTS);
	extension.Register(function);
}

template <JSONTableInOutType TYPE>
void RegisterJSONTableInOutFunction(Extension &extension, Context &context, const char *name) {
	auto &factory = context.GetFactory();
	const auto varchar = factory.CreateType(LogicalTypeId::VARCHAR);
	const auto json = JSONType(factory);
	auto op = Operator::Create(factory);
	op.SetInitGlobalCallback(JSONTableInOutInitGlobal)
	    .SetInitLocalCallback(JSONTableInOutInitLocal)
	    .SetExecuteCallback(JSONTableInOutFunction<TYPE>)
	    .SetProgressCallback(JSONTableInOutProgress);
	RegisterJSONTableInOutOverload(extension, context, op, name, varchar, false);
	RegisterJSONTableInOutOverload(extension, context, op, name, varchar, true);
	RegisterJSONTableInOutOverload(extension, context, op, name, json, false);
	RegisterJSONTableInOutOverload(extension, context, op, name, json, true);
}

} // namespace

void RegisterTableInOut(Extension &extension, Context &context) {
	RegisterJSONTableInOutFunction<JSONTableInOutType::EACH>(extension, context, "json_each");
	RegisterJSONTableInOutFunction<JSONTableInOutType::TREE>(extension, context, "json_tree");
}

} // namespace json
} // namespace duckdb
