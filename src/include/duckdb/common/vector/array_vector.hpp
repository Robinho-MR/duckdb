//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/vector/array_vector.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/vector.hpp"

namespace duckdb {

//! Arrays are stored as lists in which every valid entry has a length of ArrayType::GetSize(type)
struct ArrayVector {
	//! Fills the rows [offset, count) with consecutive entries of the array size and sizes the child to fit them
	DUCKDB_API static void SetConsecutiveEntries(Vector &vector, idx_t count, idx_t offset = 0);
	//! Returns the elements of the first "count" arrays in row order: element j of row i is at i * array_size + j.
	//! NULL arrays have NULL elements. References the child if it is already in this order.
	DUCKDB_API static Vector GetOrderedChild(const Vector &vector, idx_t count);
	//! Reorders the child of the array vector as in GetOrderedChild, and makes the entries of the first "count" rows
	//! consecutive if they are not already. Flattens the vector.
	DUCKDB_API static void MakeConsecutive(Vector &vector, idx_t count);
};

} // namespace duckdb
