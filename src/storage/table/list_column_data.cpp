#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/storage/table/list_column_data.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"
#include "duckdb/storage/statistics/array_stats.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"
#include "duckdb/storage/table/struct_column_data.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/storage/table/column_checkpoint_state.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/block_manager.hpp"

namespace duckdb {

[[noreturn]] static void ThrowListOffsetOutOfRange() {
	throw DataCorruptionException("Corrupted LIST column: offset exceeds the child column count");
}

[[noreturn]] static void ThrowListOffsetsOutOfOrder() {
	throw DataCorruptionException("Corrupted LIST column: later offset is smaller than the preceding offset");
}

static void ValidateListOffset(idx_t offset, idx_t child_count) {
	if (offset > child_count) {
		ThrowListOffsetOutOfRange();
	}
}

static idx_t GetListLength(idx_t start_offset, idx_t end_offset, idx_t child_count) {
	if (end_offset < start_offset) {
		ThrowListOffsetsOutOfOrder();
	}
	ValidateListOffset(end_offset, child_count);
	return end_offset - start_offset;
}

ListColumnData::ListColumnData(BlockManager &block_manager, DataTableInfo &info, idx_t column_index, LogicalType type_p,
                               ColumnDataType data_type, optional_ptr<ColumnData> parent)
    : ColumnData(block_manager, info, column_index, std::move(type_p), data_type, parent) {
	D_ASSERT(type.InternalType() == PhysicalType::LIST);
	if (data_type != ColumnDataType::CHECKPOINT_TARGET) {
		auto &child_type = ListType::GetChildType(type);
		validity = make_shared_ptr<ValidityColumnData>(block_manager, info, 0, *this);
		// the child column, with column index 1 (0 is the validity mask)
		child_column = CreateColumn(block_manager, info, 1, child_type, data_type, this);
	}
}

void ListColumnData::SetDataType(ColumnDataType data_type) {
	ColumnData::SetDataType(data_type);
	child_column->SetDataType(data_type);
	validity->SetDataType(data_type);
}

FilterPropagateResult ListColumnData::CheckZonemap(ColumnScanState &state, const ScanFilter &filter,
                                                   optional_idx &checked_end) {
	return CheckValidityZonemap(state, filter, checked_end, *validity);
}

void ListColumnData::InitializePrefetch(PrefetchState &prefetch_state, ColumnScanState &scan_state, idx_t start_row,
                                        idx_t end_row) {
	ColumnData::InitializePrefetch(prefetch_state, scan_state, start_row, end_row);
	validity->InitializePrefetch(prefetch_state, scan_state.child_states[0], start_row, end_row);

	// we can't know how many rows we need to prefetch for the child of this list without looking at the actual data
	// we make an estimation by looking at how many rows the child column has versus this column
	// e.g if the child column has 10K rows, and we have 1K rows, we estimate that each list has 10 elements
	idx_t rows_per_list = 1;
	if (child_column->count > this->count && this->count > 0) {
		rows_per_list = child_column->count / this->count;
	}
	// the child rows of the range are estimated from the child row the list scan is at, which last_offset holds
	D_ASSERT(start_row >= scan_state.offset_in_column);
	const idx_t child_start = scan_state.last_offset + (start_row - scan_state.offset_in_column) * rows_per_list;
	if (child_start >= child_column->GetMaxEntry()) {
		// no child rows remain
		return;
	}
	const idx_t child_end = scan_state.last_offset + (end_row - scan_state.offset_in_column) * rows_per_list;
	child_column->InitializePrefetch(prefetch_state, scan_state.child_states[1], child_start, child_end);
}

bool ListColumnData::HasImpliedOffsets() const {
	return implied_list_length.IsValid();
}

bool ListColumnData::ScansImpliedOffsets(const ColumnScanState &state) const {
	// the scan of implied offsets has no offset segment
	return HasImpliedOffsets() && !state.current;
}

void ListColumnData::InitializeImpliedScan(ColumnScanState &state, idx_t row_idx) {
	D_ASSERT(row_idx <= implied_count);
	state.current = nullptr;
	state.segment_tree = &data;
	state.offset_in_column = row_idx;
	state.internal_index = row_idx;
	state.initialized = false;
	state.scan_state.reset();
	state.last_offset = 0;
}

idx_t ListColumnData::CountValidLists(idx_t start_row, idx_t row_count) {
	if (row_count == 0) {
		return 0;
	}
	if (!implied_can_have_null && start_row + row_count <= implied_count) {
		return row_count;
	}
	ColumnScanState validity_state(nullptr);
	validity->InitializeScanWithOffset(validity_state, start_row);
	idx_t valid_count = 0;
	for (idx_t scanned = 0; scanned < row_count; scanned += STANDARD_VECTOR_SIZE) {
		auto scan_count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, row_count - scanned);
		Vector validity_vector(LogicalType::BOOLEAN, scan_count);
		validity->ScanCount(validity_state, validity_vector, scan_count);
		valid_count += FlatVector::Validity(validity_vector).CountValid(scan_count);
	}
	return valid_count;
}

idx_t ListColumnData::ImpliedOffset(idx_t row_idx) {
	D_ASSERT(HasImpliedOffsets() && row_idx <= implied_count);
	return implied_list_length.GetIndex() * CountValidLists(0, row_idx);
}

void ListColumnData::InitializeScan(ColumnScanState &state) {
	if (HasImpliedOffsets()) {
		InitializeImpliedScan(state, 0);
	} else {
		ColumnData::InitializeScan(state);
	}

	// initialize the validity segment
	D_ASSERT(state.child_states.size() == 2);
	validity->InitializeScan(state.child_states[0]);

	// initialize the child scan
	child_column->InitializeScan(state.child_states[1]);
}

uint64_t ListColumnData::FetchListOffset(idx_t row_idx) {
	if (row_idx >= count) {
		throw DataCorruptionException("Corrupted database: list offset row ID is out of range");
	}
	if (HasImpliedOffsets() && row_idx < implied_count) {
		auto offset = ImpliedOffset(row_idx + 1);
		ValidateListOffset(offset, child_column->GetMaxEntry());
		return offset;
	}
	ColumnScanState offset_state(nullptr);
	ColumnData::InitializeScanWithOffset(offset_state, row_idx);
	Vector result(LogicalType::UBIGINT, 1);
	ScanVector(offset_state, result, 1, ScanVectorType::SCAN_FLAT_VECTOR);
	auto offset = FlatVector::GetData<uint64_t>(result)[0];
	ValidateListOffset(offset, child_column->GetMaxEntry());

	// initialize the child scan with the required offset
	return offset;
}

void ListColumnData::InitializeScanWithOffset(ColumnScanState &state, idx_t row_idx) {
	if (row_idx == 0) {
		InitializeScan(state);
		return;
	}
	if (HasImpliedOffsets() && (row_idx < implied_count || !data.GetRootSegment())) {
		InitializeImpliedScan(state, row_idx);
	} else {
		ColumnData::InitializeScanWithOffset(state, row_idx);
	}

	// initialize the validity segment
	D_ASSERT(state.child_states.size() == 2);
	validity->InitializeScanWithOffset(state.child_states[0], row_idx);

	// we need to read the list at position row_idx to get the correct row offset of the child
	auto child_offset = FetchListOffset(row_idx - 1);
	if (child_offset < child_column->GetMaxEntry()) {
		child_column->InitializeScanWithOffset(state.child_states[1], child_offset);
	} else {
		// no child rows remain, start the child scan over so that it does not keep the position of an earlier scan
		child_column->InitializeScan(state.child_states[1]);
	}
	state.last_offset = child_offset;
}

idx_t ListColumnData::Scan(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                           idx_t scan_count) {
	return ScanCount(state, result, scan_count);
}

idx_t ListColumnData::ScanCount(ColumnScanState &state, Vector &result, idx_t count, idx_t result_offset) {
	if (result_offset > 0) {
		throw InternalException("ListColumnData::ScanCount not supported with result_offset > 0");
	}
	if (count == 0) {
		return 0;
	}
	// updates not supported for lists
	D_ASSERT(!updates);

	if (ScansImpliedOffsets(state) && state.offset_in_column + count > implied_count && data.GetRootSegment()) {
		// the rows past the implied offsets were appended later, and their offsets are stored
		auto last_offset = state.last_offset;
		ColumnData::InitializeScanWithOffset(state, state.offset_in_column);
		state.last_offset = last_offset;
	}
	idx_t scan_count;
	idx_t child_scan_count;
	auto previous_offset = state.last_offset;
	auto child_count = child_column->GetMaxEntry();
	if (ScansImpliedOffsets(state)) {
		// every non-NULL list has the same length
		D_ASSERT(state.offset_in_column + count <= implied_count);
		scan_count = count;
		validity->ScanCount(state.child_states[0], result, count);
		auto &list_validity = FlatVector::Validity(result);
		auto list_length = implied_list_length.GetIndex();
		auto result_data = FlatVector::Writer<list_entry_t>(result, scan_count);
		idx_t current_offset = 0;
		for (idx_t i = 0; i < scan_count; i++) {
			auto length = list_validity.RowIsValid(i) ? list_length : 0;
			result_data.WriteValue(list_entry_t(current_offset, length));
			current_offset += length;
		}
		child_scan_count = current_offset;
		previous_offset += child_scan_count;
		ValidateListOffset(previous_offset, child_count);
		state.offset_in_column += scan_count;
		state.internal_index = state.offset_in_column;
	} else {
		Vector offset_vector(LogicalType::UBIGINT, count);
		scan_count = ScanVector(state, offset_vector, count, ScanVectorType::SCAN_FLAT_VECTOR);
		D_ASSERT(scan_count > 0);
		validity->ScanCount(state.child_states[0], result, count);

		auto data = offset_vector.Values<uint64_t>();
		// shift all offsets so they are 0 at the first entry
		auto result_data = FlatVector::Writer<list_entry_t>(result, scan_count);
		idx_t current_offset = 0;
		for (idx_t i = 0; i < scan_count; i++) {
			auto offset = data[i].GetValueUnsafe();
			auto length = GetListLength(previous_offset, offset, child_count);
			result_data.WriteValue(list_entry_t(current_offset, length));
			current_offset += length;
			previous_offset = offset;
		}
		child_scan_count = current_offset;
	}

	ListVector::Reserve(result, child_scan_count);

	if (child_scan_count > 0) {
		auto &child_entry = ListVector::GetChildMutable(result);
		if (child_entry.GetType().InternalType() != PhysicalType::STRUCT &&
		    child_entry.GetType().InternalType() != PhysicalType::ARRAY &&
		    state.child_states[1].offset_in_column + child_scan_count > child_column->GetMaxEntry()) {
			throw InternalException("ListColumnData::ScanCount - internal list scan offset is out of range");
		}
		child_column->ScanCount(state.child_states[1], child_entry, child_scan_count);
	}
	state.last_offset = previous_offset;

	ListVector::SetListSize(result, child_scan_count);
	return scan_count;
}

void ListColumnData::Skip(ColumnScanState &state, idx_t count) {
	if (ScansImpliedOffsets(state) && state.offset_in_column + count > implied_count && data.GetRootSegment()) {
		auto last_offset = state.last_offset;
		ColumnData::InitializeScanWithOffset(state, state.offset_in_column);
		state.last_offset = last_offset;
	}
	if (ScansImpliedOffsets(state)) {
		D_ASSERT(state.offset_in_column <= implied_count);
		count = MinValue<idx_t>(count, implied_count - state.offset_in_column);
		auto child_skip_count = implied_list_length.GetIndex() * CountValidLists(state.offset_in_column, count);
		validity->Skip(state.child_states[0], count);
		state.offset_in_column += count;
		state.internal_index = state.offset_in_column;
		if (child_skip_count == 0) {
			return;
		}
		state.last_offset += child_skip_count;
		ValidateListOffset(state.last_offset, child_column->GetMaxEntry());
		child_column->Skip(state.child_states[1], child_skip_count);
		return;
	}
	// skip inside the validity segment
	validity->Skip(state.child_states[0], count);

	// we need to read the list entries/offsets to figure out how much to skip
	// note that we only need to read the first and last entry
	// however, let's just read all "count" entries for now
	Vector offset_vector(LogicalType::UBIGINT, count);
	idx_t scan_count = ScanVector(state, offset_vector, count, ScanVectorType::SCAN_FLAT_VECTOR);
	if (scan_count == 0) {
		// no offsets were scanned - there is nothing to skip in the child, and reading offset[-1] below would be
		// an out-of-bounds read
		return;
	}

	UnifiedVectorFormat offsets;
	offset_vector.ToUnifiedFormat(offsets);
	auto data = UnifiedVectorFormat::GetData<uint64_t>(offsets);
	auto last_entry = data[offsets.sel->get_index(scan_count - 1)];
	idx_t child_scan_count = GetListLength(state.last_offset, last_entry, child_column->GetMaxEntry());
	if (child_scan_count == 0) {
		return;
	}
	state.last_offset = last_entry;

	// skip the child state forward by the child_scan_count
	child_column->Skip(state.child_states[1], child_scan_count);
}

void ListColumnData::MaterializeImpliedOffsets(ColumnAppendState &state) {
	D_ASSERT(HasImpliedOffsets() && count == implied_count);
	ColumnAppendState offset_state;
	offset_state.transient = state.transient;
	ColumnData::InitializeAppend(offset_state);

	ColumnScanState validity_state(nullptr);
	validity->InitializeScan(validity_state);
	auto list_length = implied_list_length.GetIndex();
	auto offsets = unique_ptr<uint64_t[]>(new uint64_t[STANDARD_VECTOR_SIZE]);
	UnifiedVectorFormat vdata;
	vdata.sel = FlatVector::IncrementalSelectionVector();
	vdata.data = data_ptr_cast(offsets.get());
	vdata.physical_type = PhysicalType::UINT64;
	idx_t current_offset = 0;
	auto total_count = implied_count;
	for (idx_t row_idx = 0; row_idx < total_count; row_idx += STANDARD_VECTOR_SIZE) {
		auto append_count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, total_count - row_idx);
		Vector validity_vector(LogicalType::BOOLEAN, append_count);
		validity->ScanCount(validity_state, validity_vector, append_count);
		auto &list_validity = FlatVector::Validity(validity_vector);
		for (idx_t i = 0; i < append_count; i++) {
			current_offset += list_validity.RowIsValid(i) ? list_length : 0;
			offsets[i] = current_offset;
		}
		ColumnData::AppendData(offset_state, vdata, append_count);
	}
	// the rows were already counted
	count -= total_count;
	D_ASSERT(count == total_count);
}

void ListColumnData::InitializeAppend(ColumnAppendState &state) {
	if (HasImpliedOffsets() && !data.GetRootSegment()) {
		// the offsets of the new lists are stored after the offsets of the existing lists
		MaterializeImpliedOffsets(state);
	}
	// initialize the list offset append
	ColumnData::InitializeAppend(state);

	// initialize the validity append
	ColumnAppendState validity_append_state;
	validity_append_state.transient = state.transient;
	validity->InitializeAppend(validity_append_state);
	state.child_appends.push_back(std::move(validity_append_state));

	// initialize the child column append
	ColumnAppendState child_append_state;
	child_append_state.transient = state.transient;
	child_column->InitializeAppend(child_append_state);
	state.child_appends.push_back(std::move(child_append_state));
}

void ListColumnData::Append(ColumnAppendState &state, const Vector &vector, idx_t count) {
	D_ASSERT(count > 0);

	// construct the list_entry_t entries to append to the column data
	auto input_offsets = vector.Values<list_entry_t>();
	auto start_offset = child_column->GetMaxEntry();
	idx_t child_count = 0;
	idx_t min_length = NumericLimits<idx_t>::Maximum();
	idx_t max_length = 0;
	bool has_valid_list = false;

	ValidityMask append_mask(count);
	auto append_offsets = unique_ptr<uint64_t[]>(new uint64_t[count]);
	bool child_contiguous = true;
	for (idx_t i = 0; i < count; i++) {
		auto list_entry = input_offsets[i];
		if (!list_entry.IsValid()) {
			append_mask.SetInvalid(i);
			append_offsets[i] = start_offset + child_count;
			continue;
		}
		auto &input_list = list_entry.GetValue();
		has_valid_list = true;
		min_length = MinValue<idx_t>(min_length, input_list.length);
		max_length = MaxValue<idx_t>(max_length, input_list.length);
		if (input_list.offset != child_count) {
			child_contiguous = false;
		}
		append_offsets[i] = start_offset + child_count + input_list.length;
		child_count += input_list.length;
	}
	if (has_valid_list) {
		ListStats::MergeInLengths(*state.append_stats, min_length, max_length, child_count);
	}
	auto &list_child = ListVector::GetChild(vector);
	Vector child_vector(Vector::Ref(list_child));
	if (!child_contiguous) {
		// if the child of the list vector is a non-contiguous vector (i.e. list elements are repeating or have gaps)
		// we first push a selection vector and flatten the child vector to turn it into a contiguous vector
		SelectionVector child_sel(child_count);
		idx_t current_count = 0;
		for (idx_t i = 0; i < count; i++) {
			auto list_entry = input_offsets[i];
			if (!list_entry.IsValid()) {
				continue;
			}
			auto &input_list = list_entry.GetValue();
			for (idx_t list_idx = 0; list_idx < input_list.length; list_idx++) {
				child_sel.set_index(current_count++, input_list.offset + list_idx);
			}
		}
		D_ASSERT(current_count == child_count);
		child_vector.Slice(list_child, child_sel, child_count);
	}

	UnifiedVectorFormat vdata;
	vdata.sel = FlatVector::IncrementalSelectionVector();
	vdata.data = data_ptr_cast(append_offsets.get());
	vdata.physical_type = PhysicalType::UINT64;

	// append the child vector
	if (child_count > 0) {
		child_column->Append(state.child_appends[1], child_vector, child_count);
	}
	// append the list offsets
	ColumnData::AppendData(state, vdata, count);
	// append the validity data
	vdata.validity = append_mask;
	validity->AppendData(state.child_appends[0], vdata, count);
}

void ListColumnData::FinalizeAppend(ColumnDataFinalizeAppendState &finalize_state, ColumnAppendState &state) {
	ColumnData::FinalizeAppend(finalize_state, state);
	validity->FinalizeAppendLocked(finalize_state, state.child_appends[0]);

	ColumnDataFinalizeAppendState child_finalize_state(finalize_state, LogicalTypeId::LIST);
	child_column->FinalizeAppendLocked(child_finalize_state, state.child_appends[1]);
}

void ListColumnData::RevertAppend(row_t new_count) {
	if (!HasImpliedOffsets() || data.GetRootSegment()) {
		ColumnData::RevertAppend(new_count);
	}
	validity->RevertAppend(new_count);
	// revert append in the child column, also when no list is left
	auto column_count = GetMaxEntry();
	auto list_offset = column_count > 0 ? FetchListOffset(column_count - 1) : 0;
	child_column->RevertAppend(UnsafeNumericCast<row_t>(list_offset));
}

void ListColumnData::Update(TransactionData transaction, DuckTableEntry &table_entry, idx_t column_index,
                            Vector &update_vector, row_t *row_ids, idx_t update_count, idx_t row_group_start) {
	throw NotImplementedException("List Update is not supported.");
}

void ListColumnData::UpdateColumn(TransactionData transaction, DuckTableEntry &table_entry,
                                  const vector<column_t> &column_path, Vector &update_vector, row_t *row_ids,
                                  idx_t update_count, idx_t depth, idx_t row_group_start) {
	throw NotImplementedException("List Update Column is not supported");
}

unique_ptr<BaseStatistics> ListColumnData::GetUpdateStatistics() {
	return nullptr;
}
void ListColumnData::FetchRows(TransactionData transaction, ColumnFetchState &state, const StorageIndex &storage_index,
                               const idx_t *offsets, const SelectionVector &sel, idx_t fetch_count, Vector &result,
                               idx_t result_offset) {
	// insert the validity child state
	if (state.child_states.empty()) {
		auto child_state = make_uniq<ColumnFetchState>();
		state.child_states.push_back(std::move(child_state));
	}

	validity->FetchRowsAtSegmentLevel(transaction, *state.child_states[0], offsets, sel, fetch_count, result,
	                                  result_offset);

	auto start_data = make_unsafe_uniq_array<idx_t>(fetch_count);
	auto end_data = make_unsafe_uniq_array<idx_t>(fetch_count);
	idx_t max_row = 0;
	for (idx_t idx = 0; idx < fetch_count; idx++) {
		max_row = MaxValue<idx_t>(max_row, offsets[sel.get_index(idx)]);
	}
	if (HasImpliedOffsets() && max_row < implied_count) {
		// each list starts at the list length times the number of non-NULL lists before it
		auto list_length = implied_list_length.GetIndex();
		ValidityMask list_validity(max_row + 1);
		if (implied_can_have_null) {
			ColumnScanState validity_state(nullptr);
			validity->InitializeScan(validity_state);
			for (idx_t row_idx = 0; row_idx <= max_row; row_idx += STANDARD_VECTOR_SIZE) {
				auto scan_count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, max_row + 1 - row_idx);
				Vector validity_vector(LogicalType::BOOLEAN, scan_count);
				validity->ScanCount(validity_state, validity_vector, scan_count);
				auto &scanned_validity = FlatVector::Validity(validity_vector);
				for (idx_t i = 0; i < scan_count; i++) {
					if (!scanned_validity.RowIsValid(i)) {
						list_validity.SetInvalid(row_idx + i);
					}
				}
			}
		}
		for (idx_t idx = 0; idx < fetch_count; idx++) {
			const auto row_id = offsets[sel.get_index(idx)];
			start_data[idx] = list_length * list_validity.CountValid(row_id);
			end_data[idx] = start_data[idx] + (list_validity.RowIsValid(row_id) ? list_length : 0);
		}
	} else {
		// each list ends at its own offset and starts at the offset of the row before it
		Vector end_offsets(LogicalType::UBIGINT, fetch_count);
		FetchRowsAtSegmentLevel(transaction, state, offsets, sel, fetch_count, end_offsets, 0);
		auto previous_rows = make_unsafe_uniq_array<idx_t>(fetch_count);
		SelectionVector previous_sel(fetch_count);
		idx_t previous_count = 0;
		for (idx_t idx = 0; idx < fetch_count; idx++) {
			const auto row_id = offsets[sel.get_index(idx)];
			if (row_id > 0) {
				previous_rows[idx] = row_id - 1;
				previous_sel.set_index(previous_count++, idx);
			}
		}
		Vector start_offsets(LogicalType::UBIGINT, fetch_count);
		FetchRowsAtSegmentLevel(transaction, state, previous_rows.get(), previous_sel, previous_count, start_offsets,
		                        0);
		auto fetched_end = FlatVector::GetData<uint64_t>(end_offsets);
		auto fetched_start = FlatVector::GetData<uint64_t>(start_offsets);
		idx_t start_idx = 0;
		for (idx_t idx = 0; idx < fetch_count; idx++) {
			const auto row_id = offsets[sel.get_index(idx)];
			start_data[idx] = row_id == 0 ? 0 : fetched_start[start_idx++];
			end_data[idx] = fetched_end[idx];
		}
	}

	auto &validity_mask = FlatVector::ValidityMutable(result);
	auto list_data = FlatVector::GetDataMutable<list_entry_t>(result);
	for (idx_t idx = 0; idx < fetch_count; idx++) {
		auto start_offset = start_data[idx];
		auto end_offset = end_data[idx];
		auto length = GetListLength(start_offset, end_offset, child_column->GetMaxEntry());
		auto result_idx = result_offset + idx;
		auto &list_entry = list_data[result_idx];
		// set the list entry offset to the size of the current list
		list_entry.offset = ListVector::GetListSize(result);
		list_entry.length = length;
		if (!validity_mask.RowIsValid(result_idx)) {
			// the list is NULL! no need to fetch the child
			list_entry.length = 0;
			continue;
		}

		// now we need to read from the child all the elements between [offset...length]
		auto child_scan_count = list_entry.length;
		if (child_scan_count > 0) {
			ColumnScanState child_state(nullptr);
			auto &child_type = ListType::GetChildType(result.GetType());
			Vector child_scan(child_type, child_scan_count);
			// seek the scan towards the specified position and read [length] entries
			child_state.Initialize(state.context, child_type, nullptr);
			child_column->InitializeScanWithOffset(child_state, start_offset);
			D_ASSERT(child_type.InternalType() == PhysicalType::STRUCT ||
			         child_state.offset_in_column + child_scan_count <= child_column->GetMaxEntry());
			child_column->ScanCount(child_state, child_scan, child_scan_count);

			ListVector::Append(result, child_scan, child_scan_count);
		}
	}
}

void ListColumnData::VisitBlockIds(BlockIdVisitor &visitor) const {
	ColumnData::VisitBlockIds(visitor);
	validity->VisitBlockIds(visitor);
	child_column->VisitBlockIds(visitor);
}

const BaseStatistics &ListColumnData::GetChildStats(const ColumnData &child) const {
	if (!RefersToSameObject(child, *child_column)) {
		throw InternalException("ListColumnData::GetChildStats provided column data is not a child of this list");
	}
	auto &stats = GetStatisticsRef();
	return ListStats::GetChildStats(stats);
}

void ListColumnData::SetValidityData(shared_ptr<ValidityColumnData> validity_p) {
	if (validity) {
		throw InternalException("ListColumnData::SetValidityData cannot be used to overwrite existing validity");
	}
	validity_p->SetParent(this);
	this->validity = std::move(validity_p);
}

void ListColumnData::SetChildData(shared_ptr<ColumnData> child_column_p) {
	if (child_column) {
		throw InternalException("ListColumnData::SetChildData cannot be used to overwrite existing data");
	}
	child_column_p->SetParent(this);
	this->child_column = std::move(child_column_p);
}

struct ListColumnCheckpointState : public ColumnCheckpointState {
	ListColumnCheckpointState(const RowGroup &row_group, ColumnData &column_data, Context &context,
	                          PartialBlockManager &partial_block_manager)
	    : ColumnCheckpointState(row_group, column_data, context, partial_block_manager) {
		global_stats = ListStats::CreateEmpty(column_data.type).ToUnique();
	}

	unique_ptr<ColumnCheckpointState> validity_state;
	unique_ptr<ColumnCheckpointState> child_state;
	//! The list length statistics of the column
	ListStatsData length_data = ListStatsData {false, false, false, 0, 0, 0};
	//! The length of every list, if the list offsets are not written
	optional_idx implied_list_length;
	bool implied_can_have_null = true;

public:
	shared_ptr<ColumnData> CreateEmptyColumnData() override {
		return make_shared_ptr<ListColumnData>(original_column.GetBlockManager(), original_column.GetTableInfo(),
		                                       original_column.column_index, original_column.type,
		                                       ColumnDataType::CHECKPOINT_TARGET, nullptr);
	}

	shared_ptr<ColumnData> GetFinalResult() override {
		if (result_column) {
			auto &column_data = result_column->Cast<ListColumnData>();
			auto validity_child = validity_state->GetFinalResult();
			column_data.SetValidityData(shared_ptr_cast<ColumnData, ValidityColumnData>(std::move(validity_child)));
			column_data.SetChildData(child_state->GetFinalResult());
			column_data.implied_list_length = implied_list_length;
			column_data.implied_count = implied_list_length.IsValid() ? original_column.count.load() : 0;
			column_data.implied_can_have_null = implied_can_have_null;
		}
		return ColumnCheckpointState::GetFinalResult();
	}

	unique_ptr<BaseStatistics> GetStatistics() override {
		auto stats = global_stats->Copy();
		stats.Merge(*validity_state->GetStatistics());
		ListStats::SetChildStats(stats, child_state->GetStatistics());
		ListStats::SetLengthData(stats, length_data);
		return stats.ToUnique();
	}

	void FlushSegmentInternal(unique_ptr<ColumnSegment> segment, idx_t segment_size) override {
		// the list length stats are stored with the column and not with the segments of the offsets
		ListStats::SetLengthBounds(segment->GetStatsMutable(), optional_idx(), optional_idx());
		ColumnCheckpointState::FlushSegmentInternal(std::move(segment), segment_size);
	}

	PersistentColumnData ToPersistentData() override {
		auto data = ColumnCheckpointState::ToPersistentData();
		auto extra_data = make_uniq<ListPersistentColumnData>(length_data);
		extra_data->implied_list_length = implied_list_length;
		data.extra_data = std::move(extra_data);
		data.child_columns.push_back(validity_state->ToPersistentData());
		data.child_columns.push_back(child_state->ToPersistentData());
		return data;
	}
};

unique_ptr<ColumnCheckpointState> ListColumnData::CreateCheckpointState(const RowGroup &row_group, Context &context,
                                                                        PartialBlockManager &partial_block_manager) {
	return make_uniq<ListColumnCheckpointState>(row_group, *this, context, partial_block_manager);
}

optional_idx ListColumnData::GetCheckpointListLength(const BaseStatistics &stats) {
	if (count == 0 ||
	    !StorageManager::TargetAtLeastVersion(StorageVersion::V2_1_0, GetStorageManager().GetStorageVersion())) {
		return optional_idx();
	}
	auto min_length = ListStats::MinListLength(stats);
	auto max_length = ListStats::MaxListLength(stats);
	if (!min_length.IsValid() || !max_length.IsValid() || min_length.GetIndex() != max_length.GetIndex()) {
		return optional_idx();
	}
	return min_length;
}

struct ListOffsetBlockDropper : public BlockIdVisitor {
	explicit ListOffsetBlockDropper(BlockManager &manager) : manager(manager) {
	}

	void Visit(block_id_t block_id) override {
		manager.MarkBlockAsModified(block_id);
	}

	BlockManager &manager;
};

unique_ptr<ColumnCheckpointState> ListColumnData::Checkpoint(const RowGroup &row_group,
                                                             ColumnCheckpointInfo &checkpoint_info,
                                                             const BaseStatistics &old_stats) {
	auto list_length = GetCheckpointListLength(old_stats);
	unique_ptr<ColumnCheckpointState> base_state;
	if (list_length.IsValid() && (!data.GetRootSegment() || ColumnData::HasChanges())) {
		// all lists have the same length: the offsets are implied by it and are not written
		base_state =
		    CreateCheckpointState(row_group, checkpoint_info.GetContext(), checkpoint_info.GetPartialBlockManager());
		if (data.GetRootSegment()) {
			// the stored offsets are replaced, so their blocks are no longer required
			ListOffsetBlockDropper dropper(GetStorageManager().GetBlockManager());
			for (auto &segment : data.Segments()) {
				segment.VisitBlockIds(dropper);
			}
			base_state->GetResultColumn();
		}
		auto &list_state = base_state->Cast<ListColumnCheckpointState>();
		list_state.implied_list_length = list_length;
		list_state.implied_can_have_null = old_stats.CanHaveNull();
	} else {
		base_state = ColumnData::Checkpoint(row_group, checkpoint_info, old_stats);
	}
	auto validity_state = validity->Checkpoint(row_group, checkpoint_info, old_stats);
	auto child_state = child_column->Checkpoint(row_group, checkpoint_info, ListStats::GetChildStats(old_stats));

	auto &checkpoint_state = base_state->Cast<ListColumnCheckpointState>();
	checkpoint_state.validity_state = std::move(validity_state);
	checkpoint_state.child_state = std::move(child_state);
	checkpoint_state.length_data = ListStats::GetLengthData(old_stats);
	return base_state;
}

bool ListColumnData::IsPersistent() {
	return ColumnData::IsPersistent() && validity->IsPersistent() && child_column->IsPersistent();
}

bool ListColumnData::HasAnyChanges() const {
	return ColumnData::HasAnyChanges() || validity->HasAnyChanges() || child_column->HasAnyChanges();
}

//! Resolves the statistics of a column from the statistics of its root column
//! A nested column can hold its own statistics (e.g. when it was created by a checkpoint), but those are not updated:
//! the statistics of nested columns are tracked in the statistics of the root column
static optional_ptr<const BaseStatistics> ResolveColumnStatistics(const ColumnData &column) {
	if (!column.HasParent()) {
		return column.GetStatisticsRef();
	}
	auto &parent = column.Parent();
	auto parent_stats = ResolveColumnStatistics(parent);
	if (!parent_stats) {
		return nullptr;
	}
	switch (parent.GetType().id()) {
	case LogicalTypeId::LIST:
	case LogicalTypeId::MAP:
		return ListStats::GetChildStats(*parent_stats);
	case LogicalTypeId::ARRAY:
		return ArrayStats::GetChildStats(*parent_stats);
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE:
	case LogicalTypeId::UNION: {
		auto &struct_data = parent.Cast<StructColumnData>();
		auto child_count = StructType::GetChildCount(parent.GetType());
		for (idx_t i = 0; i < child_count; i++) {
			if (RefersToSameObject(struct_data.GetChildColumn(i), column)) {
				return StructStats::GetChildStats(*parent_stats, i);
			}
		}
		return nullptr;
	}
	default:
		// the statistics of the children of other columns (e.g. VARIANT, GEOMETRY) are not tracked in their parent
		return nullptr;
	}
}

optional_ptr<const BaseStatistics> ListColumnData::GetColumnStatistics() const {
	return ResolveColumnStatistics(*this);
}

bool ListPersistentColumnData::IsSupported(const Serializer &serializer) const {
	return serializer.ShouldSerialize(StorageVersion::V2_1_0);
}

void ListPersistentColumnData::SerializeFields(Serializer &serializer) const {
	serializer.WritePropertyWithDefault(101, "min_list_length", length_data.MinLength());
	serializer.WritePropertyWithDefault(102, "max_list_length", length_data.MaxLength());
	serializer.WritePropertyWithDefault(103, "total_list_length", length_data.TotalLength());
	serializer.WritePropertyWithDefault(104, "implied_list_length", implied_list_length);
}

unique_ptr<ExtraPersistentColumnData> ListPersistentColumnData::Deserialize(Deserializer &deserializer) {
	auto min_length = deserializer.ReadPropertyWithDefault<optional_idx>(101, "min_list_length");
	auto max_length = deserializer.ReadPropertyWithDefault<optional_idx>(102, "max_list_length");
	auto total_length = deserializer.ReadPropertyWithDefault<optional_idx>(103, "total_list_length");
	auto implied_length = deserializer.ReadPropertyWithDefault<optional_idx>(104, "implied_list_length");
	auto result = make_uniq<ListPersistentColumnData>();
	result->implied_list_length = implied_length;
	auto &data = result->length_data;
	data.has_min_list_length = min_length.IsValid();
	data.min_list_length = min_length.IsValid() ? min_length.GetIndex() : 0;
	data.has_max_list_length = max_length.IsValid();
	data.max_list_length = max_length.IsValid() ? max_length.GetIndex() : 0;
	data.has_total_list_length = total_length.IsValid();
	data.total_list_length = total_length.IsValid() ? total_length.GetIndex() : 0;
	return std::move(result);
}

PersistentColumnData ListColumnData::Serialize() {
	const bool implied_offsets = HasImpliedOffsets() && !data.GetRootSegment();
	auto persistent_data = implied_offsets ? PersistentColumnData(type) : ColumnData::Serialize();
	auto column_stats = GetColumnStatistics();
	unique_ptr<ListPersistentColumnData> extra_data;
	if (column_stats) {
		extra_data = make_uniq<ListPersistentColumnData>(ListStats::GetLengthData(*column_stats));
	}
	if (implied_offsets) {
		if (!extra_data) {
			extra_data = make_uniq<ListPersistentColumnData>();
		}
		extra_data->implied_list_length = implied_list_length;
	}
	persistent_data.extra_data = std::move(extra_data);
	persistent_data.child_columns.push_back(validity->Serialize());
	persistent_data.child_columns.push_back(child_column->Serialize());
	return persistent_data;
}

void ListColumnData::InitializeColumn(PersistentColumnData &column_data, BaseStatistics &target_stats) {
	ColumnData::InitializeColumn(column_data, target_stats);
	optional_idx list_length;
	if (column_data.extra_data && column_data.extra_data->GetType() == ExtraPersistentColumnDataType::LIST) {
		auto &list_data = column_data.extra_data->Cast<ListPersistentColumnData>();
		ListStats::SetLengthData(target_stats, list_data.length_data);
		list_length = list_data.implied_list_length;
	} else {
		ListStats::SetLengthBounds(target_stats, optional_idx(), optional_idx());
	}
	validity->InitializeColumn(column_data.child_columns[0], target_stats);
	auto &child_stats = ListStats::GetChildStats(target_stats);
	child_column->InitializeColumn(column_data.child_columns[1], child_stats);
	if (!list_length.IsValid()) {
		return;
	}
	// the offsets are implied by the list length: the validity holds the row count
	if (!column_data.pointers.empty()) {
		throw DataCorruptionException("Corrupted LIST column: implied list offsets cannot be stored");
	}
	auto row_count = validity->count.load();
	auto child_count = child_column->GetMaxEntry();
	auto length = list_length.GetIndex();
	if (length == 0 ? child_count != 0 : (child_count % length != 0 || child_count / length > row_count)) {
		throw DataCorruptionException("Corrupted LIST column: the child row count does not match the list length");
	}
	implied_list_length = list_length;
	implied_count = row_count;
	implied_can_have_null = target_stats.CanHaveNull();
	count = row_count;
}

void ListColumnData::Verify(RowGroup &parent) {
	if (!HasImpliedOffsets() || data.GetRootSegment()) {
		ColumnData::Verify(parent);
	}
}

void ListColumnData::GetColumnSegmentInfo(const QueryContext &context, idx_t row_group_index, vector<idx_t> col_path,
                                          vector<ColumnSegmentInfo> &result,
                                          const ColumnSegmentInfoScanOptions &options) {
	ColumnData::GetColumnSegmentInfo(context, row_group_index, col_path, result, options);
	col_path.push_back(0);
	validity->GetColumnSegmentInfo(context, row_group_index, col_path, result, options);
	col_path.back() = 1;
	child_column->GetColumnSegmentInfo(context, row_group_index, col_path, result, options);
}

} // namespace duckdb
