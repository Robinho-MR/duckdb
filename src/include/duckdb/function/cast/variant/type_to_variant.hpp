#pragma once

#include "duckdb/common/types/variant/variant_type_encoding.hpp"
#include "duckdb/function/cast/variant/to_variant_fwd.hpp"

namespace duckdb {
namespace variant {

//! Writes the values of a decomposed type (see VariantTypeEncoding::Emit) into the result of a cast to VARIANT, the
//! same way for both passes: the sizes are collected in the first one, and written in the second one
template <bool WRITE_DATA>
class TypeToVariantSink {
public:
	TypeToVariantSink(ToVariantGlobalResultData &result, idx_t result_index, idx_t i,
	                  optional_ptr<const SelectionVector> values_index_selvec)
	    : result(result), result_index(result_index), i(i), values_index_selvec(values_index_selvec),
	      values_offsets(OffsetData::GetValues(result.offsets)),
	      blob_offset(OffsetData::GetBlob(result.offsets)[result_index]),
	      children_offset(OffsetData::GetChildren(result.offsets)[result_index]) {
	}

public:
	//! A nested value, 'emit_child(i)' emits the i-th child
	template <class EMIT_CHILD>
	void Container(VariantLogicalType type_id, idx_t child_count, EMIT_CHILD &&emit_child) {
		BeginValue(type_id);
		WriteContainerData<WRITE_DATA>(result.variant, result_index, blob_offset, child_count, children_offset);
		const auto first_child = children_offset;
		children_offset += NumericCast<uint32_t>(child_count);
		for (idx_t child = 0; child < child_count; child++) {
			//! The value that is emitted next is this child
			parent_child_index = first_child + child;
			has_parent = true;
			emit_child(child);
		}
	}

	void Null() {
		BeginValue(VariantLogicalType::VARIANT_NULL);
	}

	void Scalar(const Value &value) {
		if (value.IsNull()) {
			return Null();
		}
		switch (value.type().id()) {
		case LogicalTypeId::BOOLEAN:
			BeginValue(BooleanValue::Get(value) ? VariantLogicalType::BOOL_TRUE : VariantLogicalType::BOOL_FALSE);
			return;
		case LogicalTypeId::TINYINT:
			return Fixed(VariantLogicalType::INT8, value.GetValueUnsafe<int8_t>());
		case LogicalTypeId::SMALLINT:
			return Fixed(VariantLogicalType::INT16, value.GetValueUnsafe<int16_t>());
		case LogicalTypeId::INTEGER:
			return Fixed(VariantLogicalType::INT32, value.GetValueUnsafe<int32_t>());
		case LogicalTypeId::BIGINT:
			return Fixed(VariantLogicalType::INT64, value.GetValueUnsafe<int64_t>());
		case LogicalTypeId::UTINYINT:
			return Fixed(VariantLogicalType::UINT8, value.GetValueUnsafe<uint8_t>());
		case LogicalTypeId::USMALLINT:
			return Fixed(VariantLogicalType::UINT16, value.GetValueUnsafe<uint16_t>());
		case LogicalTypeId::UINTEGER:
			return Fixed(VariantLogicalType::UINT32, value.GetValueUnsafe<uint32_t>());
		case LogicalTypeId::UBIGINT:
			return Fixed(VariantLogicalType::UINT64, value.GetValueUnsafe<uint64_t>());
		case LogicalTypeId::FLOAT:
			return Fixed(VariantLogicalType::FLOAT, value.GetValueUnsafe<float>());
		case LogicalTypeId::DOUBLE:
			return Fixed(VariantLogicalType::DOUBLE, value.GetValueUnsafe<double>());
		case LogicalTypeId::VARCHAR:
			return String(StringValue::Get(value));
		default:
			throw InternalException("Value of type %s can't be a modifier of a type", value.type().ToString());
		}
	}

private:
	//! Adds the value to the row, and makes the parent point to it
	void BeginValue(VariantLogicalType type_id) {
		const auto value_index = values_offsets[result_index];
		//! The first value is linked to its parent by the caller, through 'values_index_selvec'
		WriteVariantMetadata<WRITE_DATA>(result, result_index, values_offsets, blob_offset,
		                                 has_parent ? nullptr : values_index_selvec, i, type_id);
		if (has_parent && WRITE_DATA) {
			auto &variant = result.variant;
			const auto child_index = variant.children_data[result_index].offset + parent_child_index;
			variant.values_index_data[child_index] = value_index;
			variant.keys_index_validity.SetInvalid(child_index);
		}
	}

	data_ptr_t BlobData() {
		return data_ptr_cast(result.variant.blob_data[result_index].GetDataWriteable()) + blob_offset;
	}

	template <class T>
	void Fixed(VariantLogicalType type_id, T value) {
		BeginValue(type_id);
		if (WRITE_DATA) {
			Store<T>(value, BlobData());
		}
		OffsetData::AddBlobOffset(blob_offset, sizeof(T));
	}

	void String(const string &value) {
		BeginValue(VariantLogicalType::VARCHAR);
		const auto length = NumericCast<uint32_t>(value.size());
		const auto length_size = GetVarintSize(length);
		if (WRITE_DATA) {
			auto data = BlobData();
			VarintEncode(length, data);
			memcpy(data + length_size, value.data(), value.size());
		}
		OffsetData::AddBlobOffset(blob_offset, length_size + length);
	}

private:
	ToVariantGlobalResultData &result;
	idx_t result_index;
	idx_t i;
	optional_ptr<const SelectionVector> values_index_selvec;
	uint32_t *values_offsets;
	uint32_t &blob_offset;
	uint32_t &children_offset;
	//! Whether the value that is emitted next is the child of another value, and its (row relative) child index
	bool has_parent = false;
	idx_t parent_child_index = 0;
};

//! Converts a vector of TYPE values: every type is a nested value, see VariantTypeEncoding
template <bool WRITE_DATA, bool IGNORE_NULLS>
bool ConvertTypeToVariant(ToVariantSourceData &source, ToVariantGlobalResultData &result, idx_t count,
                          optional_ptr<const SelectionVector> selvec,
                          optional_ptr<const SelectionVector> values_index_selvec, const bool is_root) {
	auto blob_offset_data = OffsetData::GetBlob(result.offsets);
	auto values_offset_data = OffsetData::GetValues(result.offsets);

	auto &source_format = source.source_format;
	auto &source_validity = source_format.validity;
	auto source_data = source_format.GetData<string_t>(source_format);

	for (idx_t i = 0; i < count; i++) {
		const auto index = source[i];
		const auto result_index = selvec ? selvec->get_index(i) : i;
		if (!source_validity.RowIsValid(index)) {
			if (!IGNORE_NULLS) {
				HandleVariantNull<WRITE_DATA>(result, result_index, values_offset_data, blob_offset_data[result_index],
				                              values_index_selvec, i, is_root);
			}
			continue;
		}
		auto type = VariantTypeEncoding::Deserialize(source_data[index]);
		auto description = VariantTypeEncoding::Decompose(type);
		TypeToVariantSink<WRITE_DATA> sink(result, result_index, i, values_index_selvec);
		VariantTypeEncoding::Emit(description, sink);
	}
	return true;
}

} // namespace variant
} // namespace duckdb
