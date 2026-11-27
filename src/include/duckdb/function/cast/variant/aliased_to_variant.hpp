#pragma once

#include "duckdb/function/cast/variant/to_variant_fwd.hpp"

namespace duckdb {
namespace variant {

//! Converts a value of an aliased type to an ALIASED value: the alias and extension info of the type, and a single
//! child holding the value converted as its underlying type
template <bool WRITE_DATA, bool IGNORE_NULLS>
bool ConvertAliasedToVariant(ToVariantSourceData &source, ToVariantGlobalResultData &result, idx_t count,
                             optional_ptr<const SelectionVector> selvec,
                             optional_ptr<const SelectionVector> value_index_selvec, const bool is_root) {
	auto blob_offset_data = OffsetData::GetBlob(result.offsets);
	auto values_offset_data = OffsetData::GetValues(result.offsets);
	auto children_offset_data = OffsetData::GetChildren(result.offsets);

	auto &source_validity = source.source_format.validity;
	auto &variant = result.variant;

	auto &source_type = source.vec.GetType();
	//! The payload up to the children index is the same for every value: the alias and the extension info
	string header;
	for (auto &str : {source_type.GetAlias(), VariantUtils::SerializeExtensionInfo(source_type)}) {
		auto length = NumericCast<uint32_t>(str.size());
		auto offset = header.size();
		header.resize(offset + GetVarintSize(length));
		VarintEncode(length, data_ptr_cast(&header[offset]));
		header += str;
	}

	ContainerSelectionVectors sel(count);
	for (idx_t i = 0; i < count; i++) {
		const auto index = source[i];
		const auto result_index = selvec ? selvec->get_index(i) : i;
		auto &blob_offset = blob_offset_data[result_index];

		if (!source_validity.RowIsValid(index)) {
			if (!IGNORE_NULLS) {
				HandleVariantNull<WRITE_DATA>(result, result_index, values_offset_data, blob_offset, value_index_selvec,
				                              i, is_root);
			}
			continue;
		}
		WriteVariantMetadata<WRITE_DATA>(result, result_index, values_offset_data, blob_offset, value_index_selvec, i,
		                                 VariantLogicalType::ALIASED);

		auto &children_offset = children_offset_data[result_index];
		const auto children_offset_size = GetVarintSize(children_offset);
		if (WRITE_DATA) {
			auto blob_data = data_ptr_cast(variant.blob_data[result_index].GetDataWriteable()) + blob_offset;
			memcpy(blob_data, header.data(), header.size());
			VarintEncode(children_offset, blob_data + header.size());

			//! The single child holds the underlying value, it has no key
			auto children_index = variant.children_data[result_index].offset + children_offset;
			sel.children_selection.set_index(sel.count, children_index);
			variant.keys_index_validity.SetInvalid(children_index);
		}
		OffsetData::AddBlobOffset(blob_offset, header.size() + children_offset_size);
		sel.new_selection.set_index(sel.count, result_index);
		sel.non_null_selection.set_index(sel.count, source.GetMappedIndex(i));
		children_offset++;
		sel.count++;
	}
	if (!sel.count) {
		return true;
	}

	//! Convert the underlying values, reading the source as its underlying type
	Vector unaliased(VariantUtils::GetUnaliasedType(source_type), nullptr);
	unaliased.Reinterpret(source.vec);
	ToVariantSourceData child_source_data(unaliased, source.source_size, sel.non_null_selection);
	return ConvertToVariant<WRITE_DATA, false>(child_source_data, result, sel.count, &sel.new_selection,
	                                           &sel.children_selection, false);
}

} // namespace variant
} // namespace duckdb
