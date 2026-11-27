//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/variant/variant_type_encoding.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/variant.hpp"

namespace duckdb {

class VariantNode;
struct VariantTypeDescription;

//! A parameter of a type: an optional label (e.g. a STRUCT field name) and a value, which is a type or a scalar
struct VariantTypeModifier {
	VariantTypeModifier();
	VariantTypeModifier(VariantTypeModifier &&other) noexcept;
	VariantTypeModifier &operator=(VariantTypeModifier &&other) noexcept;
	~VariantTypeModifier();

	bool has_label = false;
	string label;
	//! The scalar value of the modifier, if it is not a type
	Value value;
	//! The type of the modifier, if it is one
	unique_ptr<VariantTypeDescription> type;
};

//! A LogicalType, decomposed into the pieces it is stored in as a TYPE value in a VARIANT
struct VariantTypeDescription {
	VariantTypeDescription();
	VariantTypeDescription(VariantTypeDescription &&other) noexcept;
	VariantTypeDescription &operator=(VariantTypeDescription &&other) noexcept;
	~VariantTypeDescription();

	//! The name of the type: the name of its LogicalTypeId, or the alias of an extension type
	string name;
	//! The parameters of the type, in order
	vector<VariantTypeModifier> modifiers;
	//! The (unaliased) layout of an extension type, absent for the other types
	unique_ptr<VariantTypeDescription> physical;
};

//! A TYPE value is stored in a VARIANT as a nested value tagged VariantLogicalType::TYPE, whose children have no keys:
//!  child 0: the name, a VARCHAR
//!  child 1: the modifiers, an ARRAY. Every modifier is an ARRAY of [label, value]: the label is a VARCHAR or a
//!           VARIANT NULL for no label, the value is a scalar, or a TYPE value for a type
//!  child 2: only for extension types, the TYPE value of the type they are stored as (their layout)
//! The type is rebuilt from these without any catalog lookup.
struct VariantTypeEncoding {
public:
	static constexpr idx_t NAME_CHILD = 0;
	static constexpr idx_t MODIFIERS_CHILD = 1;
	static constexpr idx_t PHYSICAL_CHILD = 2;

public:
	//! Decomposes the type, throws NotImplementedException for the types that can't be stored
	DUCKDB_API static VariantTypeDescription Decompose(const LogicalType &type);
	//! Rebuilds the type, throws if the description is not one
	DUCKDB_API static LogicalType Compose(const VariantTypeDescription &description);

	//! Reads the TYPE value in the (unified) variant
	DUCKDB_API static LogicalType Read(const UnifiedVariantVectorData &variant, idx_t row, uint32_t values_index);
	//! Reads the TYPE value the node points at
	DUCKDB_API static LogicalType Read(const VariantNode &node);

	//! The TYPE value in its serialized form (the physical representation of the LogicalTypeId::TYPE)
	DUCKDB_API static string Serialize(const LogicalType &type);
	DUCKDB_API static LogicalType Deserialize(const string_t &serialized_type);

	//! Emits the description to a sink, which is given the values in pre-order:
	//!  sink.Container(tag, child_count, emit_child) for a nested value, emit_child(i) must emit the i-th child
	//!  sink.Scalar(value) and sink.Null() for the others
	template <class SINK>
	static void Emit(const VariantTypeDescription &description, SINK &sink) {
		const idx_t child_count = description.physical ? 3 : 2;
		sink.Container(VariantLogicalType::TYPE, child_count, [&](idx_t child) {
			if (child == NAME_CHILD) {
				sink.Scalar(Value(description.name));
			} else if (child == MODIFIERS_CHILD) {
				sink.Container(VariantLogicalType::ARRAY, description.modifiers.size(), [&](idx_t index) {
					auto &modifier = description.modifiers[index];
					sink.Container(VariantLogicalType::ARRAY, 2, [&](idx_t part) {
						if (part == 0) {
							if (modifier.has_label) {
								sink.Scalar(Value(modifier.label));
							} else {
								sink.Null();
							}
						} else if (modifier.type) {
							Emit(*modifier.type, sink);
						} else {
							sink.Scalar(modifier.value);
						}
					});
				});
			} else {
				Emit(*description.physical, sink);
			}
		});
	}
};

} // namespace duckdb
