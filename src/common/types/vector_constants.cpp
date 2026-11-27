#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/no_destructor.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/types/vector.hpp"

namespace duckdb {

const SelectionVector *ConstantVector::ZeroSelectionVector() {
	static const NoDestructor<SelectionVector> zero_selection_vector(
	    SelectionVector(const_cast<sel_t *>(ConstantVector::ZERO_VECTOR), STANDARD_VECTOR_SIZE)); // NOLINT
	return zero_selection_vector.Get();
}

const SelectionVector *FlatVector::IncrementalSelectionVector() {
	static const NoDestructor<SelectionVector> incremental_selection_vector;
	return incremental_selection_vector.Get();
}

const sel_t ConstantVector::ZERO_VECTOR[STANDARD_VECTOR_SIZE] = {0};

} // namespace duckdb
