#include "duckdb/storage/table/legacy_union_column_data.hpp"

#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/union_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/table/column_checkpoint_state.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace duckdb {

LegacyUnionColumnData::LegacyUnionColumnData(BlockManager &block_manager, DataTableInfo &info, idx_t column_index,
                                             LogicalType type_p, ColumnDataType data_type,
                                             optional_ptr<ColumnData> parent)
    : ColumnData(block_manager, info, column_index, std::move(type_p), data_type, parent) {
	D_ASSERT(UnionType::ContainsLegacyLayout(type));
	if (data_type != ColumnDataType::CHECKPOINT_TARGET) {
		base_column =
		    CreateColumn(block_manager, info, column_index, UnionType::GetLegacyStorageType(type), data_type, this);
	}
}

void LegacyUnionColumnData::ThrowReadOnly() const {
	throw NotImplementedException(
	    "Cannot write to a column of type %s: the column was written by an older version of DuckDB, which stored "
	    "UNIONs in a layout that is read-only. Copy the table (e.g. CREATE TABLE ... AS SELECT) to convert it.",
	    type.ToString());
}

//----------------------------------------------------------------------------------------------------------------------
// Scan
//----------------------------------------------------------------------------------------------------------------------
void LegacyUnionColumnData::InitializePrefetch(PrefetchState &prefetch_state, ColumnScanState &scan_state,
                                               idx_t start_row, idx_t end_row) {
	base_column->InitializePrefetch(prefetch_state, scan_state, start_row, end_row);
}

void LegacyUnionColumnData::InitializeChildScanStates(ColumnScanState &state) {
	// the scan state follows the layout of the base column
	state.child_states.clear();
	state.Initialize(state.context, base_column->type, state.scan_options);
}

void LegacyUnionColumnData::InitializeScan(ColumnScanState &state) {
	InitializeChildScanStates(state);
	base_column->InitializeScan(state);
}

void LegacyUnionColumnData::InitializeScanWithOffset(ColumnScanState &state, idx_t row_idx) {
	InitializeChildScanStates(state);
	base_column->InitializeScanWithOffset(state, row_idx);
}

void LegacyUnionColumnData::ConvertToResult(Vector &legacy, Vector &result, idx_t count, idx_t result_offset) {
	if (result_offset == 0) {
		UnionVector::FromLegacyStorage(legacy, result, count);
		FlatVector::SetSize(result, count_t(count));
		return;
	}
	Vector converted(result.GetType(), count);
	UnionVector::FromLegacyStorage(legacy, converted, count);
	FlatVector::SetSize(converted, count_t(count));
	VectorOperations::Copy(converted, result, count, 0, result_offset);
}

idx_t LegacyUnionColumnData::Scan(TransactionData transaction, idx_t vector_index, ColumnScanState &state,
                                  Vector &result, idx_t target_count) {
	Vector legacy(base_column->type, target_count);
	auto scan_count = base_column->Scan(transaction, vector_index, state, legacy, target_count);
	ConvertToResult(legacy, result, scan_count, 0);
	return scan_count;
}

idx_t LegacyUnionColumnData::ScanCount(ColumnScanState &state, Vector &result, idx_t target_count,
                                       idx_t result_offset) {
	Vector legacy(base_column->type, target_count);
	auto scan_count = base_column->ScanCount(state, legacy, target_count, 0);
	ConvertToResult(legacy, result, scan_count, result_offset);
	return scan_count;
}

void LegacyUnionColumnData::Skip(ColumnScanState &state, idx_t skip_count) {
	base_column->Skip(state, skip_count);
}

void LegacyUnionColumnData::FetchRows(TransactionData transaction, ColumnFetchState &state,
                                      const StorageIndex &storage_index, const idx_t *offsets,
                                      const SelectionVector &sel, idx_t fetch_count, Vector &result,
                                      idx_t result_offset) {
	Vector legacy(base_column->type, MaxValue<idx_t>(fetch_count, 1));
	base_column->FetchRows(transaction, state, storage_index, offsets, sel, fetch_count, legacy, 0);
	ConvertToResult(legacy, result, fetch_count, result_offset);
}

//----------------------------------------------------------------------------------------------------------------------
// Writes - not supported
//----------------------------------------------------------------------------------------------------------------------
void LegacyUnionColumnData::InitializeAppend(ColumnAppendState &state) {
	base_column->InitializeAppend(state);
}

void LegacyUnionColumnData::Append(ColumnAppendState &state, const Vector &vector, idx_t append_count) {
	ThrowReadOnly();
}

void LegacyUnionColumnData::FinalizeAppend(ColumnDataFinalizeAppendState &finalize_state, ColumnAppendState &state) {
	base_column->FinalizeAppend(finalize_state, state);
}

void LegacyUnionColumnData::RevertAppend(row_t new_count) {
	base_column->RevertAppend(new_count);
	count = UnsafeNumericCast<idx_t>(new_count);
}

void LegacyUnionColumnData::Update(TransactionData transaction, DuckTableEntry &table_entry, idx_t column_index,
                                   Vector &update_vector, row_t *row_ids, idx_t update_count, idx_t row_group_start) {
	ThrowReadOnly();
}

void LegacyUnionColumnData::UpdateColumn(TransactionData transaction, DuckTableEntry &table_entry,
                                         const vector<column_t> &column_path, Vector &update_vector, row_t *row_ids,
                                         idx_t update_count, idx_t depth, idx_t row_group_start) {
	ThrowReadOnly();
}

unique_ptr<BaseStatistics> LegacyUnionColumnData::GetUpdateStatistics() {
	return nullptr;
}

//----------------------------------------------------------------------------------------------------------------------
// Checkpoint
//----------------------------------------------------------------------------------------------------------------------
class LegacyUnionColumnCheckpointState final : public ColumnCheckpointState {
public:
	LegacyUnionColumnCheckpointState(const RowGroup &row_group, ColumnData &column_data, Context &context,
	                                 PartialBlockManager &partial_block_manager)
	    : ColumnCheckpointState(row_group, column_data, context, partial_block_manager) {
		// statistics are not tracked for the legacy layout
		global_stats = BaseStatistics::CreateUnknown(column_data.type).ToUnique();
	}

	//! Kept alive for as long as the checkpoint state
	shared_ptr<ColumnData> inner_column;
	unique_ptr<ColumnCheckpointState> inner_column_state;

	shared_ptr<ColumnData> CreateEmptyColumnData() override {
		return make_shared_ptr<LegacyUnionColumnData>(original_column.GetBlockManager(), original_column.GetTableInfo(),
		                                              original_column.column_index, original_column.type,
		                                              ColumnDataType::CHECKPOINT_TARGET, nullptr);
	}

	shared_ptr<ColumnData> GetFinalResult() override {
		if (!result_column) {
			result_column = CreateEmptyColumnData();
		}
		auto &column_data = result_column->Cast<LegacyUnionColumnData>();
		auto new_inner = inner_column_state->GetFinalResult();
		new_inner->SetParent(column_data);
		column_data.base_column = std::move(new_inner);
		return ColumnCheckpointState::GetFinalResult();
	}

	unique_ptr<BaseStatistics> GetStatistics() override {
		D_ASSERT(global_stats);
		return std::move(global_stats);
	}

	PersistentColumnData ToPersistentData() override {
		return inner_column_state->ToPersistentData();
	}
};

unique_ptr<ColumnCheckpointState>
LegacyUnionColumnData::CreateCheckpointState(const RowGroup &row_group, Context &context,
                                             PartialBlockManager &partial_block_manager) {
	return make_uniq<LegacyUnionColumnCheckpointState>(row_group, *this, context, partial_block_manager);
}

unique_ptr<ColumnCheckpointState> LegacyUnionColumnData::Checkpoint(const RowGroup &row_group,
                                                                    ColumnCheckpointInfo &info,
                                                                    const BaseStatistics &old_stats) {
	if (base_column->HasAnyChanges()) {
		ThrowReadOnly();
	}
	auto checkpoint_state =
	    make_uniq<LegacyUnionColumnCheckpointState>(row_group, *this, info.GetContext(), info.GetPartialBlockManager());
	auto base_stats = BaseStatistics::CreateUnknown(base_column->type);
	checkpoint_state->inner_column = base_column;
	checkpoint_state->inner_column_state = base_column->Checkpoint(row_group, info, base_stats);
	return std::move(checkpoint_state);
}

//----------------------------------------------------------------------------------------------------------------------
// Persistence
//----------------------------------------------------------------------------------------------------------------------
bool LegacyUnionColumnData::IsPersistent() {
	return base_column->IsPersistent();
}

bool LegacyUnionColumnData::HasAnyChanges() const {
	return base_column->HasAnyChanges();
}

PersistentColumnData LegacyUnionColumnData::Serialize() {
	return base_column->Serialize();
}

void LegacyUnionColumnData::InitializeColumn(PersistentColumnData &column_data, BaseStatistics &target_stats) {
	auto base_stats = BaseStatistics::CreateEmpty(base_column->type);
	base_column->InitializeColumn(column_data, base_stats);
	count = base_column->count.load();
	// the statistics of the legacy layout don't apply to the VARIANT-backed UNION
	target_stats.Copy(BaseStatistics::CreateUnknown(type));
}

//----------------------------------------------------------------------------------------------------------------------
// Misc
//----------------------------------------------------------------------------------------------------------------------
idx_t LegacyUnionColumnData::GetMaxEntry() {
	return base_column->GetMaxEntry();
}

void LegacyUnionColumnData::GetColumnSegmentInfo(const QueryContext &context, idx_t row_group_index,
                                                 vector<idx_t> col_path, vector<ColumnSegmentInfo> &result,
                                                 const ColumnSegmentInfoScanOptions &options) {
	base_column->GetColumnSegmentInfo(context, row_group_index, std::move(col_path), result, options);
}

void LegacyUnionColumnData::Verify(RowGroup &parent) {
	base_column->Verify(parent);
}

void LegacyUnionColumnData::VisitBlockIds(BlockIdVisitor &visitor) const {
	base_column->VisitBlockIds(visitor);
}

} // namespace duckdb
