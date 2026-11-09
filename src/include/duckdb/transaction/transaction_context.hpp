//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/transaction/transaction_context.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/assert.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/parser/parsed_data/transaction_info.hpp"

namespace duckdb {

class ClientContext;
class MetaTransaction;
class Transaction;
class TransactionManager;

//! The transaction context keeps track of all the information relating to the
//! current transaction
class TransactionContext {
public:
	explicit TransactionContext(ClientContext &context);
	~TransactionContext();

	MetaTransaction &ActiveTransaction() {
		if (!HasActiveTransaction()) {
			throw InternalException("TransactionContext::ActiveTransaction called without active transaction");
		}
		return *current_transaction;
	}

	//! Whether there is a transaction that did not end yet (a shared transaction can be ended by another connection)
	bool HasActiveTransaction() const;

	void BeginTransaction();
	void Commit();
	void Rollback(optional_ptr<ErrorData>);
	void ClearTransaction();
	void SetAutocheckpointError(ErrorData error);

	void SetAutoCommit(bool value);
	bool IsAutoCommit() const {
		return auto_commit;
	}

	void SetReadOnly();

	//! Exports the current transaction so that other connections can join it, returns its snapshot identifier
	string ExportSnapshot();
	//! Replaces the (still unused) current transaction by the exported transaction with the given identifier
	void ImportSnapshot(const string &snapshot_id);
	//! Called when a query starts: registers the query on a shared transaction
	void OnQueryBegin();
	//! Called when a query ended
	void OnQueryEnd();
	//! Blocks until no other connection runs queries on the shared transaction
	void WaitForOtherUsers();
	//! Drops a shared transaction that was created by a different connection without ending it
	bool DetachSharedTransaction();
	//! Whether the current transaction is shared and was created by a different connection
	bool IsJoinedTransaction() const;

	idx_t GetActiveQuery();
	void ResetActiveQuery();
	void SetActiveQuery(transaction_t query_number);

	void SetInvalidationPolicy(TransactionInvalidationPolicy new_invalidation_policy);
	TransactionInvalidationPolicy GetInvalidationPolicy() {
		return invalidation_policy;
	};
	void SetAutoRollback(bool new_auto_rollback) {
		auto_rollback = new_auto_rollback;
	};
	bool GetAutoRollback() {
		return auto_rollback;
	};

private:
	ClientContext &context;
	bool auto_commit;
	TransactionInvalidationPolicy invalidation_policy = TransactionInvalidationPolicy::STANDARD_POLICY;
	bool auto_rollback = false;

	shared_ptr<MetaTransaction> current_transaction;
	//! The shared transaction on which this connection currently runs a query
	shared_ptr<MetaTransaction> joined_transaction;
	//! The query id of the current query of this connection
	transaction_t active_query_id = MAXIMUM_QUERY_ID;
	ErrorData autocheckpoint_error;

	void EndSharedTransaction();

	TransactionContext(const TransactionContext &) = delete;
};

} // namespace duckdb
