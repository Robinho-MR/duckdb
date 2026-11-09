#include "duckdb/transaction/transaction_context.hpp"
#include "duckdb/logging/log_manager.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/main/database_manager.hpp"

namespace duckdb {

TransactionContext::TransactionContext(ClientContext &context)
    : context(context), auto_commit(true), invalidation_policy(TransactionInvalidationPolicy::STANDARD_POLICY),
      auto_rollback(false), current_transaction(nullptr) {
}

bool TransactionContext::HasActiveTransaction() const {
	return current_transaction && !current_transaction->IsFinished();
}

bool TransactionContext::IsJoinedTransaction() const {
	return current_transaction && &current_transaction->context != &context;
}

TransactionContext::~TransactionContext() {
	if (IsJoinedTransaction()) {
		// the transaction belongs to the connection that created it
		current_transaction.reset();
		return;
	}
	if (HasActiveTransaction()) {
		try {
			Rollback(nullptr);
		} catch (std::exception &ex) {
			ErrorData data(ex);
			try {
				DUCKDB_LOG_ERROR(context, "TransactionContext::~TransactionContext()\t\t" + data.Message());
			} catch (...) { // NOLINT
			}
		} catch (...) { // NOLINT
		}
	}
}

void TransactionContext::BeginTransaction() {
	if (current_transaction) {
		throw TransactionException("cannot start a transaction within a transaction");
	}
	auto start_timestamp = Timestamp::GetCurrentTimestamp();
	auto global_transaction_id = context.db->GetDatabaseManager().GetNewTransactionNumber();
	current_transaction = make_shared_ptr<MetaTransaction>(context, start_timestamp, global_transaction_id);

	// Notify any registered state of transaction begin
	for (auto &state : context.GetContext().GetRegisteredState().States()) {
		state->TransactionBegin(*current_transaction, context);
	}
}

void TransactionContext::SetInvalidationPolicy(TransactionInvalidationPolicy new_invalidation_policy) {
	if (new_invalidation_policy == TransactionInvalidationPolicy::STANDARD_POLICY) {
		// if no policy is specified explicitly use the default one from the settings
		new_invalidation_policy = Settings::Get<DefaultTransactionInvalidationPolicySetting>(context);
	}
	invalidation_policy = new_invalidation_policy;
}

void TransactionContext::SetAutocheckpointError(ErrorData error) {
	autocheckpoint_error = std::move(error);
}

void TransactionContext::Commit() {
	if (!current_transaction) {
		throw TransactionException("failed to commit: no transaction active");
	}
	autocheckpoint_error = ErrorData();
	EndSharedTransaction();
	auto transaction = std::move(current_transaction);
	ClearTransaction();
	auto error = transaction->Commit();
	// Notify any registered state of transaction commit
	if (error.HasError()) {
		for (auto const &s : context.GetContext().GetRegisteredState().States()) {
			s->TransactionRollback(*transaction, context, error);
		}
		if (Exception::InvalidatesDatabase(error.Type()) || error.Type() == ExceptionType::INTERNAL) {
			// throw fatal / internal exceptions directly
			error.Throw();
		}
		throw TransactionException("Failed to commit: %s", error.RawMessage());
	}
	for (auto &state : context.GetContext().GetRegisteredState().States()) {
		state->TransactionCommit(*transaction, context);
	}
	transaction->Finalize();
	if (autocheckpoint_error.HasError()) {
		auto err = std::move(autocheckpoint_error);
		autocheckpoint_error = ErrorData();
		err.Throw();
	}
}

void TransactionContext::SetAutoCommit(bool value) {
	auto_commit = value;
	if (!auto_commit && !current_transaction) {
		BeginTransaction();
	}
}

void TransactionContext::SetReadOnly() {
	current_transaction->SetReadOnly();
}

void TransactionContext::Rollback(optional_ptr<ErrorData> error) {
	if (!current_transaction) {
		throw TransactionException("failed to rollback: no transaction active");
	}
	EndSharedTransaction();
	auto transaction = std::move(current_transaction);
	ClearTransaction();

	ErrorData rollback_error;
	try {
		transaction->Rollback();
	} catch (std::exception &ex) {
		rollback_error = ErrorData(ex);
	}
	// Notify any registered state of transaction rollback - all of them, also when one of them fails
	for (auto const &s : context.GetContext().GetRegisteredState().States()) {
		try {
			s->TransactionRollback(*transaction, context, error);
		} catch (std::exception &ex) {
			if (!rollback_error.HasError()) {
				rollback_error = ErrorData(ex);
			}
		}
	}
	if (rollback_error.HasError()) {
		rollback_error.Throw();
	}
	transaction->Finalize();
}

void TransactionContext::EndSharedTransaction() {
	auto &transaction = *current_transaction;
	if (transaction.IsShared()) {
		if (!transaction.TryFinish(joined_transaction ? 1 : 0)) {
			throw TransactionException("cannot end the transaction: other connections are running queries on it");
		}
	} else {
		transaction.MarkFinished();
	}
	auto snapshot_id = transaction.GetSnapshotId();
	if (!snapshot_id.empty()) {
		context.db->GetDatabaseManager().RemoveExportedTransaction(snapshot_id);
	}
}

string TransactionContext::ExportSnapshot() {
	if (!HasActiveTransaction() || auto_commit) {
		throw TransactionException("a snapshot can only be exported from within a transaction");
	}
	if (IsJoinedTransaction()) {
		throw TransactionException("cannot export a snapshot from a transaction that was joined from another "
		                           "connection");
	}
	auto snapshot_id = current_transaction->GetSnapshotId();
	if (!snapshot_id.empty()) {
		return snapshot_id;
	}
	snapshot_id = context.db->GetDatabaseManager().ExportTransaction(current_transaction);
	// from now on other connections can use the transaction, this connection is running a query on it
	current_transaction->Share(snapshot_id);
	joined_transaction = current_transaction;
	return snapshot_id;
}

void TransactionContext::ImportSnapshot(const string &snapshot_id) {
	if (!HasActiveTransaction() || auto_commit) {
		throw TransactionException("SET TRANSACTION SNAPSHOT must be called within a transaction");
	}
	if (current_transaction->IsShared() || !current_transaction->OpenedTransactions().empty()) {
		throw TransactionException("SET TRANSACTION SNAPSHOT must be called before any query in the transaction");
	}
	auto source = context.db->GetDatabaseManager().GetExportedTransaction(snapshot_id);
	if (!source || source->IsFinished()) {
		throw TransactionException("invalid snapshot identifier: \"%s\"", snapshot_id);
	}
	if (!source->TryJoin()) {
		throw TransactionException("invalid snapshot identifier: \"%s\"", snapshot_id);
	}
	// discard our own (empty) transaction and continue in the shared one
	auto previous = std::move(current_transaction);
	current_transaction = source;
	joined_transaction = source;
	previous->MarkFinished();
	previous->Rollback();
	for (auto const &s : context.GetContext().GetRegisteredState().States()) {
		s->TransactionRollback(*previous, context, nullptr);
	}
	previous->Finalize();
	for (auto &state : context.GetContext().GetRegisteredState().States()) {
		state->TransactionBegin(*current_transaction, context);
	}
}

void TransactionContext::OnQueryBegin() {
	if (!current_transaction || !current_transaction->IsShared() || joined_transaction) {
		return;
	}
	if (!current_transaction->TryJoin()) {
		// another connection ended the shared transaction
		current_transaction.reset();
		auto_commit = true;
		throw TransactionException("the shared transaction was ended by another connection");
	}
	joined_transaction = current_transaction;
}

void TransactionContext::OnQueryEnd() {
	if (!joined_transaction) {
		return;
	}
	auto transaction = std::move(joined_transaction);
	transaction->Leave();
}

void TransactionContext::WaitForOtherUsers() {
	if (joined_transaction) {
		joined_transaction->WaitForOtherUsers(1);
	}
}

bool TransactionContext::DetachSharedTransaction() {
	if (!IsJoinedTransaction()) {
		return false;
	}
	current_transaction.reset();
	auto_commit = true;
	return true;
}

void TransactionContext::ClearTransaction() {
	SetAutoCommit(true);
	current_transaction = nullptr;
}

idx_t TransactionContext::GetActiveQuery() {
	if (!current_transaction) {
		throw InternalException("GetActiveQuery called without active transaction");
	}
	return active_query_id;
}

void TransactionContext::ResetActiveQuery() {
	if (current_transaction) {
		SetActiveQuery(MAXIMUM_QUERY_ID);
	}
}

void TransactionContext::SetActiveQuery(transaction_t query_number) {
	if (!current_transaction) {
		throw InternalException("SetActiveQuery called without active transaction");
	}
	active_query_id = query_number;
	current_transaction->SetActiveQuery(query_number);
}

} // namespace duckdb
