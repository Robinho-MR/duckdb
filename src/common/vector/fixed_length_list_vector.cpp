#include "duckdb/common/vector/fixed_length_list_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

FixedLengthListBuffer::FixedLengthListBuffer(const Vector &child_p, idx_t list_length_p, count_t count_p)
    : VectorBuffer(VectorType::FIXED_LENGTH_LIST_VECTOR, VectorBufferType::FIXED_LENGTH_LIST_BUFFER, count_p),
      child(make_uniq<Vector>(Vector::Ref(child_p))), list_length(list_length_p) {
	validity.Resize(count_p);
}

FixedLengthListBuffer::~FixedLengthListBuffer() {
}

void FixedLengthListBuffer::SetVectorType(VectorType new_vector_type) {
	throw InternalException("FixedLengthListBuffer::SetVectorType is not supported - flatten the vector first");
}

idx_t FixedLengthListBuffer::GetDataSize(const LogicalType &type, idx_t count) const {
	return child->GetDataSize(count * list_length);
}

idx_t FixedLengthListBuffer::GetAllocationSize() const {
	return VectorBuffer::GetAllocationSize() + child->GetAllocationSize();
}

string FixedLengthListBuffer::ToString(const LogicalType &type, idx_t count) const {
	string result = "FIXED_LENGTH_LIST(" + to_string(list_length) + "): [";
	for (idx_t i = 0; i < count; i++) {
		if (i > 0) {
			result += ", ";
		}
		result += GetValue(type, i).ToString();
	}
	return result + "]";
}

Value FixedLengthListBuffer::GetValue(const LogicalType &type, idx_t index) const {
	if (!validity.RowIsValid(index)) {
		return Value(type);
	}
	vector<Value> children;
	children.reserve(list_length);
	for (idx_t i = 0; i < list_length; i++) {
		children.push_back(child->GetValue(index * list_length + i));
	}
	return Value::LIST(ListType::GetChildType(type), std::move(children));
}

//! Keeps a flattened fixed length list buffer alive, as references to its child can outlive the flatten
struct FixedLengthListBufferHolder : public AuxiliaryDataHolder {
	explicit FixedLengthListBufferHolder(buffer_ptr<const VectorBuffer> buffer_p) : buffer(std::move(buffer_p)) {
	}

	buffer_ptr<const VectorBuffer> buffer;
};

buffer_ptr<VectorBuffer> FixedLengthListBuffer::FlattenSliceInternal(const LogicalType &type,
                                                                     const SelectionVector &sel, idx_t count) const {
	auto result = make_buffer<VectorListBuffer>(Allocator::DefaultAllocator(), capacity_t(MaxValue<idx_t>(count, 1)),
	                                            type, capacity_t(0ULL));
	// the flat list references the same child entries
	result->GetChildMutable() = make_uniq<Vector>(Vector::Ref(*child));
	auto list_data = reinterpret_cast<list_entry_t *>(result->GetDataMutable());
	for (idx_t i = 0; i < count; i++) {
		list_data[i] = list_entry_t(sel.get_index(i) * list_length, list_length);
	}
	result->GetValidityMask().CopySel(validity, sel, 0, 0, count);
	result->SetVectorSize(count);
	result->AddAuxiliaryData(make_uniq<FixedLengthListBufferHolder>(buffer_from_this()));
	return result;
}

void FixedLengthListBuffer::VerifyInternal(const LogicalType &type, const SelectionVector &sel, idx_t count) const {
	D_ASSERT(type.InternalType() == PhysicalType::LIST);
	D_ASSERT(vector_type == VectorType::FIXED_LENGTH_LIST_VECTOR);
	D_ASSERT(child->size() >= Size() * list_length);
	D_ASSERT(child->GetType() == ListType::GetChildType(type));
	child->Verify();
}

void FixedLengthListVector::Verify(const Vector &vector) {
	if (vector.GetVectorType() != VectorType::FIXED_LENGTH_LIST_VECTOR) {
		throw InternalException("Operation requires a fixed length list vector");
	}
}

void FixedLengthListVector::Create(Vector &vector, const Vector &child, idx_t list_length, idx_t count) {
	D_ASSERT(vector.GetType().InternalType() == PhysicalType::LIST);
	D_ASSERT(child.size() >= count * list_length);
	Vector result(vector.GetType(), make_buffer<FixedLengthListBuffer>(child, list_length, count_t(count)));
	vector.Reference(result);
}

idx_t FixedLengthListVector::GetListLength(const Vector &vector) {
	Verify(vector);
	return vector.GetBufferRef()->Cast<FixedLengthListBuffer>().GetListLength();
}

const Vector &FixedLengthListVector::GetChild(const Vector &vector) {
	Verify(vector);
	return vector.GetBufferRef()->Cast<FixedLengthListBuffer>().GetChild();
}

Vector &FixedLengthListVector::GetChildMutable(Vector &vector) {
	Verify(vector);
	return vector.BufferMutable().Cast<FixedLengthListBuffer>().GetChild();
}

const ValidityMask &FixedLengthListVector::Validity(const Vector &vector) {
	Verify(vector);
	return vector.GetBufferRef()->GetValidityMask();
}

ValidityMask &FixedLengthListVector::ValidityMutable(Vector &vector) {
	Verify(vector);
	return vector.BufferMutable().GetValidityMask();
}

void FixedLengthListVector::DebugTransform(Vector &vector, idx_t count) {
	if (count == 0 || vector.GetType().id() != LogicalTypeId::LIST ||
	    vector.GetVectorType() != VectorType::FLAT_VECTOR) {
		return;
	}
	auto list_entries = FlatVector::GetData<list_entry_t>(vector);
	auto &list_validity = FlatVector::Validity(vector);
	optional_idx list_length;
	if (!list_validity.CheckAllValid(count)) {
		// the child of a list holds no entries for NULL lists
		return;
	}
	for (idx_t i = 0; i < count; i++) {
		if (!list_length.IsValid()) {
			list_length = list_entries[i].length;
		} else if (list_entries[i].length != list_length.GetIndex()) {
			return;
		}
	}
	if (!list_length.IsValid()) {
		return;
	}
	auto length = list_length.GetIndex();
	auto &list_child = ListVector::GetChild(vector);
	// the child holds the entries of every list in order
	auto child_count = count * length;
	SelectionVector child_sel(MaxValue<idx_t>(child_count, 1));
	for (idx_t i = 0; i < count; i++) {
		for (idx_t j = 0; j < length; j++) {
			child_sel.set_index(i * length + j, list_entries[i].offset + j);
		}
	}
	Vector child(list_child.GetType(), MaxValue<idx_t>(child_count, STANDARD_VECTOR_SIZE));
	VectorOperations::Copy(list_child, child, child_sel, child_count, 0, 0);
	FlatVector::SetSize(child, count_t(child_count));
	Create(vector, child, length, count);
}

} // namespace duckdb
