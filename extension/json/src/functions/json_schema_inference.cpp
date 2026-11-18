// Ports legacy/json_schema_inference.cpp

#include "json_schema_inference.hpp"

#include <limits>

namespace duckdb {
namespace json {

//===--------------------------------------------------------------------===//
// Numeric types
//===--------------------------------------------------------------------===//
bool JSONSchemaInference::IsNumericType(LogicalTypeId type) {
	return type == LogicalTypeId::DOUBLE || type == LogicalTypeId::UBIGINT || type == LogicalTypeId::BIGINT ||
	       type == LogicalTypeId::HUGEINT;
}

LogicalTypeId JSONSchemaInference::MaxNumericType(LogicalTypeId a, LogicalTypeId b) {
	D_ASSERT(a != b);
	if (a == LogicalTypeId::DOUBLE || b == LogicalTypeId::DOUBLE) {
		return LogicalTypeId::DOUBLE;
	}
	if (a == LogicalTypeId::HUGEINT || b == LogicalTypeId::HUGEINT) {
		return LogicalTypeId::HUGEINT;
	}
	// One is BIGINT and the other is UBIGINT - need HUGEINT to represent both ranges
	if ((a == LogicalTypeId::BIGINT && b == LogicalTypeId::UBIGINT) ||
	    (a == LogicalTypeId::UBIGINT && b == LogicalTypeId::BIGINT)) {
		return LogicalTypeId::HUGEINT;
	}
	return LogicalTypeId::BIGINT;
}

//===--------------------------------------------------------------------===//
// JSONStringTypeTester
//===--------------------------------------------------------------------===//
JSONStringTypeTester::JSONStringTypeTester(Factory &factory_p, JSONInferenceDateFormats *date_formats_p,
                                           const JSONInferenceOptions &options)
    : factory(factory_p), date_formats(date_formats_p),
      user_specified_timestamp_format(options.user_specified_timestamp_format) {
}

Vector JSONStringTypeTester::LoadedStrings() {
	if (!string_chunk) {
		vector<LogicalType> types;
		types.push_back(factory.CreateType(LogicalTypeId::VARCHAR));
		string_chunk.emplace(factory.CreateDataChunk(types));
	}
	return string_chunk->GetVector(0);
}

void JSONStringTypeTester::LoadStrings(yyjson_val *vals[], idx_t count_p) {
	count = count_p;
	LoadedStrings();
	string_chunk->Clear();
	auto strings = LoadedStrings();
	StringWriter writer(strings, count);
	for (idx_t i = 0; i < count; i++) {
		const auto val = vals[i];
		if (!val || !unsafe_yyjson_is_str(val)) {
			writer.WriteNull();
			continue;
		}
		writer.WriteValue(string_view(unsafe_yyjson_get_str(val), unsafe_yyjson_get_len(val)));
	}
}

bool JSONStringTypeTester::TryCast(LogicalTypeId type) {
	if (count == 0) {
		return true;
	}
	auto result = ResultVector(type);
	if (!LoadedStrings().TryCast(factory, result)) {
		return false;
	}
	if (type == LogicalTypeId::UUID) {
		// The detection casts strictly: only the canonical form with hyphens is a UUID
		auto strings = LoadedStrings();
		ValueView<varchar_t> values(strings);
		for (idx_t i = 0; i < count; i++) {
			if (!values.IsValid(i)) {
				continue;
			}
			const auto value = values.Get(i);
			const auto text = value.view();
			if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' || text[23] != '-') {
				return false;
			}
		}
	}
	if (type == LogicalTypeId::TIME) {
		// The detection casts strictly: a TIME is HH:MM[:SS], not a date or timestamp that happens to start a time
		auto strings = LoadedStrings();
		ValueView<varchar_t> values(strings);
		for (idx_t i = 0; i < count; i++) {
			if (!values.IsValid(i)) {
				continue;
			}
			const auto value = values.Get(i);
			const auto text = value.view();
			idx_t pos = 0;
			while (pos < text.size() && (text[pos] == ' ' || (text[pos] >= '\t' && text[pos] <= '\r'))) {
				pos++;
			}
			const auto digits_start = pos;
			while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
				pos++;
			}
			if (pos == digits_start || pos >= text.size() || text[pos] != ':') {
				return false;
			}
		}
	}
	return true;
}

Vector JSONStringTypeTester::ResultVector(LogicalTypeId type) {
	auto entry = result_chunks.find(static_cast<uint32_t>(type));
	if (entry == result_chunks.end()) {
		vector<LogicalType> types;
		types.push_back(factory.CreateType(type));
		entry = result_chunks.emplace(static_cast<uint32_t>(type), factory.CreateDataChunk(types)).first;
	}
	entry->second.Clear();
	return entry->second.GetVector(0);
}

// Date::TryConvertDate reuses has_offset for specials; only Z / +-HH[:MM] is a real offset.
static bool TimestampStringHasUtcOffset(string_view value) {
	const auto str = value.data();
	const auto len = value.size();
	idx_t time_start = 0;
	for (idx_t i = 0; i < len; i++) {
		if (str[i] == 'T' || str[i] == ' ') {
			time_start = i + 1;
			break;
		}
	}
	if (time_start == 0 || time_start >= len) {
		return false;
	}
	for (idx_t i = time_start; i < len; i++) {
		if (str[i] == 'Z') {
			return true;
		}
		if (str[i] == '+' || str[i] == '-') {
			// HH, optionally followed by [:]MM
			idx_t pos = i + 1;
			if (pos >= len || str[pos] < '0' || str[pos] > '9') {
				return false;
			}
			pos++;
			if (pos < len && str[pos] >= '0' && str[pos] <= '9') {
				pos++;
			}
			if (pos < len && str[pos] == ':') {
				pos++;
				return pos < len && str[pos] >= '0' && str[pos] <= '9';
			}
			return true;
		}
	}
	return false;
}

// Single commutative transition table, shared by per-vector refinement and parallel merges,
// so thread scheduling cannot change the detected type. UNKNOWN is the identity, MIXED is sticky.
static TimestampOffsetState MergeTimestampOffsetState(const TimestampOffsetState a, const TimestampOffsetState b) {
	if (a == TimestampOffsetState::UNKNOWN) {
		return b;
	}
	if (b == TimestampOffsetState::UNKNOWN) {
		return a;
	}
	if (a != b) {
		return TimestampOffsetState::MIXED;
	}
	return a;
}

// Update the sticky offset state from this vector's non-null strings.
// MIXED -> VARCHAR: neither TIMESTAMPTZ (would invent session TZ for naive values)
// nor TIMESTAMP (would drop offsets) is safe.
void JSONStringTypeTester::UpdateTimestampOffsetState(TimestampOffsetState &state) {
	if (count == 0) {
		return;
	}
	// The strings that are timestamps are those that convert to TIMESTAMPTZ
	auto timestamps = ResultVector(LogicalTypeId::TIMESTAMP_TZ);
	auto strings = LoadedStrings();
	strings.TryCast(factory, timestamps);

	ValueView<varchar_t> input(strings);
	ValueView<int64_t> parsed(timestamps);
	for (idx_t i = 0; i < count; i++) {
		if (!input.IsValid(i)) {
			continue; // nulls leave the state unchanged
		}
		if (!parsed.IsValid(i)) {
			continue; // not a timestamp string -- other candidates handle it
		}
		const auto ts = parsed.Get(i);
		if (ts == std::numeric_limits<int64_t>::max() || ts == -std::numeric_limits<int64_t>::max()) {
			continue; // +-infinity is valid for TIMESTAMP and TIMESTAMPTZ
		}
		// the view of an inlined string points into the string itself, so it needs to outlive the view
		const auto value = input.Get(i);
		const auto text = value.view();
		if (text.find_first_of("0123456789") == string_view::npos) {
			continue; // specials such as epoch carry no offset
		}
		const auto has_offset = TimestampStringHasUtcOffset(text);
		state = MergeTimestampOffsetState(state, has_offset ? TimestampOffsetState::WITH_OFFSET
		                                                    : TimestampOffsetState::WITHOUT_OFFSET);
		if (state == TimestampOffsetState::MIXED) {
			return;
		}
	}
}

//===--------------------------------------------------------------------===//
// JSONStructureNode
//===--------------------------------------------------------------------===//
JSONStructureNode::JSONStructureNode(const bool case_insensitive_keys_p)
    : count(0), null_count(0), case_insensitive_keys(case_insensitive_keys_p) {
}

JSONStructureNode::JSONStructureNode(const char *key_ptr, const size_t key_len, const bool case_insensitive_keys_p)
    : JSONStructureNode(case_insensitive_keys_p) {
	key = make_uniq<string>(key_ptr, key_len);
}

JSONStructureNode::~JSONStructureNode() = default;

JSONStructureNode JSONStructureNode::EmptyLike(const JSONStructureNode &parent) {
	return JSONStructureNode(parent.case_insensitive_keys);
}

static void SwapJSONStructureNode(JSONStructureNode &a, JSONStructureNode &b) noexcept {
	std::swap(a.key, b.key);
	std::swap(a.initialized, b.initialized);
	std::swap(a.descriptions, b.descriptions);
	std::swap(a.count, b.count);
	std::swap(a.null_count, b.null_count);
	std::swap(a.case_insensitive_keys, b.case_insensitive_keys);
}

JSONStructureNode::JSONStructureNode(JSONStructureNode &&other) noexcept : count(0), null_count(0) {
	SwapJSONStructureNode(*this, other);
}

JSONStructureNode &JSONStructureNode::operator=(JSONStructureNode &&other) noexcept {
	SwapJSONStructureNode(*this, other);
	return *this;
}

JSONStructureDescription &JSONStructureNode::GetOrCreateDescription(const LogicalTypeId type) {
	if (descriptions.empty()) {
		// Empty, just put this type in there
		descriptions.emplace_back(type, case_insensitive_keys);
		return descriptions.back();
	}

	if (descriptions.size() == 1 && descriptions[0].type == LogicalTypeId::SQLNULL) {
		// Only a NULL in there, override
		descriptions[0].type = type;
		return descriptions[0];
	}

	if (type == LogicalTypeId::SQLNULL) {
		// 'descriptions' is non-empty, so let's not add NULL
		return descriptions.back();
	}

	// Check if type is already in there or if we can merge numerics
	const auto is_numeric = JSONSchemaInference::IsNumericType(type);
	for (auto &description : descriptions) {
		if (type == description.type) {
			return description;
		}
		if (is_numeric && JSONSchemaInference::IsNumericType(description.type)) {
			description.type = JSONSchemaInference::MaxNumericType(type, description.type);
			return description;
		}
	}
	// Type was not there, create a new description
	descriptions.emplace_back(type, case_insensitive_keys);
	return descriptions.back();
}

bool JSONStructureNode::ContainsVarchar() const {
	if (descriptions.size() != 1) {
		// We can't refine types if we have more than 1 description (yet), defaults to JSON type for now
		return false;
	}
	auto &description = descriptions[0];
	if (description.type == LogicalTypeId::VARCHAR) {
		return true;
	}
	for (auto &child : description.children) {
		if (child.ContainsVarchar()) {
			return true;
		}
	}

	return false;
}

void JSONStructureNode::InitializeCandidateTypes(const JSONInferenceOptions &options, const idx_t depth) {
	if (depth >= options.max_depth) {
		return;
	}
	if (descriptions.size() != 1) {
		// We can't refine types if we have more than 1 description (yet), defaults to JSON type for now
		return;
	}
	auto &description = descriptions[0];
	if (description.type == LogicalTypeId::VARCHAR && !initialized) {
		// We loop through the candidate types and format templates from back to front.
		// TIMESTAMP_TZ is only a candidate when the user did not pass timestamp_format=; never infer that
		// from the date-format map (auto-detect can also converge on a single format).
		// Trial order puts TIMESTAMP_TZ before TIMESTAMP so back-to-front tries TZ first (issue #14919).
		const bool user_format = options.user_specified_timestamp_format;
		if (options.convert_strings_to_integers) {
			if (user_format) {
				description.candidate_types = {LogicalTypeId::UUID, LogicalTypeId::BIGINT, LogicalTypeId::TIMESTAMP,
				                               LogicalTypeId::DATE, LogicalTypeId::TIME};
			} else {
				description.candidate_types = {LogicalTypeId::UUID,      LogicalTypeId::BIGINT,
				                               LogicalTypeId::TIMESTAMP, LogicalTypeId::TIMESTAMP_TZ,
				                               LogicalTypeId::DATE,      LogicalTypeId::TIME};
			}
		} else if (user_format) {
			description.candidate_types = {LogicalTypeId::UUID, LogicalTypeId::TIMESTAMP, LogicalTypeId::DATE,
			                               LogicalTypeId::TIME};
		} else {
			description.candidate_types = {LogicalTypeId::UUID, LogicalTypeId::TIMESTAMP, LogicalTypeId::TIMESTAMP_TZ,
			                               LogicalTypeId::DATE, LogicalTypeId::TIME};
		}
		initialized = true;
	} else {
		for (auto &child : description.children) {
			child.InitializeCandidateTypes(options, depth + 1);
		}
	}
}

void JSONStructureNode::RefineCandidateTypes(yyjson_val *vals[], const idx_t val_count, JSONStringTypeTester &tester) {
	if (descriptions.size() != 1) {
		// We can't refine types if we have more than 1 description (yet), defaults to JSON type for now
		return;
	}
	if (!ContainsVarchar()) {
		return;
	}
	auto &description = descriptions[0];
	switch (description.type) {
	case LogicalTypeId::LIST:
		return RefineCandidateTypesArray(vals, val_count, tester);
	case LogicalTypeId::STRUCT:
		return RefineCandidateTypesObject(vals, val_count, tester);
	case LogicalTypeId::VARCHAR:
		return RefineCandidateTypesString(vals, val_count, tester);
	default:
		return;
	}
}

void JSONStructureNode::RefineCandidateTypesArray(yyjson_val *vals[], const idx_t val_count,
                                                  JSONStringTypeTester &tester) {
	D_ASSERT(descriptions.size() == 1 && descriptions[0].type == LogicalTypeId::LIST);
	auto &desc = descriptions[0];
	D_ASSERT(desc.children.size() == 1);
	auto &child = desc.children[0];

	idx_t total_list_size = 0;
	for (idx_t i = 0; i < val_count; i++) {
		if (vals[i] && !unsafe_yyjson_is_null(vals[i])) {
			D_ASSERT(yyjson_is_arr(vals[i]));
			total_list_size += unsafe_yyjson_get_len(vals[i]);
		}
	}

	vector<yyjson_val *> child_vals;
	child_vals.reserve(total_list_size);

	size_t idx, max;
	yyjson_val *child_val;
	for (idx_t i = 0; i < val_count; i++) {
		if (vals[i] && !unsafe_yyjson_is_null(vals[i])) {
			yyjson_arr_foreach(vals[i], idx, max, child_val) {
				child_vals.push_back(child_val);
			}
		}
	}
	child.RefineCandidateTypes(child_vals.data(), total_list_size, tester);
}

void JSONStructureNode::RefineCandidateTypesObject(yyjson_val *vals[], const idx_t val_count,
                                                   JSONStringTypeTester &tester) {
	D_ASSERT(descriptions.size() == 1 && descriptions[0].type == LogicalTypeId::STRUCT);
	auto &desc = descriptions[0];

	const idx_t child_count = desc.children.size();
	vector<vector<yyjson_val *>> child_vals(child_count);
	for (auto &child_val_vector : child_vals) {
		child_val_vector.resize(val_count);
	}
	vector<char> found_keys(child_count);

	const auto &key_map = desc.key_map;
	size_t idx, max;
	yyjson_val *child_key, *child_val;
	for (idx_t i = 0; i < val_count; i++) {
		if (vals[i] && !unsafe_yyjson_is_null(vals[i])) {
			idx_t found_key_count = 0;
			std::fill(found_keys.begin(), found_keys.end(), false);

			D_ASSERT(yyjson_is_obj(vals[i]));
			yyjson_obj_foreach(vals[i], idx, max, child_key, child_val) {
				D_ASSERT(yyjson_is_str(child_key));
				const auto key_ptr = unsafe_yyjson_get_str(child_key);
				const auto key_len = unsafe_yyjson_get_len(child_key);
				auto it = key_map.find({key_ptr, key_len});
				D_ASSERT(it != key_map.end());
				const auto child_idx = it->second;
				child_vals[child_idx][i] = child_val;
				found_key_count += !found_keys[child_idx];
				found_keys[child_idx] = true;
			}

			if (found_key_count != child_count) {
				// Set child val to nullptr so recursion doesn't break
				for (idx_t child_idx = 0; child_idx < child_count; child_idx++) {
					if (!found_keys[child_idx]) {
						child_vals[child_idx][i] = nullptr;
					}
				}
			}
		} else {
			for (idx_t child_idx = 0; child_idx < child_count; child_idx++) {
				child_vals[child_idx][i] = nullptr;
			}
		}
	}

	for (idx_t child_idx = 0; child_idx < child_count; child_idx++) {
		desc.children[child_idx].RefineCandidateTypes(child_vals[child_idx].data(), val_count, tester);
	}
}

void JSONStructureNode::RefineCandidateTypesString(yyjson_val *vals[], const idx_t val_count,
                                                   JSONStringTypeTester &tester) {
	D_ASSERT(descriptions.size() == 1 && descriptions[0].type == LogicalTypeId::VARCHAR);
	if (descriptions[0].candidate_types.empty()) {
		return;
	}
	tester.LoadStrings(vals, val_count);
	EliminateCandidateTypes(val_count, tester);
}

void JSONStructureNode::EliminateCandidateTypes(const idx_t vec_count, JSONStringTypeTester &tester) {
	D_ASSERT(descriptions.size() == 1 && descriptions[0].type == LogicalTypeId::VARCHAR);
	auto &description = descriptions[0];
	auto &candidate_types = description.candidate_types;
	const bool user_specified_timestamp_format = tester.UserSpecifiedTimestampFormat();

	// Monotonic across vectors: once MIXED, refuse to guess.
	if (!user_specified_timestamp_format) {
		tester.UpdateTimestampOffsetState(description.timestamp_offset_state);
		if (description.timestamp_offset_state == TimestampOffsetState::MIXED) {
			candidate_types.clear();
			return;
		}
	}

	auto date_formats = tester.GetDateFormats();
	while (true) {
		if (candidate_types.empty()) {
			return;
		}
		const auto type = candidate_types.back();
		if (type == LogicalTypeId::TIMESTAMP_TZ) {
			// Defense in depth: InitializeCandidateTypes already omits TZ when the user set timestamp_format=.
			if (user_specified_timestamp_format) {
				candidate_types.pop_back();
				continue;
			}
			// Only accept TIMESTAMPTZ when every non-null value seen so far carries an offset/Z.
			// UNKNOWN (e.g., null-only vectors) and WITHOUT_OFFSET drop TZ and try TIMESTAMP (etc.).
			if (description.timestamp_offset_state != TimestampOffsetState::WITH_OFFSET) {
				candidate_types.pop_back();
				continue;
			}
			if (!tester.TryCast(type)) {
				candidate_types.pop_back();
			} else {
				return;
			}
		} else if (date_formats && date_formats->HasFormats(type)) {
			if (EliminateCandidateFormats(vec_count, type, tester)) {
				return;
			} else {
				candidate_types.pop_back();
			}
		} else {
			if (!tester.TryCast(type)) {
				candidate_types.pop_back();
			} else {
				return;
			}
		}
	}
}

bool JSONStructureNode::EliminateCandidateFormats(const idx_t vec_count, LogicalTypeId type,
                                                  JSONStringTypeTester &tester) {
	D_ASSERT(descriptions.size() == 1 && descriptions[0].type == LogicalTypeId::VARCHAR);
	if (type != LogicalTypeId::DATE && type != LogicalTypeId::TIMESTAMP) {
		throw InternalException("No date/timestamp formats for type %d", static_cast<int>(type));
	}
	return tester.GetDateFormats()->MatchesAnyFormat(type, tester.LoadedStrings(), vec_count);
}

//===--------------------------------------------------------------------===//
// JSONStructureDescription
//===--------------------------------------------------------------------===//
JSONStructureDescription::JSONStructureDescription(const LogicalTypeId type_p, const bool case_insensitive_keys)
    : type(type_p),
      key_map(0, JSONKeyOptionalCaseHash {case_insensitive_keys}, JSONKeyOptionalCaseEquality {case_insensitive_keys}) {
}

JSONStructureDescription::~JSONStructureDescription() = default;

static void SwapJSONStructureDescription(JSONStructureDescription &a, JSONStructureDescription &b) noexcept {
	std::swap(a.type, b.type);
	std::swap(a.key_map, b.key_map);
	std::swap(a.children, b.children);
	std::swap(a.candidate_types, b.candidate_types);
	std::swap(a.has_large_ubigint, b.has_large_ubigint);
	std::swap(a.timestamp_offset_state, b.timestamp_offset_state);
}

JSONStructureDescription::JSONStructureDescription(JSONStructureDescription &&other) noexcept {
	SwapJSONStructureDescription(*this, other);
}

JSONStructureDescription &JSONStructureDescription::operator=(JSONStructureDescription &&other) noexcept {
	SwapJSONStructureDescription(*this, other);
	return *this;
}

JSONStructureNode &JSONStructureDescription::GetOrCreateChild() {
	D_ASSERT(type == LogicalTypeId::LIST);
	if (children.empty()) {
		children.emplace_back(key_map.hash_function().case_insensitive);
	}
	D_ASSERT(children.size() == 1);
	return children.back();
}

JSONStructureNode &JSONStructureDescription::GetOrCreateChild(const char *key_ptr, const size_t key_size) {
	// Check if there is already a child with the same key
	const JSONKey temp_key {key_ptr, key_size};
	const auto it = key_map.find(temp_key);
	if (it != key_map.end()) {
		return children[it->second]; // Found it
	}

	// Didn't find, create a new child (the first spelling of a key names it when keys are case-insensitive)
	children.emplace_back(key_ptr, key_size, key_map.hash_function().case_insensitive);
	const auto &persistent_key_string = *children.back().key;
	JSONKey new_key {persistent_key_string.c_str(), persistent_key_string.length()};
	key_map.emplace(new_key, children.size() - 1);
	return children.back();
}

JSONStructureNode &JSONStructureDescription::GetOrCreateChild(yyjson_val *key, yyjson_val *val,
                                                              const JSONInferenceOptions &options, const idx_t depth) {
	D_ASSERT(yyjson_is_str(key));
	auto &child = GetOrCreateChild(unsafe_yyjson_get_str(key), unsafe_yyjson_get_len(key));
	JSONStructure::ExtractStructure(val, child, options, depth);
	return child;
}

//===--------------------------------------------------------------------===//
// Extracting the structure of documents
//===--------------------------------------------------------------------===//
static void ExtractStructureArray(yyjson_val *arr, JSONStructureNode &node, const JSONInferenceOptions &options,
                                  const idx_t depth) {
	D_ASSERT(yyjson_is_arr(arr));
	auto &description = node.GetOrCreateDescription(LogicalTypeId::LIST);
	auto &child = description.GetOrCreateChild();

	size_t idx, max;
	yyjson_val *val;
	yyjson_arr_foreach(arr, idx, max, val) {
		JSONStructure::ExtractStructure(val, child, options, depth + 1);
	}
}

static string LowerCaseKey(const string &key) {
	string result = key;
	for (auto &c : result) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return result;
}

static void ExtractStructureObject(yyjson_val *obj, JSONStructureNode &node, const JSONInferenceOptions &options,
                                   const idx_t depth) {
	D_ASSERT(yyjson_is_obj(obj));
	auto &description = node.GetOrCreateDescription(LogicalTypeId::STRUCT);

	// Keep track of keys so we can detect duplicates
	unordered_set<string> obj_keys;
	unordered_map<string, string> ci_obj_keys;

	size_t idx, max;
	yyjson_val *key, *val;
	yyjson_obj_foreach(obj, idx, max, key, val) {
		const string obj_key(unsafe_yyjson_get_str(key), unsafe_yyjson_get_len(key));
		auto insert_result = obj_keys.insert(obj_key);
		if (!options.ignore_errors && !insert_result.second) { // Exact match
			JSONCommon::ThrowValFormatError("Duplicate key \"" + obj_key + "\" in object %s", obj);
		}
		auto ci_insert_result = ci_obj_keys.emplace(LowerCaseKey(obj_key), obj_key);
		if (!options.ignore_errors && !ci_insert_result.second) { // Case-insensitive match
			JSONCommon::ThrowValFormatError("Duplicate key (different case) \"" + obj_key + "\" and \"" +
			                                    ci_insert_result.first->second + "\" in object %s",
			                                obj);
		}
		description.GetOrCreateChild(key, val, options, depth + 1);
	}
}

static void ExtractStructureVal(yyjson_val *val, JSONStructureNode &node) {
	D_ASSERT(!yyjson_is_arr(val) && !yyjson_is_obj(val));
	const auto val_type = JSONCommon::ValTypeToLogicalTypeId(val);
	auto &desc = node.GetOrCreateDescription(val_type);
	if (val_type == LogicalTypeId::UBIGINT &&
	    unsafe_yyjson_get_uint(val) > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
		desc.has_large_ubigint = true;
	}
}

void JSONStructure::ExtractStructure(yyjson_val *val, JSONStructureNode &node, const JSONInferenceOptions &options,
                                     const idx_t depth) {
	if (depth >= JSONCommon::MAX_RECURSION_DEPTH) {
		throw InvalidInputException("JSON exceeds maximum recursion depth of %d", JSONCommon::MAX_RECURSION_DEPTH);
	}
	node.count++;
	const auto tag = yyjson_get_tag(val);
	if (tag == (YYJSON_TYPE_NULL | YYJSON_SUBTYPE_NONE)) {
		node.null_count++;
	}

	switch (tag) {
	case YYJSON_TYPE_ARR | YYJSON_SUBTYPE_NONE:
		return ExtractStructureArray(val, node, options, depth);
	case YYJSON_TYPE_OBJ | YYJSON_SUBTYPE_NONE:
		// A GeoJSON geometry is a leaf: it becomes GEOMETRY rather than a struct of type/coordinates. Detecting it
		// here (rather than on the finished tree) means geometries of different kinds still merge into one type.
		if (options.geometry_detector && options.geometry_detector(val)) {
			node.GetOrCreateDescription(LogicalTypeId::GEOMETRY);
			return;
		}
		return ExtractStructureObject(val, node, options, depth);
	default:
		return ExtractStructureVal(val, node);
	}
}

//===--------------------------------------------------------------------===//
// Merging structures
//===--------------------------------------------------------------------===//
static void MergeNodeArray(JSONStructureNode &merged, const JSONStructureDescription &child_desc) {
	D_ASSERT(child_desc.type == LogicalTypeId::LIST);
	auto &merged_desc = merged.GetOrCreateDescription(LogicalTypeId::LIST);
	auto &merged_child = merged_desc.GetOrCreateChild();
	for (auto &list_child : child_desc.children) {
		JSONStructure::MergeNodes(merged_child, list_child);
	}
}

static void MergeNodeObject(JSONStructureNode &merged, const JSONStructureDescription &child_desc) {
	D_ASSERT(child_desc.type == LogicalTypeId::STRUCT);
	auto &merged_desc = merged.GetOrCreateDescription(LogicalTypeId::STRUCT);
	for (auto &struct_child : child_desc.children) {
		const auto &struct_child_key = *struct_child.key;
		auto &merged_child = merged_desc.GetOrCreateChild(struct_child_key.c_str(), struct_child_key.length());
		JSONStructure::MergeNodes(merged_child, struct_child);
	}
}

static void MergeNodeVal(JSONStructureNode &merged, const JSONStructureDescription &child_desc,
                         const bool node_initialized) {
	D_ASSERT(child_desc.type != LogicalTypeId::LIST && child_desc.type != LogicalTypeId::STRUCT);
	auto &merged_desc = merged.GetOrCreateDescription(child_desc.type);
	if (child_desc.has_large_ubigint) {
		merged_desc.has_large_ubigint = true;
	}
	if (merged_desc.type != LogicalTypeId::VARCHAR || !node_initialized || merged.descriptions.size() != 1) {
		return;
	}
	// Sticky offset state across parallel auto-detect tasks
	merged_desc.timestamp_offset_state =
	    MergeTimestampOffsetState(merged_desc.timestamp_offset_state, child_desc.timestamp_offset_state);
	if (merged_desc.timestamp_offset_state == TimestampOffsetState::MIXED) {
		merged_desc.candidate_types.clear(); // mixed offset/plain -> VARCHAR
		merged.initialized = true;
		return;
	}
	if (!merged.initialized) {
		merged_desc.candidate_types = child_desc.candidate_types;
	} else if (merged_desc.candidate_types.empty() != child_desc.candidate_types.empty() // both empty or neither empty
	           || (!merged_desc.candidate_types.empty() &&
	               merged_desc.candidate_types.back() != child_desc.candidate_types.back())) { // non-empty: check type
		merged_desc.candidate_types.clear(); // Not the same, default to VARCHAR
	}

	merged.initialized = true;
}

void JSONStructure::MergeNodes(JSONStructureNode &merged, const JSONStructureNode &node) {
	merged.count += node.count;
	merged.null_count += node.null_count;
	for (const auto &child_desc : node.descriptions) {
		switch (child_desc.type) {
		case LogicalTypeId::LIST:
			MergeNodeArray(merged, child_desc);
			break;
		case LogicalTypeId::STRUCT:
			MergeNodeObject(merged, child_desc);
			break;
		default:
			MergeNodeVal(merged, child_desc, node.initialized);
			break;
		}
	}
}

//===--------------------------------------------------------------------===//
// Structure to type
//===--------------------------------------------------------------------===//
//! Struct members by name; names compare case-insensitively, like the engine's identifiers
using ChildTypeMap = unordered_map<string, LogicalType>;

static ChildTypeMap GetStructChildren(const LogicalType &type) {
	ChildTypeMap result;
	const auto count = type.GetStructChildCount();
	for (idx_t i = 0; i < count; i++) {
		result.emplace(LowerCaseKey(type.GetStructChildName(i)), type.GetStructChildType(i));
	}
	return result;
}

static double CalculateTypeSimilarity(const LogicalType &merged, const LogicalType &type, idx_t max_depth, idx_t depth);

static double CalculateMapAndStructSimilarity(const LogicalType &map_type, const LogicalType &struct_type,
                                              const bool swapped, const idx_t max_depth, const idx_t depth) {
	const auto map_value_type = map_type.GetMapValueType();
	const auto child_count = struct_type.GetStructChildCount();
	double total_similarity = 0;
	for (idx_t i = 0; i < child_count; i++) {
		const auto struct_child_type = struct_type.GetStructChildType(i);
		const auto similarity = swapped
		                            ? CalculateTypeSimilarity(struct_child_type, map_value_type, max_depth, depth + 1)
		                            : CalculateTypeSimilarity(map_value_type, struct_child_type, max_depth, depth + 1);
		if (similarity < 0) {
			return similarity;
		}
		total_similarity += similarity;
	}
	return total_similarity / static_cast<double>(child_count);
}

static double CalculateTypeSimilarity(const LogicalType &merged, const LogicalType &type, const idx_t max_depth,
                                      const idx_t depth) {
	if (depth >= max_depth || merged.GetTypeId() == LogicalTypeId::SQLNULL ||
	    type.GetTypeId() == LogicalTypeId::SQLNULL) {
		return 1;
	}
	if (IsJSONType(merged)) {
		// Incompatible types
		return -1;
	}
	if (IsJSONType(type) || merged == type) {
		return 1;
	}

	switch (merged.GetTypeId()) {
	case LogicalTypeId::STRUCT: {
		if (type.GetTypeId() == LogicalTypeId::MAP) {
			// This can happen for empty structs/maps ("{}"), or in rare cases where an inconsistent struct becomes
			// consistent when merged, but does not have enough children to be considered a map.
			return CalculateMapAndStructSimilarity(type, merged, true, max_depth, depth);
		} else if (type.GetTypeId() != LogicalTypeId::STRUCT) {
			return -1;
		}

		// Only structs can be merged into a struct
		D_ASSERT(type.GetTypeId() == LogicalTypeId::STRUCT);
		const auto merged_child_types_map = GetStructChildren(merged);

		double total_similarity = 0;
		const auto type_child_count = type.GetStructChildCount();
		for (idx_t i = 0; i < type_child_count; i++) {
			const auto it = merged_child_types_map.find(LowerCaseKey(type.GetStructChildName(i)));
			if (it == merged_child_types_map.end()) {
				return -1;
			}
			const auto similarity =
			    CalculateTypeSimilarity(it->second, type.GetStructChildType(i), max_depth, depth + 1);
			if (similarity < 0) {
				return similarity;
			}
			total_similarity += similarity;
		}
		return total_similarity / static_cast<double>(merged.GetStructChildCount());
	}
	case LogicalTypeId::MAP: {
		if (type.GetTypeId() == LogicalTypeId::MAP) {
			return CalculateTypeSimilarity(merged.GetMapValueType(), type.GetMapValueType(), max_depth, depth + 1);
		}

		// Only maps and structs can be merged into a map
		if (type.GetTypeId() != LogicalTypeId::STRUCT) {
			return -1;
		}
		return CalculateMapAndStructSimilarity(merged, type, false, max_depth, depth);
	}
	case LogicalTypeId::LIST: {
		// Only lists can be merged into a list
		if (type.GetTypeId() != LogicalTypeId::LIST) {
			return -1;
		}
		return CalculateTypeSimilarity(merged.GetListChildType(), type.GetListChildType(), max_depth, depth + 1);
	}
	default:
		// This is only reachable if type has been inferred using candidate_types, but candidate_types were not
		// consistent among all map values
		return 1;
	}
}

static bool IsStructureInconsistent(const JSONStructureDescription &desc, const idx_t sample_count,
                                    const idx_t null_count, const double field_appearance_threshold) {
	D_ASSERT(sample_count > null_count);
	double total_child_counts = 0;
	for (const auto &child : desc.children) {
		total_child_counts += static_cast<double>(child.count) / static_cast<double>(sample_count - null_count);
	}
	const auto avg_occurrence = total_child_counts / static_cast<double>(desc.children.size());
	return avg_occurrence < field_appearance_threshold;
}

static LogicalType MapOf(Factory &factory, const LogicalType &key, const LogicalType &value) {
	vector<cxx::TypeParam> params;
	params.emplace_back(factory.CreateValue(key));
	params.emplace_back(factory.CreateValue(value));
	return factory.CreateType(LogicalTypeId::MAP, params);
}

static LogicalType GetMergedType(Factory &factory, const JSONStructureNode &node, const JSONInferenceOptions &options,
                                 const idx_t depth) {
	D_ASSERT(node.descriptions.size() == 1);
	auto &desc = node.descriptions[0];
	auto merged = JSONStructureNode::EmptyLike(node);
	for (const auto &child : desc.children) {
		JSONStructure::MergeNodes(merged, child);
	}
	return JSONStructure::StructureToType(factory, merged, options, depth + 1);
}

static LogicalType StructureToTypeArray(Factory &factory, const JSONStructureNode &node,
                                        const JSONInferenceOptions &options, const idx_t depth) {
	D_ASSERT(node.descriptions.size() == 1 && node.descriptions[0].type == LogicalTypeId::LIST);
	const auto &desc = node.descriptions[0];
	D_ASSERT(desc.children.size() == 1);

	return ListOf(factory, JSONStructure::StructureToType(factory, desc.children[0], options, depth + 1));
}

static LogicalType StructureToTypeObject(Factory &factory, const JSONStructureNode &node,
                                         const JSONInferenceOptions &options, const idx_t depth) {
	D_ASSERT(node.descriptions.size() == 1 && node.descriptions[0].type == LogicalTypeId::STRUCT);
	auto &desc = node.descriptions[0];
	const auto map_inference_threshold = options.map_inference_threshold;
	const auto map_inference_disabled = map_inference_threshold == static_cast<idx_t>(-1);
	const auto varchar = factory.CreateType(LogicalTypeId::VARCHAR);

	if (desc.children.empty()) {
		if (!map_inference_disabled) {
			// Empty struct - use MAP(VARCHAR, JSON) as a generic container;
			// JSON() is safe for all output formats
			return MapOf(factory, varchar, JSONType(factory));
		} else {
			return JSONType(factory);
		}
	}

	// An empty key can't be a STRUCT member name, so such objects can only be a MAP or JSON
	bool has_empty_key = false;
	for (auto &child : desc.children) {
		D_ASSERT(child.key);
		has_empty_key = has_empty_key || child.key->empty();
	}
	if (has_empty_key && map_inference_disabled) {
		return JSONType(factory);
	}

	// If it's an inconsistent object we also just do MAP with the best-possible, recursively-merged value type
	if (has_empty_key || (!map_inference_disabled && IsStructureInconsistent(desc, node.count, node.null_count,
	                                                                         options.field_appearance_threshold))) {
		return MapOf(factory, varchar, GetMergedType(factory, node, options, depth + 1));
	}

	// We have a consistent object
	vector<std::pair<string, LogicalType>> child_types;
	child_types.reserve(desc.children.size());
	for (auto &child : desc.children) {
		D_ASSERT(child.key);
		child_types.emplace_back(*child.key, JSONStructure::StructureToType(factory, child, options, depth + 1));
	}

	// If we have many children and all children have similar-enough types we infer map
	if (desc.children.size() >= map_inference_threshold) {
		const auto map_value_type = GetMergedType(factory, node, options, depth + 1);

		double total_similarity = 0;
		for (const auto &child_type : child_types) {
			const auto similarity =
			    CalculateTypeSimilarity(map_value_type, child_type.second, options.max_depth, depth + 1);
			if (similarity < 0) {
				total_similarity = similarity;
				break;
			}
			total_similarity += similarity;
		}
		const auto avg_similarity = total_similarity / static_cast<double>(child_types.size());
		if (avg_similarity >= 0.8) {
			return MapOf(factory, varchar, map_value_type);
		}
	}

	// Member names are unique ignoring case. The columns of a record are renamed (the reader still finds them by
	// their key), the members of anything else are dropped or an error
	vector<cxx::TypeParam> params;
	std::unordered_map<string, idx_t> member_names;
	const bool rename = depth == 0 && options.rename_duplicate_root_members;
	for (const auto &child_type : child_types) {
		auto name = child_type.first;
		auto lower = StringUtil::Lower(name);
		if (member_names.find(lower) != member_names.end()) {
			if (rename) {
				while (member_names.find(lower) != member_names.end()) {
					member_names[lower] += 1;
					name = name + "_" + std::to_string(member_names[lower]);
					lower = StringUtil::Lower(name);
				}
			} else if (options.ignore_errors) {
				continue;
			} else {
				throw NotImplementedException(
				    "Duplicate name %s in struct auto-detected in JSON, try ignore_errors=true", child_type.first);
			}
		}
		member_names[lower] = 0;
		params.emplace_back(name, factory.CreateValue(child_type.second));
	}
	return factory.CreateType(LogicalTypeId::STRUCT, params);
}

static LogicalType StructureToTypeString(Factory &factory, const JSONStructureNode &node) {
	D_ASSERT(node.descriptions.size() == 1 && node.descriptions[0].type == LogicalTypeId::VARCHAR);
	auto &desc = node.descriptions[0];
	if (desc.candidate_types.empty()) {
		return factory.CreateType(LogicalTypeId::VARCHAR);
	}
	return factory.CreateType(desc.candidate_types.back());
}

LogicalType JSONStructure::StructureToType(Factory &factory, const JSONStructureNode &node,
                                           const JSONInferenceOptions &options, const idx_t depth) {
	if (depth >= options.max_depth) {
		return JSONType(factory);
	}
	if (node.descriptions.empty()) {
		return JSONType(factory); // no data observed, fall back to generic JSON type
	}
	if (node.descriptions.size() != 1) { // Inconsistent types, so we resort to JSON
		return JSONType(factory);
	}
	auto &desc = node.descriptions[0];
	D_ASSERT(desc.type != LogicalTypeId::INVALID);
	switch (desc.type) {
	case LogicalTypeId::LIST:
		return StructureToTypeArray(factory, node, options, depth);
	case LogicalTypeId::STRUCT:
		return StructureToTypeObject(factory, node, options, depth);
	case LogicalTypeId::VARCHAR:
		return StructureToTypeString(factory, node);
	case LogicalTypeId::UBIGINT:
		if (desc.has_large_ubigint) {
			return factory.CreateType(LogicalTypeId::HUGEINT);
		}
		return factory.CreateType(LogicalTypeId::BIGINT);
	case LogicalTypeId::SQLNULL:
		// The NULL type can only be parsed (CreateType refuses it)
		return factory.ParseType("\"NULL\"");
	case LogicalTypeId::GEOMETRY:
		// GEOMETRY takes type parameters (the CRS), so it cannot be created from its id
		return factory.ParseType("GEOMETRY");
	default:
		return factory.CreateType(desc.type);
	}
}

} // namespace json
} // namespace duckdb
