#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"

namespace duckdb {

void ArrayVector::SetConsecutiveEntries(Vector &vector, idx_t count, idx_t offset) {
	D_ASSERT(vector.GetType().id() == LogicalTypeId::ARRAY);
	auto array_size = ArrayType::GetSize(vector.GetType());
	ListVector::SetListSize(vector, count * array_size);
	auto entries = FlatVector::GetDataMutable<list_entry_t>(vector);
	for (idx_t i = offset; i < count; i++) {
		entries[i] = list_entry_t(i * array_size, array_size);
	}
}

static bool HasOrderedChild(const Vector &vector, const VectorIterator<list_entry_t> &entries, idx_t count) {
	auto array_size = ArrayType::GetSize(vector.GetType());
	if (ListVector::GetListSize(vector) < count * array_size) {
		return false;
	}
	for (idx_t i = 0; i < count; i++) {
		auto entry = entries[i];
		if (!entry.IsValid() || entry.GetValue().offset != i * array_size) {
			return false;
		}
	}
	return true;
}

Vector ArrayVector::GetOrderedChild(const Vector &vector, idx_t count) {
	D_ASSERT(vector.GetType().id() == LogicalTypeId::ARRAY);
	auto array_size = ArrayType::GetSize(vector.GetType());
	auto &child = ListVector::GetChild(vector);
	auto child_size = ListVector::GetListSize(vector);

	auto entries = vector.Values<list_entry_t>();
	if (HasOrderedChild(vector, entries, count)) {
		return Vector::Ref(child);
	}

	// NULL arrays select the first element, and are set to NULL below
	SelectionVector child_sel(count * array_size);
	for (idx_t i = 0; i < count; i++) {
		auto entry = entries[i];
		for (idx_t k = 0; k < array_size; k++) {
			child_sel.set_index(i * array_size + k, entry.IsValid() ? entry.GetValue().offset + k : 0);
		}
	}
	unique_ptr<Vector> null_source;
	if (child_size == 0) {
		// all arrays are NULL
		null_source = make_uniq<Vector>(child.GetType(), 1);
		null_source->SetValue(0, Value(child.GetType()));
	}
	Vector result(null_source ? *null_source : child, child_sel, count * array_size);
	result.Flatten();
	for (idx_t i = 0; i < count; i++) {
		if (entries[i].IsValid()) {
			continue;
		}
		for (idx_t k = 0; k < array_size; k++) {
			FlatVector::SetNull(result, i * array_size + k, true);
		}
	}
	return result;
}

void ArrayVector::MakeConsecutive(Vector &vector, idx_t count) {
	vector.Flatten();
	if (HasOrderedChild(vector, vector.Values<list_entry_t>(), count)) {
		return;
	}
	auto ordered = GetOrderedChild(vector, count);
	ListVector::GetChildMutable(vector).Reference(ordered);
	SetConsecutiveEntries(vector, count);
}

} // namespace duckdb
