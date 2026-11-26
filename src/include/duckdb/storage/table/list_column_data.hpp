//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/list_column_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/storage/table/validity_column_data.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"

namespace duckdb {

//! The list length statistics that are persisted with a list column
class ListPersistentColumnData final : public ExtraPersistentColumnData {
public:
	static constexpr auto TYPE = ExtraPersistentColumnDataType::LIST;
	ListPersistentColumnData() : ExtraPersistentColumnData(TYPE) {
	}
	explicit ListPersistentColumnData(const ListStatsData &length_data_p)
	    : ExtraPersistentColumnData(TYPE), length_data(length_data_p) {
	}

	bool IsSupported(const Serializer &serializer) const override;
	void SerializeFields(Serializer &serializer) const;
	static unique_ptr<ExtraPersistentColumnData> Deserialize(Deserializer &deserializer);

	ListStatsData length_data = ListStatsData {false, false, false, 0, 0, 0};
	//! The length of the lists whose offsets are not stored, if the length statistics do not imply it
	optional_idx implied_list_length;
};

//! List column data represents a list
class ListColumnData : public ColumnData {
	friend struct ListColumnCheckpointState;

public:
	ListColumnData(BlockManager &block_manager, DataTableInfo &info, idx_t column_index, LogicalType type,
	               ColumnDataType data_type, optional_ptr<ColumnData> parent);

public:
	void SetDataType(ColumnDataType data_type) override;
	FilterPropagateResult CheckZonemap(ColumnScanState &state, const ScanFilter &filter,
	                                   optional_idx &checked_end) override;

	void InitializePrefetch(PrefetchState &prefetch_state, ColumnScanState &scan_state, idx_t start_row,
	                        idx_t end_row) override;
	void InitializeScan(ColumnScanState &state) override;
	void InitializeScanWithOffset(ColumnScanState &state, idx_t row_idx) override;

	idx_t Scan(TransactionData transaction, idx_t vector_index, ColumnScanState &state, Vector &result,
	           idx_t scan_count) override;
	idx_t ScanCount(ColumnScanState &state, Vector &result, idx_t count, idx_t result_offset = 0) override;

	void Skip(ColumnScanState &state, idx_t count = STANDARD_VECTOR_SIZE) override;

	void InitializeAppend(ColumnAppendState &state) override;
	void Append(ColumnAppendState &state, const Vector &vector, idx_t count) override;
	void FinalizeAppend(ColumnDataFinalizeAppendState &finalize_state, ColumnAppendState &state) override;
	void RevertAppend(row_t new_count) override;
	void FetchRows(TransactionData transaction, ColumnFetchState &state, const StorageIndex &storage_index,
	               const idx_t *offsets, const SelectionVector &sel, idx_t count, Vector &result,
	               idx_t result_offset) override;
	void Update(TransactionData transaction, DuckTableEntry &table_entry, idx_t column_index, Vector &update_vector,
	            row_t *row_ids, idx_t update_count, idx_t row_group_start) override;
	void UpdateColumn(TransactionData transaction, DuckTableEntry &table_entry, const vector<column_t> &column_path,
	                  Vector &update_vector, row_t *row_ids, idx_t update_count, idx_t depth,
	                  idx_t row_group_start) override;
	unique_ptr<BaseStatistics> GetUpdateStatistics() override;

	void VisitBlockIds(BlockIdVisitor &visitor) const override;

	unique_ptr<ColumnCheckpointState> CreateCheckpointState(const RowGroup &row_group, Context &context,
	                                                        PartialBlockManager &partial_block_manager) override;
	unique_ptr<ColumnCheckpointState> Checkpoint(const RowGroup &row_group, ColumnCheckpointInfo &info,
	                                             const BaseStatistics &old_stats) override;

	bool IsPersistent() override;
	bool HasAnyChanges() const override;
	PersistentColumnData Serialize() override;
	void InitializeColumn(PersistentColumnData &column_data, BaseStatistics &target_stats) override;

	void GetColumnSegmentInfo(const QueryContext &context, duckdb::idx_t row_group_index,
	                          vector<duckdb::idx_t> col_path, vector<duckdb::ColumnSegmentInfo> &result,
	                          const ColumnSegmentInfoScanOptions &options) override;

	void SetValidityData(shared_ptr<ValidityColumnData> validity_p);
	//! The statistics of this column, if they can be resolved
	optional_ptr<const BaseStatistics> GetColumnStatistics() const;
	void SetChildData(shared_ptr<ColumnData> child_column_p);

	const BaseStatistics &GetChildStats(const ColumnData &child) const override;
	void Verify(RowGroup &parent) override;

protected:
	//! The child-column of the list
	shared_ptr<ColumnData> child_column;
	//! The validity column data of the list
	shared_ptr<ValidityColumnData> validity;

	//! The lists in [0, implied_count) are not NULL and all have length implied_list_length, so they are laid out
	//! like arrays: their offsets are not stored, and the offset segments hold the offsets of the rows from
	//! implied_count on
	optional_idx implied_list_length;
	idx_t implied_count = 0;

private:
	uint64_t FetchListOffset(idx_t row_idx);
	//! Whether a scan state is at a row with implied offsets (the offset segments are not scanned yet)
	bool ScansImpliedOffsets(const ColumnScanState &state) const;
	//! The row of the list column a scan state is at
	idx_t ScanRow(const ColumnScanState &state) const;
	void InitializeImpliedScan(ColumnScanState &state, idx_t row_idx);
	//! Moves a scan from the rows with implied offsets to the first row with stored offsets
	void StartStoredScan(ColumnScanState &state);
	//! The child row the list at row_idx starts at, for row_idx <= implied_count
	idx_t ImpliedOffset(idx_t row_idx) const;
	//! The list length if the offsets do not have to be stored when checkpointing
	optional_idx GetCheckpointListLength(const BaseStatistics &stats);
};

} // namespace duckdb
