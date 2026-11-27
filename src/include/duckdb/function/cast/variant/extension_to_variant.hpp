#pragma once

#include "duckdb/function/cast/variant/to_variant_fwd.hpp"
#include "duckdb/function/cast/variant/type_to_variant.hpp"

namespace duckdb {
namespace variant {

//! Converts a value of an extension type to an EXTENSION value: its type (a TYPE value, see VariantTypeEncoding) and
//! its value, converted as the type it is stored as
template <bool WRITE_DATA, bool IGNORE_NULLS>
bool ConvertExtensionToVariant(ToVariantSourceData &source, ToVariantGlobalResultData &result, idx_t count,
                               optional_ptr<const SelectionVector> selvec,
                               optional_ptr<const SelectionVector> values_index_selvec, const bool is_root) {
	auto blob_offset_data = OffsetData::GetBlob(result.offsets);
	auto values_offset_data = OffsetData::GetValues(result.offsets);
	auto children_offset_data = OffsetData::GetChildren(result.offsets);

	auto &source_validity = source.source_format.validity;
	auto &variant = result.variant;

	auto &source_type = source.vec.GetType();
	//! The type is the same for every value, it is decomposed once
	auto description = VariantTypeEncoding::Decompose(source_type);

	ContainerSelectionVectors sel(count);
	for (idx_t i = 0; i < count; i++) {
		const auto index = source[i];
		const auto result_index = selvec ? selvec->get_index(i) : i;
		auto &blob_offset = blob_offset_data[result_index];

		if (!source_validity.RowIsValid(index)) {
			if (!IGNORE_NULLS) {
				HandleVariantNull<WRITE_DATA>(result, result_index, values_offset_data, blob_offset,
				                              values_index_selvec, i, is_root);
			}
			continue;
		}
		WriteVariantMetadata<WRITE_DATA>(result, result_index, values_offset_data, blob_offset, values_index_selvec, i,
		                                 VariantLogicalType::EXTENSION);
		auto &children_offset = children_offset_data[result_index];
		WriteContainerData<WRITE_DATA>(variant, result_index, blob_offset, 2, children_offset);

		if (WRITE_DATA) {
			//! The children have no keys
			auto children_index = variant.children_data[result_index].offset + children_offset;
			variant.keys_index_validity.SetInvalid(children_index);
			variant.keys_index_validity.SetInvalid(children_index + 1);
			sel.children_selection.set_index(sel.count, children_index);
		}
		sel.new_selection.set_index(sel.count, result_index);
		sel.non_null_selection.set_index(sel.count, source.GetMappedIndex(i));
		children_offset += 2;
		sel.count++;
	}
	if (!sel.count) {
		return true;
	}

	//! The first child is the type
	for (idx_t i = 0; i < sel.count; i++) {
		TypeToVariantSink<WRITE_DATA> sink(result, sel.new_selection[i], i, &sel.children_selection);
		VariantTypeEncoding::Emit(description, sink);
	}
	if (WRITE_DATA) {
		for (idx_t i = 0; i < sel.count; i++) {
			sel.children_selection[i]++;
		}
	}

	//! The second child is the value, read as the type it is stored as
	Vector unaliased(VariantUtils::GetUnaliasedType(source_type), nullptr);
	unaliased.Reinterpret(source.vec);
	ToVariantSourceData child_source_data(unaliased, source.source_size, sel.non_null_selection);
	return ConvertToVariant<WRITE_DATA, false>(child_source_data, result, sel.count, &sel.new_selection,
	                                           &sel.children_selection, false);
}

} // namespace variant
} // namespace duckdb
