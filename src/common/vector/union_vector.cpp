#include "duckdb/common/vector/union_vector.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/variant.hpp"
#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"

namespace duckdb {

static bool MemberAcceptsValue(const LogicalType &member_type, VariantLogicalType value_type) {
	if (UnionType::IsUnion(member_type)) {
		for (auto &member : UnionType::GetMemberTypes(member_type)) {
			if (MemberAcceptsValue(member.second, value_type)) {
				return true;
			}
		}
		return false;
	}
	if (member_type.id() == LogicalTypeId::VARIANT || member_type.IsJSONType()) {
		// a VARIANT or JSON member can hold a value of any type
		return true;
	}
	switch (member_type.id()) {
	case LogicalTypeId::BOOLEAN:
		return value_type == VariantLogicalType::BOOL_TRUE || value_type == VariantLogicalType::BOOL_FALSE;
	case LogicalTypeId::ENUM:
		return value_type == VariantLogicalType::VARCHAR;
	case LogicalTypeId::LIST:
	case LogicalTypeId::MAP:
	case LogicalTypeId::ARRAY:
	case LogicalTypeId::TUPLE:
		return value_type == VariantLogicalType::ARRAY;
	default: {
		VariantLogicalType expected;
		if (!TryVariantTypeFromLogicalTypeId(member_type.id(), expected)) {
			return false;
		}
		return expected == value_type;
	}
	}
}

//! Whether a (nested) VARIANT value can be a value of the member type: the value has the type of the member, and the
//! elements of a LIST or the fields of a STRUCT recursively match the child types
static bool MemberMatchesValue(const LogicalType &member_type, const UnifiedVariantVectorData &variant, idx_t row,
                               uint32_t value_index) {
	auto value_type = variant.GetTypeId(row, value_index);
	if (UnionType::IsUnion(member_type)) {
		for (auto &member : UnionType::GetMemberTypes(member_type)) {
			if (MemberMatchesValue(member.second, variant, row, value_index)) {
				return true;
			}
		}
		return false;
	}
	if (value_type == VariantLogicalType::VARIANT_NULL) {
		// a NULL child can be a value of any type
		return true;
	}
	switch (member_type.id()) {
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY: {
		if (value_type != VariantLogicalType::ARRAY) {
			return false;
		}
		auto &child_type = member_type.id() == LogicalTypeId::LIST ? ListType::GetChildType(member_type)
		                                                           : ArrayType::GetChildType(member_type);
		auto nested = VariantUtils::DecodeNestedData(variant, row, value_index);
		for (idx_t i = 0; i < nested.child_count; i++) {
			auto child_index = variant.GetValuesIndex(row, nested.children_idx + i);
			if (!MemberMatchesValue(child_type, variant, row, child_index)) {
				return false;
			}
		}
		return true;
	}
	case LogicalTypeId::STRUCT: {
		if (value_type != VariantLogicalType::OBJECT) {
			return false;
		}
		auto &fields = StructType::GetChildTypes(member_type);
		auto nested = VariantUtils::DecodeNestedData(variant, row, value_index);
		for (idx_t i = 0; i < nested.child_count; i++) {
			auto &key = variant.GetKey(row, variant.GetKeysIndex(row, nested.children_idx + i));
			auto key_str = key.GetString();
			optional_idx field_idx;
			for (idx_t f = 0; f < fields.size(); f++) {
				if (StringUtil::CIEquals(fields[f].first.GetIdentifierName(), key_str)) {
					field_idx = f;
					break;
				}
			}
			if (!field_idx.IsValid()) {
				return false;
			}
			auto child_index = variant.GetValuesIndex(row, nested.children_idx + i);
			if (!MemberMatchesValue(fields[field_idx.GetIndex()].second, variant, row, child_index)) {
				return false;
			}
		}
		return true;
	}
	case LogicalTypeId::ENUM:
		if (value_type != VariantLogicalType::VARCHAR) {
			return false;
		}
		return EnumType::GetPos(member_type, VariantUtils::DecodeStringData(variant, row, value_index)) >= 0;
	default:
		return MemberAcceptsValue(member_type, value_type);
	}
}

static optional_idx FindMemberOfValue(const LogicalType &union_type, const UnifiedVariantVectorData &variant,
                                      idx_t row) {
	if (variant.GetTypeId(row, 0) == VariantLogicalType::VARIANT_NULL) {
		return optional_idx();
	}
	auto &members = UnionType::GetMemberTypes(union_type);
	for (idx_t member_idx = 0; member_idx < members.size(); member_idx++) {
		if (MemberMatchesValue(members[member_idx].second, variant, row, 0)) {
			return member_idx;
		}
	}
	return optional_idx();
}

optional_idx UnionVector::FindMember(const LogicalType &union_type, VariantLogicalType value_type) {
	auto &members = UnionType::GetMemberTypes(union_type);
	for (idx_t member_idx = 0; member_idx < members.size(); member_idx++) {
		if (MemberAcceptsValue(members[member_idx].second, value_type)) {
			return member_idx;
		}
	}
	return optional_idx();
}

void UnionVector::GetTags(const Vector &union_vector, Vector &result, idx_t count) {
	GetTags(union_vector, union_vector.GetType(), result, count);
}

void UnionVector::GetTags(const Vector &union_vector, const LogicalType &union_type, Vector &result, idx_t count) {
	D_ASSERT(union_vector.GetType().id() == LogicalTypeId::VARIANT);
	D_ASSERT(UnionType::IsUnion(union_type));
	D_ASSERT(result.GetType().id() == LogicalTypeId::UTINYINT);

	RecursiveUnifiedVectorFormat format;
	Vector::RecursiveToUnifiedFormat(union_vector, format);
	UnifiedVariantVectorData variant(format);

	auto writer = FlatVector::Writer<union_tag_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		if (!variant.RowIsValid(i)) {
			writer.WriteNull();
			continue;
		}
		auto member = FindMemberOfValue(union_type, variant, i);
		if (!member.IsValid()) {
			writer.WriteNull();
			continue;
		}
		writer.WriteValue(UnsafeNumericCast<union_tag_t>(member.GetIndex()));
	}
}

//! Casts VARIANT values to the member type - ENUMs are stored as VARCHAR in a VARIANT, so they are cast through VARCHAR
static void CastToMember(Vector &source, Vector &result, idx_t count) {
	auto &member_type = result.GetType();
	if (!TypeVisitor::Contains(member_type, LogicalTypeId::ENUM)) {
		VectorOperations::DefaultCast(source, result, count);
		return;
	}
	auto varchar_type = TypeVisitor::VisitReplace(member_type, [](const LogicalType &type) {
		return type.id() == LogicalTypeId::ENUM ? LogicalType(LogicalType::VARCHAR) : type;
	});
	Vector intermediate(varchar_type, count);
	VectorOperations::DefaultCast(source, intermediate, count);
	VectorOperations::DefaultCast(intermediate, result, count);
}

static void ExtractMember(const Vector &union_vector, const Vector &tags, idx_t member_index, Vector &result,
                          idx_t count) {
	auto tag_values = tags.Values<union_tag_t>();
	idx_t match_count = 0;
	for (auto entry : tag_values) {
		if (entry.IsValid() && entry.GetValue() == member_index) {
			match_count++;
		}
	}
	if (match_count == 0) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		ConstantVector::SetNull(result, count_t(count));
		return;
	}
	if (match_count == count) {
		auto source = Vector::Ref(union_vector);
		CastToMember(source, result, count);
		return;
	}
	// NULL out the rows that hold another member, then cast the remaining values to the member type
	Vector masked(union_vector.GetType(), count);
	VectorOperations::Copy(union_vector, masked, count, 0, 0);
	for (auto entry : tag_values) {
		if (!entry.IsValid() || entry.GetValue() != member_index) {
			FlatVector::SetNull(masked, entry.GetIndex(), true);
		}
	}
	CastToMember(masked, result, count);
}

void UnionVector::GetMember(const Vector &union_vector, idx_t member_index, Vector &result, idx_t count) {
	D_ASSERT(member_index < UnionType::GetMemberCount(union_vector.GetType()));
	D_ASSERT(result.GetType() == UnionType::GetMemberType(union_vector.GetType(), member_index));
	Vector tags(LogicalType::UTINYINT, count);
	GetTags(union_vector, tags, count);
	ExtractMember(union_vector, tags, member_index, result, count);
}

void UnionVector::ToLegacyLayout(const Vector &union_vector, Vector &result, idx_t count) {
	auto &union_type = union_vector.GetType();
	D_ASSERT(UnionType::IsUnion(union_type));
	D_ASSERT(result.GetType().id() == LogicalTypeId::STRUCT);
	auto &entries = StructVector::GetEntries(result);
	D_ASSERT(entries.size() == UnionType::GetMemberCount(union_type) + 1);

	auto &tags = entries[0];
	GetTags(union_vector, tags, count);
	for (idx_t member_idx = 0; member_idx < UnionType::GetMemberCount(union_type); member_idx++) {
		ExtractMember(union_vector, tags, member_idx, entries[member_idx + 1], count);
	}
	auto &result_validity = FlatVector::ValidityMutable(result);
	auto union_validity = union_vector.Validity();
	for (idx_t i = 0; i < count; i++) {
		if (!union_validity.IsValid(i)) {
			result_validity.SetInvalid(i);
		}
	}
	FlatVector::SetSize(result, count);
}

void UnionVector::FromLegacyLayout(Vector &legacy, Vector &result, idx_t count) {
	D_ASSERT(UnionType::IsUnion(result.GetType()));
	auto member_count = UnionType::GetMemberCount(result.GetType());
	auto tags = StructVector::GetEntries(legacy)[0].Values<union_tag_t>();
	for (auto tag : tags) {
		if (tag.IsValid() && tag.GetValue() >= member_count) {
			throw InvalidInputException("UNION tag %d is out of range for %s", tag.GetValue(),
			                            result.GetType().ToString());
		}
	}
	VariantUtils::LegacyUnionToVariant(legacy, result, count);
}

static bool ContainsUnion(const LogicalType &type) {
	return TypeVisitor::Contains(type, [](const LogicalType &child) { return UnionType::IsUnion(child); });
}

void UnionVector::FromLegacyStorage(Vector &source, Vector &result, idx_t count) {
	auto &type = result.GetType();
	if (!ContainsUnion(type)) {
		result.Reference(source);
		return;
	}
	if (UnionType::IsUnion(type)) {
		auto &members = UnionType::GetMemberTypes(type);
		bool nested = false;
		for (auto &member : members) {
			nested = nested || ContainsUnion(member.second);
		}
		if (!nested) {
			FromLegacyLayout(source, result, count);
			return;
		}
		// convert the members first
		Vector legacy(UnionType::GetMemberStructType(type), count);
		source.Flatten();
		auto &source_entries = StructVector::GetEntries(source);
		auto &legacy_entries = StructVector::GetEntries(legacy);
		legacy_entries[0].Reference(source_entries[0]);
		for (idx_t member_idx = 0; member_idx < members.size(); member_idx++) {
			FromLegacyStorage(source_entries[member_idx + 1], legacy_entries[member_idx + 1], count);
		}
		FlatVector::SetValidity(legacy, FlatVector::Validity(source));
		FlatVector::SetSize(legacy, count);
		FromLegacyLayout(legacy, result, count);
		return;
	}
	source.Flatten();
	switch (type.InternalType()) {
	case PhysicalType::STRUCT: {
		auto &source_entries = StructVector::GetEntries(source);
		auto &result_entries = StructVector::GetEntries(result);
		for (idx_t child_idx = 0; child_idx < result_entries.size(); child_idx++) {
			FromLegacyStorage(source_entries[child_idx], result_entries[child_idx], count);
		}
		break;
	}
	case PhysicalType::LIST: {
		auto source_data = FlatVector::GetData<list_entry_t>(source);
		auto result_data = FlatVector::GetDataMutable<list_entry_t>(result);
		for (idx_t i = 0; i < count; i++) {
			result_data[i] = source_data[i];
		}
		auto child_count = ListVector::GetListSize(source);
		ListVector::Reserve(result, child_count);
		auto &source_child = ListVector::GetChild(source);
		Vector source_child_ref = Vector::Ref(source_child);
		FromLegacyStorage(source_child_ref, ListVector::GetChildMutable(result), child_count);
		ListVector::SetListSize(result, child_count);
		break;
	}
	case PhysicalType::ARRAY: {
		auto child_count = count * ArrayType::GetSize(type);
		Vector source_child_ref = Vector::Ref(ArrayVector::GetChild(source));
		FromLegacyStorage(source_child_ref, ArrayVector::GetChildMutable(result), child_count);
		break;
	}
	default:
		throw InternalException("Unexpected type in UnionVector::FromLegacyStorage: %s", type.ToString());
	}
	FlatVector::SetValidity(result, FlatVector::Validity(source));
	FlatVector::SetSize(result, count);
}

} // namespace duckdb
