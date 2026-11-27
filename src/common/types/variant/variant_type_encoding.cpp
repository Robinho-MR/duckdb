#include "duckdb/common/types/variant/variant_type_encoding.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/common/types/geometry_crs.hpp"
#include "duckdb/common/types/variant_iterator.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"

namespace duckdb {

VariantTypeModifier::VariantTypeModifier() {
}
VariantTypeModifier::VariantTypeModifier(VariantTypeModifier &&other) noexcept = default;
VariantTypeModifier &VariantTypeModifier::operator=(VariantTypeModifier &&other) noexcept = default;
VariantTypeModifier::~VariantTypeModifier() {
}

VariantTypeDescription::VariantTypeDescription() {
}
VariantTypeDescription::VariantTypeDescription(VariantTypeDescription &&other) noexcept = default;
VariantTypeDescription &VariantTypeDescription::operator=(VariantTypeDescription &&other) noexcept = default;
VariantTypeDescription::~VariantTypeDescription() {
}

//===--------------------------------------------------------------------===//
// LogicalType -> VariantTypeDescription
//===--------------------------------------------------------------------===//
namespace {

//! Every level of a type takes three levels of nesting in a VARIANT, which allows 128
constexpr idx_t MAX_TYPE_DEPTH = 40;

VariantTypeModifier ValueModifier(Value value) {
	VariantTypeModifier result;
	result.value = std::move(value);
	return result;
}

VariantTypeModifier ValueModifier(const string &label, Value value) {
	auto result = ValueModifier(std::move(value));
	result.has_label = true;
	result.label = label;
	return result;
}

VariantTypeDescription Decompose(const LogicalType &type, idx_t depth);

VariantTypeModifier TypeModifier(const LogicalType &type, idx_t depth) {
	VariantTypeModifier result;
	result.type = make_uniq<VariantTypeDescription>(Decompose(type, depth + 1));
	return result;
}

VariantTypeModifier TypeModifier(const string &label, const LogicalType &type, idx_t depth) {
	auto result = TypeModifier(type, depth);
	result.has_label = true;
	result.label = label;
	return result;
}

//! The kinds of values a modifier of an extension type can have, they are stored as the VARIANT value of that type
bool IsSupportedModifierValue(const Value &value) {
	switch (value.type().id()) {
	case LogicalTypeId::SQLNULL:
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::VARCHAR:
		return !value.type().HasAlias();
	default:
		return false;
	}
}

[[noreturn]] void ThrowUnsupportedType(const LogicalType &type) {
	throw NotImplementedException("Type %s can't be stored in a VARIANT", type.ToString());
}

VariantTypeDescription DecomposeExtensionType(const LogicalType &type, idx_t depth) {
	auto extension_info = type.GetExtensionInfo();
	if (extension_info && !extension_info->properties.empty()) {
		throw NotImplementedException("Type %s can't be stored in a VARIANT: types with properties are not supported",
		                              type.ToString());
	}
	VariantTypeDescription result;
	result.name = type.GetAlias();
	if (extension_info) {
		for (auto &modifier : extension_info->modifiers) {
			if (!IsSupportedModifierValue(modifier.value)) {
				throw NotImplementedException(
				    "Type %s can't be stored in a VARIANT: modifiers of type %s are not supported", type.ToString(),
				    modifier.value.type().ToString());
			}
			if (modifier.label.empty()) {
				result.modifiers.push_back(ValueModifier(modifier.value));
			} else {
				result.modifiers.push_back(ValueModifier(modifier.label, modifier.value));
			}
		}
	}
	auto physical_type = type.WithAlias(string()).WithExtensionInfo(nullptr);
	result.physical = make_uniq<VariantTypeDescription>(Decompose(physical_type, depth + 1));
	return result;
}

VariantTypeDescription Decompose(const LogicalType &type, idx_t depth) {
	if (depth > MAX_TYPE_DEPTH) {
		throw InvalidInputException("Type is nested too deeply to be stored in a VARIANT (maximum depth is %d)",
		                            MAX_TYPE_DEPTH);
	}
	if (type.IsJSONType()) {
		VariantTypeDescription result;
		result.name = LogicalType::JSON_TYPE_NAME;
		return result;
	}
	if (type.IsAggregateState()) {
		ThrowUnsupportedType(type);
	}
	if (type.HasAlias()) {
		return DecomposeExtensionType(type, depth);
	}

	const auto id = type.id();
	VariantTypeDescription result;
	result.name = EnumUtil::ToString(id);
	auto &modifiers = result.modifiers;
	switch (id) {
	case LogicalTypeId::SQLNULL:
	case LogicalTypeId::TYPE:
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::UUID:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::BIT:
	case LogicalTypeId::BIGNUM:
	case LogicalTypeId::VARIANT:
	case LogicalTypeId::CHAR:
		break;
	case LogicalTypeId::VARCHAR: {
		auto collation = StringType::GetCollation(type);
		if (!collation.empty()) {
			modifiers.push_back(ValueModifier("collation", Value(collation)));
		}
		break;
	}
	case LogicalTypeId::DECIMAL:
		modifiers.push_back(ValueModifier(Value::BIGINT(DecimalType::GetWidth(type))));
		modifiers.push_back(ValueModifier(Value::BIGINT(DecimalType::GetScale(type))));
		break;
	case LogicalTypeId::ENUM: {
		auto &values = EnumType::GetValuesInsertOrder(type);
		auto size = EnumType::GetSize(type);
		for (idx_t i = 0; i < size; i++) {
			modifiers.push_back(ValueModifier(values.GetValue(i)));
		}
		break;
	}
	case LogicalTypeId::LIST:
		modifiers.push_back(TypeModifier(ListType::GetChildType(type), depth));
		break;
	case LogicalTypeId::ARRAY:
		modifiers.push_back(TypeModifier(ArrayType::GetChildType(type), depth));
		modifiers.push_back(ValueModifier(ArrayType::IsAnySize(type)
		                                      ? Value(LogicalType::SQLNULL)
		                                      : Value::BIGINT(NumericCast<int64_t>(ArrayType::GetSize(type)))));
		break;
	case LogicalTypeId::MAP:
		modifiers.push_back(TypeModifier(MapType::KeyType(type), depth));
		modifiers.push_back(TypeModifier(MapType::ValueType(type), depth));
		break;
	case LogicalTypeId::STRUCT:
		for (auto &child : StructType::GetChildTypes(type)) {
			modifiers.push_back(TypeModifier(child.first.GetIdentifierName(), child.second, depth));
		}
		break;
	case LogicalTypeId::TUPLE:
		for (auto &child : StructType::GetChildTypes(type)) {
			modifiers.push_back(TypeModifier(child.second, depth));
		}
		break;
	case LogicalTypeId::UNION:
		for (auto &member : UnionType::CopyMemberTypes(type)) {
			modifiers.push_back(TypeModifier(member.first.GetIdentifierName(), member.second, depth));
		}
		break;
	case LogicalTypeId::GEOMETRY:
	case LogicalTypeId::GEOGRAPHY:
		if (GeoType::HasCRS(type)) {
			modifiers.push_back(ValueModifier("crs", Value(GeoType::GetCRS(type).GetDefinition())));
		}
		break;
	default:
		ThrowUnsupportedType(type);
	}
	return result;
}

//===--------------------------------------------------------------------===//
// VariantTypeDescription -> LogicalType
//===--------------------------------------------------------------------===//
[[noreturn]] void ThrowInvalidType(const string &message) {
	throw InvalidInputException("Invalid TYPE value in VARIANT: %s", message);
}

LogicalType Compose(const VariantTypeDescription &description, idx_t depth);

LogicalType ModifierType(const VariantTypeDescription &description, const VariantTypeModifier &modifier, idx_t depth) {
	if (!modifier.type) {
		ThrowInvalidType(StringUtil::Format("type %s expects a type as modifier", description.name));
	}
	return Compose(*modifier.type, depth + 1);
}

int64_t ModifierInteger(const VariantTypeDescription &description, const VariantTypeModifier &modifier) {
	if (modifier.type || modifier.value.IsNull() || !modifier.value.type().IsIntegral()) {
		ThrowInvalidType(StringUtil::Format("type %s expects an integer as modifier", description.name));
	}
	return modifier.value.GetValue<int64_t>();
}

const string &ModifierString(const VariantTypeDescription &description, const VariantTypeModifier &modifier) {
	if (modifier.type || modifier.value.IsNull() || modifier.value.type().id() != LogicalTypeId::VARCHAR) {
		ThrowInvalidType(StringUtil::Format("type %s expects a string as modifier", description.name));
	}
	return StringValue::Get(modifier.value);
}

void ExpectModifiers(const VariantTypeDescription &description, idx_t count) {
	if (description.modifiers.size() != count) {
		ThrowInvalidType(StringUtil::Format("type %s expects %llu modifiers, found %llu", description.name, count,
		                                    description.modifiers.size()));
	}
}

child_list_t<LogicalType> ComposeMembers(const VariantTypeDescription &description, bool named, idx_t depth) {
	child_list_t<LogicalType> members;
	for (auto &modifier : description.modifiers) {
		string name;
		if (named) {
			if (!modifier.has_label) {
				ThrowInvalidType(StringUtil::Format("type %s expects a label for every member", description.name));
			}
			name = modifier.label;
		}
		members.emplace_back(std::move(name), ModifierType(description, modifier, depth));
	}
	return members;
}

LogicalType ComposeExtensionType(const VariantTypeDescription &description, idx_t depth) {
	auto result = Compose(*description.physical, depth + 1).WithAlias(description.name);
	if (description.modifiers.empty()) {
		return result;
	}
	auto info = make_uniq<ExtensionTypeInfo>();
	for (auto &modifier : description.modifiers) {
		if (modifier.type) {
			ThrowInvalidType(StringUtil::Format("type %s has a type as modifier", description.name));
		}
		LogicalTypeModifier type_modifier(modifier.value);
		type_modifier.label = modifier.has_label ? modifier.label : string();
		info->modifiers.push_back(std::move(type_modifier));
	}
	return result.WithExtensionInfo(std::move(info));
}

LogicalType Compose(const VariantTypeDescription &description, idx_t depth) {
	if (depth > MAX_TYPE_DEPTH) {
		throw InvalidInputException("Type is nested too deeply to be stored in a VARIANT (maximum depth is %d)",
		                            MAX_TYPE_DEPTH);
	}
	if (description.physical) {
		return ComposeExtensionType(description, depth);
	}
	if (description.name == LogicalType::JSON_TYPE_NAME) {
		return LogicalType::JSON();
	}
	LogicalTypeId id;
	try {
		id = EnumUtil::FromString<LogicalTypeId>(description.name.c_str());
	} catch (std::exception &) {
		ThrowInvalidType(StringUtil::Format("unknown type %s", description.name));
	}
	switch (id) {
	case LogicalTypeId::SQLNULL:
	case LogicalTypeId::TYPE:
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::UUID:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::BIT:
	case LogicalTypeId::BIGNUM:
	case LogicalTypeId::VARIANT:
	case LogicalTypeId::CHAR:
		ExpectModifiers(description, 0);
		return id == LogicalTypeId::VARIANT ? LogicalType::VARIANT()
		       : id == LogicalTypeId::TYPE  ? LogicalType::TYPE()
		                                    : LogicalType(id);
	case LogicalTypeId::VARCHAR: {
		if (description.modifiers.empty()) {
			return LogicalType::VARCHAR;
		}
		ExpectModifiers(description, 1);
		return LogicalType::VARCHAR_COLLATION(ModifierString(description, description.modifiers[0]));
	}
	case LogicalTypeId::DECIMAL: {
		ExpectModifiers(description, 2);
		auto width = ModifierInteger(description, description.modifiers[0]);
		auto scale = ModifierInteger(description, description.modifiers[1]);
		if (width < 1 || width > NumericCast<int64_t>(DecimalType::MaxWidth()) || scale < 0 || scale > width) {
			ThrowInvalidType(StringUtil::Format("invalid DECIMAL(%lld, %lld)", width, scale));
		}
		return LogicalType::DECIMAL(NumericCast<uint32_t>(width), NumericCast<uint32_t>(scale));
	}
	case LogicalTypeId::ENUM: {
		const auto size = description.modifiers.size();
		Vector values(LogicalType::VARCHAR, size);
		auto writer = FlatVector::Writer<string_t>(values, size);
		for (auto &modifier : description.modifiers) {
			writer.WriteValue(StringVector::AddString(values, ModifierString(description, modifier)));
		}
		return LogicalType::ENUM(values, size);
	}
	case LogicalTypeId::LIST:
		ExpectModifiers(description, 1);
		return LogicalType::LIST(ModifierType(description, description.modifiers[0], depth));
	case LogicalTypeId::ARRAY: {
		ExpectModifiers(description, 2);
		auto child = ModifierType(description, description.modifiers[0], depth);
		auto &size_modifier = description.modifiers[1];
		if (!size_modifier.type && size_modifier.value.IsNull()) {
			return LogicalType::ARRAY(child, optional_idx());
		}
		auto size = ModifierInteger(description, size_modifier);
		if (size < 1 || size > NumericCast<int64_t>(ArrayType::MAX_ARRAY_SIZE)) {
			ThrowInvalidType(StringUtil::Format("invalid ARRAY size %lld", size));
		}
		return LogicalType::ARRAY(child, optional_idx(NumericCast<idx_t>(size)));
	}
	case LogicalTypeId::MAP:
		ExpectModifiers(description, 2);
		return LogicalType::MAP(ModifierType(description, description.modifiers[0], depth),
		                        ModifierType(description, description.modifiers[1], depth));
	case LogicalTypeId::STRUCT:
		return LogicalType::STRUCT(ComposeMembers(description, true, depth));
	case LogicalTypeId::TUPLE:
		return LogicalType::TUPLE(ComposeMembers(description, false, depth));
	case LogicalTypeId::UNION:
		return LogicalType::UNION(ComposeMembers(description, true, depth));
	case LogicalTypeId::GEOMETRY:
	case LogicalTypeId::GEOGRAPHY: {
		if (description.modifiers.empty()) {
			return id == LogicalTypeId::GEOMETRY ? LogicalType::GEOMETRY() : LogicalType::GEOGRAPHY();
		}
		ExpectModifiers(description, 1);
		auto &crs = ModifierString(description, description.modifiers[0]);
		return id == LogicalTypeId::GEOMETRY ? LogicalType::GEOMETRY(crs) : LogicalType::GEOGRAPHY(crs);
	}
	default:
		ThrowInvalidType(StringUtil::Format("type %s can't be stored", description.name));
	}
}

//===--------------------------------------------------------------------===//
// VARIANT -> VariantTypeDescription
//===--------------------------------------------------------------------===//
//! A TYPE value of a VariantNode
class NodeTypeReader {
public:
	explicit NodeTypeReader(const VariantNode &node) : node(node) {
	}

	LogicalTypeId GetTypeId() const {
		return node.GetTypeId();
	}
	idx_t ChildCount() const {
		return node.GetArrayChildren().size();
	}
	NodeTypeReader Child(idx_t index) const {
		return NodeTypeReader(node.GetArrayChildren()[index]);
	}
	string GetString() const {
		return node.GetString().GetString();
	}
	Value GetScalar() const {
		switch (node.GetTypeId()) {
		case LogicalTypeId::SQLNULL:
			return Value(LogicalType::SQLNULL);
		case LogicalTypeId::BOOLEAN:
			return Value::BOOLEAN(node.GetBool());
		case LogicalTypeId::TINYINT:
			return Value::TINYINT(node.GetData<int8_t>());
		case LogicalTypeId::SMALLINT:
			return Value::SMALLINT(node.GetData<int16_t>());
		case LogicalTypeId::INTEGER:
			return Value::INTEGER(node.GetData<int32_t>());
		case LogicalTypeId::BIGINT:
			return Value::BIGINT(node.GetData<int64_t>());
		case LogicalTypeId::UTINYINT:
			return Value::UTINYINT(node.GetData<uint8_t>());
		case LogicalTypeId::USMALLINT:
			return Value::USMALLINT(node.GetData<uint16_t>());
		case LogicalTypeId::UINTEGER:
			return Value::UINTEGER(node.GetData<uint32_t>());
		case LogicalTypeId::UBIGINT:
			return Value::UBIGINT(node.GetData<uint64_t>());
		case LogicalTypeId::FLOAT:
			return Value::FLOAT(node.GetData<float>());
		case LogicalTypeId::DOUBLE:
			return Value::DOUBLE(node.GetData<double>());
		case LogicalTypeId::VARCHAR:
			return Value(node.GetString().GetString());
		default:
			ThrowInvalidType("a modifier has an unsupported value");
		}
	}

private:
	VariantNode node;
};

//! A TYPE value in unified variant data
class UnifiedTypeReader {
public:
	UnifiedTypeReader(const UnifiedVariantVectorData &variant, idx_t row, uint32_t values_index)
	    : variant(variant), row(row), values_index(values_index) {
	}

	LogicalTypeId GetTypeId() const {
		return VariantTypeToLogicalTypeId(variant.GetTypeId(row, values_index));
	}
	idx_t ChildCount() const {
		return VariantUtils::DecodeNestedData(variant, row, values_index).child_count;
	}
	UnifiedTypeReader Child(idx_t index) const {
		auto nested_data = VariantUtils::DecodeNestedData(variant, row, values_index);
		return UnifiedTypeReader(variant, row, variant.GetValuesIndex(row, nested_data.children_idx + index));
	}
	string GetString() const {
		return VariantUtils::DecodeStringData(variant, row, values_index).GetString();
	}
	Value GetScalar() const {
		return VariantUtils::ConvertVariantToValue(variant, row, values_index);
	}

private:
	const UnifiedVariantVectorData &variant;
	idx_t row;
	uint32_t values_index;
};

template <class READER>
void ExpectType(const READER &reader, LogicalTypeId expected, const char *what) {
	if (reader.GetTypeId() != expected) {
		ThrowInvalidType(StringUtil::Format("expected %s", what));
	}
}

template <class READER>
VariantTypeDescription ReadDescription(const READER &reader, idx_t depth) {
	if (depth > MAX_TYPE_DEPTH) {
		throw InvalidInputException("Type is nested too deeply to be stored in a VARIANT (maximum depth is %d)",
		                            MAX_TYPE_DEPTH);
	}
	ExpectType(reader, LogicalTypeId::TYPE, "a TYPE value");
	const auto child_count = reader.ChildCount();
	if (child_count != 2 && child_count != 3) {
		ThrowInvalidType(StringUtil::Format("a TYPE value has %llu children", child_count));
	}
	VariantTypeDescription result;
	auto name = reader.Child(VariantTypeEncoding::NAME_CHILD);
	ExpectType(name, LogicalTypeId::VARCHAR, "the name of a type to be a VARCHAR");
	result.name = name.GetString();

	auto modifiers = reader.Child(VariantTypeEncoding::MODIFIERS_CHILD);
	ExpectType(modifiers, LogicalTypeId::LIST, "the modifiers of a type to be an ARRAY");
	const auto modifier_count = modifiers.ChildCount();
	for (idx_t i = 0; i < modifier_count; i++) {
		auto entry = modifiers.Child(i);
		ExpectType(entry, LogicalTypeId::LIST, "a modifier of a type to be an ARRAY");
		if (entry.ChildCount() != 2) {
			ThrowInvalidType("a modifier of a type is not a pair of a label and a value");
		}
		VariantTypeModifier modifier;
		auto label = entry.Child(0);
		if (label.GetTypeId() == LogicalTypeId::VARCHAR) {
			modifier.has_label = true;
			modifier.label = label.GetString();
		} else if (label.GetTypeId() != LogicalTypeId::SQLNULL) {
			ThrowInvalidType("the label of a modifier is not a VARCHAR or NULL");
		}
		auto value = entry.Child(1);
		if (value.GetTypeId() == LogicalTypeId::TYPE) {
			modifier.type = make_uniq<VariantTypeDescription>(ReadDescription(value, depth + 1));
		} else {
			modifier.value = value.GetScalar();
		}
		result.modifiers.push_back(std::move(modifier));
	}
	if (child_count == 3) {
		result.physical = make_uniq<VariantTypeDescription>(
		    ReadDescription(reader.Child(VariantTypeEncoding::PHYSICAL_CHILD), depth + 1));
	}
	return result;
}

} // namespace

VariantTypeDescription VariantTypeEncoding::Decompose(const LogicalType &type) {
	return duckdb::Decompose(type, 0);
}

LogicalType VariantTypeEncoding::Compose(const VariantTypeDescription &description) {
	return duckdb::Compose(description, 0);
}

LogicalType VariantTypeEncoding::Read(const UnifiedVariantVectorData &variant, idx_t row, uint32_t values_index) {
	return Compose(ReadDescription(UnifiedTypeReader(variant, row, values_index), 0));
}

LogicalType VariantTypeEncoding::Read(const VariantNode &node) {
	return Compose(ReadDescription(NodeTypeReader(node), 0));
}

string VariantTypeEncoding::Serialize(const LogicalType &type) {
	return StringValue::Get(Value::TYPE(type));
}

LogicalType VariantTypeEncoding::Deserialize(const string_t &serialized_type) {
	return TypeValue::GetType(Value::TYPE(serialized_type));
}

} // namespace duckdb
