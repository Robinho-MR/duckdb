#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/arrow/arrow_appender.hpp"
#include "duckdb/common/arrow/appender/union_data.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector/union_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Unions
//===--------------------------------------------------------------------===//
void ArrowUnionData::Initialize(ArrowAppendData &result, const LogicalType &type, idx_t capacity) {
	result.GetMainBuffer().reserve(capacity * sizeof(int8_t));

	for (auto &child : UnionType::CopyMemberTypes(type)) {
		auto child_buffer = ArrowAppender::InitializeChild(child.second, capacity, result.options);
		result.child_data.push_back(std::move(child_buffer));
	}
}

void ArrowUnionData::Append(ArrowAppendData &append_data, const Vector &input, idx_t from, idx_t to, idx_t input_size) {
	idx_t size = to - from;
	auto &types_buffer = append_data.GetMainBuffer();

	// split the union into one vector per member
	Vector slice(input, from, to);
	Vector members(UnionType::GetMemberStructType(input.GetType()), size);
	UnionVector::ToLegacyLayout(slice, members, size);
	auto &entries = StructVector::GetEntries(members);

	auto tags = entries[0].Values<union_tag_t>();
	for (auto tag : tags) {
		types_buffer.push_back<data_t>(tag.IsValid() ? NumericCast<data_t>(tag.GetValue()) : 0);
	}
	for (idx_t child_idx = 0; child_idx + 1 < entries.size(); child_idx++) {
		append_data.child_data[child_idx]->AppendChild(entries[child_idx + 1], 0, size, size);
	}
	append_data.row_count += size;
}

void ArrowUnionData::Finalize(ArrowAppendData &append_data, const LogicalType &type, ArrowArray *result) {
	result->n_buffers = 1;
	result->buffers[0] = append_data.GetMainBuffer().data();

	auto &child_types = UnionType::CopyMemberTypes(type);
	ArrowAppender::AddChildren(append_data, child_types.size());
	result->children = append_data.child_pointers.data();
	result->n_children = NumericCast<int64_t>(child_types.size());
	for (idx_t i = 0; i < child_types.size(); i++) {
		auto &child_type = child_types[i].second;
		append_data.child_arrays[i] = *ArrowAppender::FinalizeChild(child_type, std::move(append_data.child_data[i]));
	}
}

} // namespace duckdb
