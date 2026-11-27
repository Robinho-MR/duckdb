#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/storage/table/array_column_data.hpp"
#include "duckdb/storage/statistics/array_stats.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/storage/table/column_checkpoint_state.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/storage/table/list_column_data.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"
#include "duckdb/common/vector/list_vector.hpp"

namespace duckdb {

static const LogicalType &FixedSizeChildType(const LogicalType &type) {
	return type.InternalType() == PhysicalType::LIST ? ListType::GetChildType(type) : ArrayType::GetChildType(type);
}

static const BaseStatistics &FixedSizeChildStats(const BaseStatistics &stats) {
	return stats.GetType().InternalType() == PhysicalType::LIST ? ListStats::GetChildStats(stats)
	                                                            : ArrayStats::GetChildStats(stats);
}

static BaseStatistics &FixedSizeChildStats(BaseStatistics &stats) {
	return stats.GetType().InternalType() == PhysicalType::LIST ? ListStats::GetChildStats(stats)
	                                                            : ArrayStats::GetChildStats(stats);
}

//! The list length statistics of lists that all have the same length
static ListStatsData FixedSizeListLengths(idx_t array_size, idx_t count) {
	return ListStatsData {true, true, true, array_size, array_size, array_size * count};
}

ArrayColumnData::ArrayColumnData(BlockManager &block_manager, DataTableInfo &info, idx_t column_index,
                                 LogicalType type_p, ColumnDataType data_type, optional_ptr<ColumnData> parent)
    : ArrayColumnData(block_manager, info, column_index, type_p, ArrayType::GetSize(type_p), data_type, parent) {
}

ArrayColumnData::ArrayColumnData(BlockManager &block_manager, DataTableInfo &info, idx_t column_index,
                                 LogicalType type_p, idx_t array_size_p, ColumnDataType data_type,
                                 optional_ptr<ColumnData> parent)
    : ColumnData(block_manager, info, column_index, std::move(type_p), data_type, parent), array_size(array_size_p) {
	D_ASSERT(type.InternalType() == PhysicalType::ARRAY || type.InternalType() == PhysicalType::LIST);
	if (data_type != ColumnDataType::CHECKPOINT_TARGET) {
		auto &child_type = FixedSizeChildType(type);
		validity = make_shared_ptr<ValidityColumnData>(block_manager, info, 0, *this);
		// the child column, with column index 1 (0 is the validity mask)
		child_column = CreateColumn(block_manager, info, 1, child_type, data_type, this);
	}
}

void ArrayColumnData::SetDataType(ColumnDataType data_type) {
	ColumnData::SetDataType(data_type);
	child_column->SetDataType(data_type);
	validity->SetDataType(data_type);
}

FilterPropagateResult ArrayColumnData::CheckZonemap(ColumnScanState &state, const ScanFilter &filter,
                                                    optional_idx &checked_end) {
	return CheckValidityZonemap(state, filter, checked_end, *validity);
}

void ArrayColumnData::InitializePrefetch(PrefetchState &prefetch_state, ColumnScanState &scan_state, idx_t start_row,
                                         idx_t end_row) {
	ColumnData::InitializePrefetch(prefetch_state, scan_state, start_row, end_row);
	validity->InitializePrefetch(prefetch_state, scan_state.child_states[0], start_row, end_row);
	child_column->InitializePrefetch(prefetch_state, scan_state.child_states[1], start_row * array_size,
	                                 end_row * array_size);
}

void ArrayColumnData::InitializeScan(ColumnScanState &state) {
	// initialize the validity segment
	D_ASSERT(state.child_states.size() == 2);

	state.offset_in_column = 0;
	state.current = nullptr;

	validity->InitializeScan(state.child_states[0]);

	// initialize the child scan
	child_column->InitializeScan(state.child_states[1]);
}

void ArrayColumnData::InitializeScanWithOffset(ColumnScanState &state, idx_t row_idx) {
	D_ASSERT(state.child_states.size() == 2);

	if (row_idx == 0) {
		// Trivial case, no offset
		InitializeScan(state);
		return;
	}

	state.offset_in_column = row_idx;
	state.current = nullptr;

	// initialize the validity segment
	validity->InitializeScanWithOffset(state.child_states[0], row_idx);

	auto child_count = row_idx * array_size;

	D_ASSERT(child_count <= child_column->GetMaxEntry());
	if (child_count < child_column->GetMaxEntry()) {
		const auto child_offset = child_count;
		child_column->InitializeScanWithOffset(state.child_states[1], child_offset);
	}
}

idx_t ArrayColumnData::Scan(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                            idx_t scan_count) {
	return ScanCount(state, result, scan_count);
}

bool ArrayColumnData::IsList() const {
	return type.InternalType() == PhysicalType::LIST;
}

void ArrayColumnData::ScanLists(ColumnScanState &state, Vector &result, idx_t count, idx_t scan_count) {
	// the list at row i holds the child rows [i * array_size, (i + 1) * array_size)
	{
		auto result_data = FlatVector::Writer<list_entry_t>(result, scan_count);
		for (idx_t i = 0; i < scan_count; i++) {
			result_data.WriteValue(list_entry_t(i * array_size, array_size));
		}
	}
	auto child_count = scan_count * array_size;
	ListVector::Reserve(result, child_count);
	if (child_count > 0) {
		child_column->ScanCount(state.child_states[1], ListVector::GetChildMutable(result), child_count);
	}
	ListVector::SetListSize(result, child_count);
}

idx_t ArrayColumnData::ScanCount(ColumnScanState &state, Vector &result, idx_t count, idx_t result_offset) {
	if (IsList()) {
		if (result_offset > 0) {
			throw InternalException("ArrayColumnData::ScanCount not supported for lists with result_offset > 0");
		}
		auto scan_count = validity->ScanCount(state.child_states[0], result, count);
		ScanLists(state, result, count, scan_count);
		return scan_count;
	}
	// Scan validity
	auto scan_count = validity->ScanCount(state.child_states[0], result, count, result_offset);
	// Scan child column
	auto &child_vec = ArrayVector::GetChildMutable(result);
	child_column->ScanCount(state.child_states[1], child_vec, count * array_size, result_offset * array_size);
	return scan_count;
}

void ArrayColumnData::Select(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
                             SelectionVector &sel, idx_t sel_count) {
	bool is_supported =
	    !IsList() && !child_column->type.IsNested() && child_column->type.InternalType() != PhysicalType::VARCHAR;
	if (!is_supported) {
		ColumnData::Select(transaction, vector_index, state, result, sel, sel_count);
		return;
	}
	// the below specialized Select implementation selects only the required arrays, and skips over non-required data
	// note that this implementation is not necessarily faster than the naive implementation of scanning + slicing
	// this optimization is better:
	// (1) the fewer consecutive ranges we are scanning
	// (2) the larger the arrays are
	// below we try to select the optimal variant

	// first count the number of consecutive requests we must make
	idx_t consecutive_ranges = 0;
	for (idx_t i = 0; i < sel_count; i++) {
		idx_t start_idx = sel.get_index(i);
		idx_t end_idx = start_idx + 1;
		for (; i + 1 < sel_count; i++) {
			auto next_idx = sel.get_index(i + 1);
			if (next_idx > end_idx) {
				// not consecutive - break
				break;
			}
			end_idx = next_idx + 1;
		}
		consecutive_ranges++;
	}

	auto target_count = GetVectorCount(vector_index);

	// experimentally, we want to allow around one consecutive range every ~2 array size
	// for array size = 10, we allow 5 consecutive ranges
	// for array size = 100, we allow 50 consecutive ranges
	auto allowed_ranges = array_size / 2;
	if (allowed_ranges < consecutive_ranges) {
		// fallback to select + filter
		ColumnData::Select(transaction, vector_index, state, result, sel, sel_count);
		return;
	}

	idx_t current_offset = 0;
	idx_t current_position = 0;
	auto &child_vec = ArrayVector::GetChildMutable(result);
	for (idx_t i = 0; i < sel_count; i++) {
		idx_t start_idx = sel.get_index(i);
		idx_t end_idx = start_idx + 1;
		for (; i + 1 < sel_count; i++) {
			auto next_idx = sel.get_index(i + 1);
			if (next_idx > end_idx) {
				// not consecutive - break
				break;
			}
			end_idx = next_idx + 1;
		}
		if (start_idx > current_position) {
			// skip forward
			idx_t skip_amount = start_idx - current_position;
			validity->Skip(state.child_states[0], skip_amount);
			child_column->Skip(state.child_states[1], skip_amount * array_size);
		}
		// scan into the result array
		idx_t scan_count = end_idx - start_idx;
		validity->ScanCount(state.child_states[0], result, scan_count, current_offset);
		child_column->ScanCount(state.child_states[1], child_vec, scan_count * array_size, current_offset * array_size);
		// move the current position forward
		current_offset += scan_count;
		current_position = end_idx;
	}
	// if there is any remaining at the end - skip any trailing rows
	if (current_position < target_count) {
		idx_t skip_amount = target_count - current_position;
		validity->Skip(state.child_states[0], skip_amount);
		child_column->Skip(state.child_states[1], skip_amount * array_size);
	}
}

void ArrayColumnData::Skip(ColumnScanState &state, idx_t count) {
	// Skip validity
	validity->Skip(state.child_states[0], count);
	// Skip child column
	child_column->Skip(state.child_states[1], count * array_size);
}

void ArrayColumnData::InitializeAppend(ColumnAppendState &state) {
	if (IsList()) {
		throw InternalException("Lists that are stored like an array cannot be appended to");
	}
	ColumnAppendState validity_append;
	validity_append.transient = state.transient;
	validity->InitializeAppend(validity_append);
	state.child_appends.push_back(std::move(validity_append));

	ColumnAppendState child_append;
	child_append.transient = state.transient;
	child_column->InitializeAppend(child_append);
	state.child_appends.push_back(std::move(child_append));
}

void ArrayColumnData::Append(ColumnAppendState &state, const Vector &vector, idx_t count) {
	if (vector.GetVectorType() != VectorType::FLAT_VECTOR) {
		Vector append_vector(Vector::Ref(vector));
		append_vector.Flatten();
		Append(state, append_vector, count);
		return;
	}

	// Append validity
	validity->Append(state.child_appends[0], vector, count);
	// Append child column
	const auto &child_vec = ArrayVector::GetChild(vector);
	child_column->Append(state.child_appends[1], child_vec, count * array_size);

	this->count += count;
}

void ArrayColumnData::FinalizeAppend(ColumnDataFinalizeAppendState &finalize_state, ColumnAppendState &state) {
	validity->FinalizeAppendLocked(finalize_state, state.child_appends[0]);

	ColumnDataFinalizeAppendState child_finalize_state(finalize_state, LogicalTypeId::ARRAY);
	child_column->FinalizeAppendLocked(child_finalize_state, state.child_appends[1]);
}

void ArrayColumnData::RevertAppend(row_t new_count) {
	// Revert validity
	validity->RevertAppend(new_count);
	// Revert child column
	child_column->RevertAppend(new_count * UnsafeNumericCast<row_t>(array_size));

	this->count = UnsafeNumericCast<idx_t>(new_count);
}

void ArrayColumnData::Update(TransactionData transaction, DuckTableEntry &table_entry, idx_t column_index,
                             Vector &update_vector, row_t *row_ids, idx_t update_count, idx_t row_group_start) {
	throw NotImplementedException("Array Update is not supported.");
}

void ArrayColumnData::UpdateColumn(TransactionData transaction, DuckTableEntry &table_entry,
                                   const vector<column_t> &column_path, Vector &update_vector, row_t *row_ids,
                                   idx_t update_count, idx_t depth, idx_t row_group_start) {
	throw NotImplementedException("Array Update Column is not supported");
}

unique_ptr<BaseStatistics> ArrayColumnData::GetUpdateStatistics() {
	return nullptr;
}

void ArrayColumnData::FetchRows(TransactionData transaction, ColumnFetchState &state, const StorageIndex &storage_index,
                                const idx_t *offsets, const SelectionVector &sel, idx_t fetch_count, Vector &result,
                                idx_t result_offset) {
	// Create state for validity & child column
	if (state.child_states.empty()) {
		state.child_states.push_back(make_uniq<ColumnFetchState>());
	}

	// Fetch validity
	validity->FetchRowsAtSegmentLevel(transaction, *state.child_states[0], offsets, sel, fetch_count, result,
	                                  result_offset);

	if (IsList()) {
		FetchLists(state, offsets, sel, fetch_count, result, result_offset);
		return;
	}
	// Fetch child column
	auto &child_vec = ArrayVector::GetChildMutable(result);
	auto &child_type = ArrayType::GetChildType(type);

	for (idx_t idx = 0; idx < fetch_count; idx++) {
		// We need to fetch between [row_id * array_size, (row_id + 1) * array_size)
		ColumnScanState child_state(nullptr);
		child_state.Initialize(state.context, child_type, nullptr);

		const auto row_id = offsets[sel.get_index(idx)];
		const auto child_offset = row_id * array_size;

		child_column->InitializeScanWithOffset(child_state, child_offset);
		Vector child_scan(child_type, array_size);
		child_column->ScanCount(child_state, child_scan, array_size);
		VectorOperations::Copy(child_scan, child_vec, array_size, 0, (result_offset + idx) * array_size);
	}
}

void ArrayColumnData::FetchLists(ColumnFetchState &state, const idx_t *offsets, const SelectionVector &sel,
                                 idx_t fetch_count, Vector &result, idx_t result_offset) {
	auto &child_type = ListType::GetChildType(type);
	auto &list_validity = FlatVector::Validity(result);
	auto list_data = FlatVector::GetDataMutable<list_entry_t>(result);
	for (idx_t idx = 0; idx < fetch_count; idx++) {
		auto result_idx = result_offset + idx;
		auto &list_entry = list_data[result_idx];
		list_entry.offset = ListVector::GetListSize(result);
		list_entry.length = list_validity.RowIsValid(result_idx) ? array_size : 0;
		if (list_entry.length == 0) {
			continue;
		}
		// the list holds the child rows [row_id * array_size, (row_id + 1) * array_size)
		ColumnScanState child_state(nullptr);
		child_state.Initialize(state.context, child_type, nullptr);
		const auto row_id = offsets[sel.get_index(idx)];
		child_column->InitializeScanWithOffset(child_state, row_id * array_size);
		Vector child_scan(child_type, array_size);
		child_column->ScanCount(child_state, child_scan, array_size);
		ListVector::Append(result, child_scan, array_size);
	}
}

void ArrayColumnData::VisitBlockIds(BlockIdVisitor &visitor) const {
	validity->VisitBlockIds(visitor);
	child_column->VisitBlockIds(visitor);
}

const BaseStatistics &ArrayColumnData::GetChildStats(const ColumnData &child) const {
	if (!RefersToSameObject(child, *child_column)) {
		throw InternalException("ArrayColumnData::GetChildStats provided column data is not a child of this array");
	}
	auto &stats = GetStatisticsRef();
	return FixedSizeChildStats(stats);
}

void ArrayColumnData::SetValidityData(shared_ptr<ValidityColumnData> validity_p) {
	if (validity) {
		throw InternalException("ArrayColumnData::SetValidityData cannot be used to overwrite existing validity");
	}
	validity_p->SetParent(this);
	this->validity = std::move(validity_p);
}

void ArrayColumnData::SetChildData(shared_ptr<ColumnData> child_column_p) {
	if (child_column) {
		throw InternalException("ArrayColumnData::SetChildData cannot be used to overwrite existing data");
	}
	child_column_p->SetParent(this);
	this->child_column = std::move(child_column_p);
}

struct ArrayColumnCheckpointState : public ColumnCheckpointState {
	ArrayColumnCheckpointState(const RowGroup &row_group, ColumnData &column_data, idx_t array_size_p, Context &context,
	                           PartialBlockManager &partial_block_manager)
	    : ColumnCheckpointState(row_group, column_data, context, partial_block_manager), array_size(array_size_p) {
		global_stats = IsList() ? ListStats::CreateEmpty(column_data.type).ToUnique()
		                        : ArrayStats::CreateEmpty(column_data.type).ToUnique();
	}

	unique_ptr<ColumnCheckpointState> validity_state;
	unique_ptr<ColumnCheckpointState> child_state;
	idx_t array_size;

	bool IsList() const {
		return original_column.type.InternalType() == PhysicalType::LIST;
	}

public:
	shared_ptr<ColumnData> CreateEmptyColumnData() override {
		return make_shared_ptr<ArrayColumnData>(original_column.GetBlockManager(), original_column.GetTableInfo(),
		                                        original_column.column_index, original_column.type, array_size,
		                                        ColumnDataType::CHECKPOINT_TARGET, nullptr);
	}

	shared_ptr<ColumnData> GetFinalResult() override {
		if (!result_column) {
			result_column = CreateEmptyColumnData();
		}
		auto &column_data = result_column->Cast<ArrayColumnData>();
		auto validity_child = validity_state->GetFinalResult();
		column_data.SetValidityData(shared_ptr_cast<ColumnData, ValidityColumnData>(std::move(validity_child)));
		column_data.SetChildData(child_state->GetFinalResult());
		return ColumnCheckpointState::GetFinalResult();
	}

	unique_ptr<BaseStatistics> GetStatistics() override {
		auto stats = global_stats->Copy();
		stats.Merge(*validity_state->GetStatistics());
		if (IsList()) {
			ListStats::SetChildStats(stats, child_state->GetStatistics());
			ListStats::SetLengthData(stats, FixedSizeListLengths(array_size, original_column.count.load()));
		} else {
			ArrayStats::SetChildStats(stats, child_state->GetStatistics());
		}
		return stats.ToUnique();
	}

	PersistentColumnData ToPersistentData() override {
		PersistentColumnData data(original_column.type);
		if (IsList()) {
			data.extra_data = make_uniq<ListPersistentColumnData>(
			    FixedSizeListLengths(array_size, original_column.count.load()), array_size);
		}
		data.child_columns.push_back(validity_state->ToPersistentData());
		data.child_columns.push_back(child_state->ToPersistentData());
		return data;
	}
};

unique_ptr<ColumnCheckpointState> ArrayColumnData::CreateCheckpointState(const RowGroup &row_group, Context &context,
                                                                         PartialBlockManager &partial_block_manager) {
	return make_uniq<ArrayColumnCheckpointState>(row_group, *this, array_size, context, partial_block_manager);
}

unique_ptr<ColumnCheckpointState> ArrayColumnData::Checkpoint(const RowGroup &row_group,
                                                              ColumnCheckpointInfo &checkpoint_info,
                                                              const BaseStatistics &old_stats) {
	return CheckpointFixedSize(*this, *validity, *child_column, array_size, row_group, checkpoint_info, old_stats);
}

unique_ptr<ColumnCheckpointState> ArrayColumnData::CheckpointFixedSize(ColumnData &column, ColumnData &validity,
                                                                       ColumnData &child, idx_t array_size,
                                                                       const RowGroup &row_group,
                                                                       ColumnCheckpointInfo &checkpoint_info,
                                                                       const BaseStatistics &old_stats) {
	auto &partial_block_manager = checkpoint_info.GetPartialBlockManager();
	auto &context = checkpoint_info.GetContext();
	auto checkpoint_state =
	    make_uniq<ArrayColumnCheckpointState>(row_group, column, array_size, context, partial_block_manager);
	checkpoint_state->validity_state = validity.Checkpoint(row_group, checkpoint_info, old_stats);
	checkpoint_state->child_state = child.Checkpoint(row_group, checkpoint_info, FixedSizeChildStats(old_stats));
	return std::move(checkpoint_state);
}

bool ArrayColumnData::IsPersistent() {
	return validity->IsPersistent() && child_column->IsPersistent();
}

bool ArrayColumnData::HasAnyChanges() const {
	return child_column->HasAnyChanges() || validity->HasAnyChanges();
}

PersistentColumnData ArrayColumnData::Serialize() {
	PersistentColumnData persistent_data(type);
	if (IsList()) {
		persistent_data.extra_data =
		    make_uniq<ListPersistentColumnData>(FixedSizeListLengths(array_size, count), array_size);
	}
	persistent_data.child_columns.push_back(validity->Serialize());
	persistent_data.child_columns.push_back(child_column->Serialize());
	return persistent_data;
}

void ArrayColumnData::InitializeColumn(PersistentColumnData &column_data, BaseStatistics &target_stats) {
	D_ASSERT(column_data.pointers.empty());
	validity->InitializeColumn(column_data.child_columns[0], target_stats);
	auto &child_stats = FixedSizeChildStats(target_stats);
	InitializeChildColumn(child_column, column_data.child_columns[1], child_stats);
	this->count = validity->count.load();
	if (IsList()) {
		ListStats::SetLengthData(target_stats, FixedSizeListLengths(array_size, count));
		if (child_column->GetMaxEntry() != array_size * count) {
			throw DataCorruptionException("Corrupted LIST column: the child row count does not match the list length");
		}
	}
}

void ArrayColumnData::GetColumnSegmentInfo(const QueryContext &context, idx_t row_group_index, vector<idx_t> col_path,
                                           vector<ColumnSegmentInfo> &result,
                                           const ColumnSegmentInfoScanOptions &options) {
	col_path.push_back(0);
	validity->GetColumnSegmentInfo(context, row_group_index, col_path, result, options);
	col_path.back() = 1;
	child_column->GetColumnSegmentInfo(context, row_group_index, col_path, result, options);
}

void ArrayColumnData::Verify(RowGroup &parent) {
#ifdef DEBUG
	if (!IsList()) {
		// lists have no offset segments when they are stored like an array
		ColumnData::Verify(parent);
	}
	validity->Verify(parent);
	child_column->Verify(parent);
#endif
}

} // namespace duckdb
