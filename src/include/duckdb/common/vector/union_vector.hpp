//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/vector/union_vector.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"

namespace duckdb {

enum class VariantLogicalType : uint8_t;

//! A UNION vector is a VARIANT vector holding values of the member types. No tag is stored: the member of a row is
//! the first member whose type matches the type of the stored value.
struct UnionVector {
	//! The member (tag) a value with the given VARIANT type belongs to, if any
	DUCKDB_API static optional_idx FindMember(const LogicalType &union_type, VariantLogicalType value_type);
	//! Computes the member (tag) of every row into a UTINYINT vector - NULL if the row is NULL or matches no member
	DUCKDB_API static void GetTags(const Vector &union_vector, Vector &result, idx_t count);
	//! Computes the member (tag) of every row of a VARIANT vector with respect to 'union_type'
	DUCKDB_API static void GetTags(const Vector &variant_vector, const LogicalType &union_type, Vector &result,
	                               idx_t count);
	//! Extracts a member (cast to the member type) - rows holding another member are NULL
	DUCKDB_API static void GetMember(const Vector &union_vector, idx_t member_index, Vector &result, idx_t count);
	//! Converts a UNION vector to the legacy layout STRUCT(tag UTINYINT, member_1, ..., member_n)
	DUCKDB_API static void ToLegacyLayout(const Vector &union_vector, Vector &result, idx_t count);
	//! Converts a vector in the legacy layout STRUCT(tag UTINYINT, member_1, ..., member_n) into a UNION vector
	DUCKDB_API static void FromLegacyLayout(Vector &legacy, Vector &result, idx_t count);
	//! Converts a vector of UnionType::GetLegacyStorageType(type) into a vector of 'type' (converting nested UNIONs)
	DUCKDB_API static void FromLegacyStorage(Vector &source, Vector &result, idx_t count);
};

} // namespace duckdb
