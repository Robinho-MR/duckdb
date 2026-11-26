//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/vector/fixed_length_list_vector.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/vector.hpp"

namespace duckdb {

//! A list vector whose lists all have the same length: the list at row i holds the child entries
//! [i * list_length, (i + 1) * list_length), like an array. The child holds entries for NULL lists as well.
class FixedLengthListBuffer : public VectorBuffer {
public:
	FixedLengthListBuffer(const Vector &child, idx_t list_length, count_t count);
	~FixedLengthListBuffer() override;

public:
	ValidityMask &GetValidityMask() override {
		return validity;
	}
	const ValidityMask &GetValidityMask() const override {
		return validity;
	}
	idx_t Capacity() const override {
		return Size();
	}
	const Vector &GetChild() const {
		return *child;
	}
	Vector &GetChild() {
		return *child;
	}
	idx_t GetListLength() const {
		return list_length;
	}
	void SetVectorType(VectorType new_vector_type) override;

public:
	idx_t GetDataSize(const LogicalType &type, idx_t count) const override;
	idx_t GetAllocationSize() const override;
	string ToString(const LogicalType &type, idx_t count) const override;
	Value GetValue(const LogicalType &type, idx_t index) const override;

protected:
	buffer_ptr<VectorBuffer> FlattenSliceInternal(const LogicalType &type, const SelectionVector &sel,
	                                              idx_t count) const override;
	void VerifyInternal(const LogicalType &type, const SelectionVector &sel, idx_t count) const override;

private:
	ValidityMask validity;
	unique_ptr<Vector> child;
	idx_t list_length;
};

struct FixedLengthListVector {
	//! Turns the vector into count lists of list_length entries of the child, which must hold count * list_length
	//! entries
	DUCKDB_API static void Create(Vector &vector, const Vector &child, idx_t list_length, idx_t count);
	DUCKDB_API static idx_t GetListLength(const Vector &vector);
	DUCKDB_API static const Vector &GetChild(const Vector &vector);
	DUCKDB_API static Vector &GetChildMutable(Vector &vector);
	DUCKDB_API static const ValidityMask &Validity(const Vector &vector);
	DUCKDB_API static ValidityMask &ValidityMutable(Vector &vector);
	//! Turns a flat list vector whose lists all have the same length into a fixed length list vector, for testing
	DUCKDB_API static void DebugTransform(Vector &vector, idx_t count);

private:
	static void Verify(const Vector &vector);
};

} // namespace duckdb
