#include "duckdb/common/multi_file/table_function_row_id_filter.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/query_result.hpp"

namespace duckdb {

static const char *ROW_ID_QUERY_OPTION = "row_id_query";

bool TableFunctionRowIdFilter::HasRowIdQuery(const OpenFileInfo &file) {
	if (!file.extended_info) {
		return false;
	}
	auto entry = file.extended_info->options.find(ROW_ID_QUERY_OPTION);
	return entry != file.extended_info->options.end() && !entry->second.IsNull();
}

unique_ptr<DeleteFilter> TableFunctionRowIdFilter::TryCreate(ClientContext &context, BaseFileReader &reader) {
	if (!HasRowIdQuery(reader.file)) {
		return nullptr;
	}
	auto row_count = reader.TryGetRowCount(context);
	if (!row_count.IsValid()) {
		throw NotImplementedException("The row id query of file %s needs the number of rows of the file",
		                              reader.file.path);
	}
	auto result = make_uniq<TableFunctionRowIdFilter>();
	result->row_count = row_count.GetIndex();
	result->scan_rows.Initialize(result->row_count);
	result->scan_rows.SetAllInvalid(result->row_count);
	auto query = StringValue::Get(reader.file.extended_info->options.at(ROW_ID_QUERY_OPTION));
	result->ReadRowIds(context, query, reader.file.path);
	return std::move(result);
}

void TableFunctionRowIdFilter::ReadRowIds(ClientContext &context, const string &query, const string &path) {
	// run the query on a connection of its own - this runs while the scan holds the lock of its context
	Connection connection(DatabaseInstance::GetDatabase(context));
	auto view = connection.Query("CREATE TEMP VIEW __file_row_ids AS SELECT range::BIGINT AS row_id FROM range(" +
	                             to_string(row_count) + ")");
	if (view->HasError()) {
		view->GetErrorObject().Throw("Failed to read the row ids of file " + path + ": ");
	}
	auto result = connection.Query("SELECT * FROM (" + query + ")");
	if (result->HasError()) {
		result->GetErrorObject().Throw("Failed to read the row ids of file " + path + ": ");
	}
	optional_idx row_id_column;
	optional_idx row_value_column;
	auto &names = result->GetNames();
	for (idx_t i = 0; i < names.size(); i++) {
		if (names[i].GetIdentifierName() == "row_id") {
			row_id_column = i;
		} else if (names[i].GetIdentifierName() == "row_value") {
			row_value_column = i;
		}
	}
	if (!row_id_column.IsValid()) {
		throw InvalidInputException("The row id query of file %s needs a \"row_id\" column", path);
	}
	has_row_values = row_value_column.IsValid();
	if (has_row_values) {
		row_values.resize(row_count);
		row_value_valid.Initialize(row_count);
		row_value_valid.SetAllInvalid(row_count);
	}
	while (true) {
		auto chunk = result->Fetch();
		if (!chunk || chunk->size() == 0) {
			break;
		}
		Vector row_ids(LogicalType::UBIGINT, chunk->size());
		VectorOperations::DefaultCast(chunk->data[row_id_column.GetIndex()], row_ids, chunk->size());
		Vector values(LogicalType::BIGINT, chunk->size());
		if (has_row_values) {
			VectorOperations::DefaultCast(chunk->data[row_value_column.GetIndex()], values, chunk->size());
		}
		auto value_entries = values.Values<int64_t>();
		for (auto entry : row_ids.Values<uint64_t>()) {
			if (!entry.IsValid() || entry.GetValue() >= row_count) {
				continue;
			}
			scan_rows.SetValid(entry.GetValue());
			if (has_row_values && value_entries[entry.GetIndex()].IsValid()) {
				row_values[entry.GetValue()] = value_entries[entry.GetIndex()].GetValue();
				row_value_valid.SetValid(entry.GetValue());
			}
		}
	}
}

void TableFunctionRowIdFilter::GatherRowValues(const Vector &file_row_numbers, Vector &result, idx_t count) const {
	result.Flatten();
	auto positions = file_row_numbers.Values<int64_t>();
	auto writer = FlatVector::Writer<int64_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		auto position = positions[i];
		if (!position.IsValid() || position.GetValue() < 0 || NumericCast<idx_t>(position.GetValue()) >= row_count ||
		    !row_value_valid.RowIsValid(NumericCast<idx_t>(position.GetValue()))) {
			writer.WriteNull();
			continue;
		}
		writer.WriteValue(row_values[NumericCast<idx_t>(position.GetValue())]);
	}
}

idx_t TableFunctionRowIdFilter::Filter(row_t start_row_index, idx_t count, SelectionVector &result_sel) {
	auto start = NumericCast<idx_t>(start_row_index);
	result_sel.Initialize(STANDARD_VECTOR_SIZE);
	idx_t result_count = 0;
	for (idx_t i = 0; i < count && start + i < row_count; i++) {
		if (scan_rows.RowIsValid(start + i)) {
			result_sel.set_index(result_count++, i);
		}
	}
	return result_count;
}

} // namespace duckdb
