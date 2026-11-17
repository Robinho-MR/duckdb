#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/uhugeint.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/smaller_binary.hpp"
#include "duckdb/function/scalar/comparison_functions.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Fast path: direct BinaryExecutor for primitive types (single pass, no intermediate vector)
//===--------------------------------------------------------------------===//
template <class OP>
static bool TryPrimitiveComparisonExecute(const Vector &left, const Vector &right, Vector &result) {
#if DUCKDB_SMALLER_BINARY(primitive_comparison_execute)
	return false;
#else
	D_ASSERT(left.GetType().InternalType() == right.GetType().InternalType());
	switch (left.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		BinaryExecutor::Execute<int8_t, int8_t, bool, OP>(left, right, result);
		return true;
	case PhysicalType::INT16:
		BinaryExecutor::Execute<int16_t, int16_t, bool, OP>(left, right, result);
		return true;
	case PhysicalType::INT32:
		BinaryExecutor::Execute<int32_t, int32_t, bool, OP>(left, right, result);
		return true;
	case PhysicalType::INT64:
		BinaryExecutor::Execute<int64_t, int64_t, bool, OP>(left, right, result);
		return true;
	case PhysicalType::UINT8:
		BinaryExecutor::Execute<uint8_t, uint8_t, bool, OP>(left, right, result);
		return true;
	case PhysicalType::UINT16:
		BinaryExecutor::Execute<uint16_t, uint16_t, bool, OP>(left, right, result);
		return true;
	case PhysicalType::UINT32:
		BinaryExecutor::Execute<uint32_t, uint32_t, bool, OP>(left, right, result);
		return true;
	case PhysicalType::UINT64:
		BinaryExecutor::Execute<uint64_t, uint64_t, bool, OP>(left, right, result);
		return true;
	case PhysicalType::INT128:
		BinaryExecutor::Execute<hugeint_t, hugeint_t, bool, OP>(left, right, result);
		return true;
	case PhysicalType::UINT128:
		BinaryExecutor::Execute<uhugeint_t, uhugeint_t, bool, OP>(left, right, result);
		return true;
	case PhysicalType::FLOAT:
		BinaryExecutor::Execute<float, float, bool, OP>(left, right, result);
		return true;
	case PhysicalType::DOUBLE:
		BinaryExecutor::Execute<double, double, bool, OP>(left, right, result);
		return true;
	case PhysicalType::INTERVAL:
		BinaryExecutor::Execute<interval_t, interval_t, bool, OP>(left, right, result);
		return true;
	case PhysicalType::VARCHAR:
		BinaryExecutor::Execute<string_t, string_t, bool, OP>(left, right, result);
		return true;
	default:
		return false;
	}
#endif
}

template <class PREDICATE>
static void ComparatorToBoolean(const Vector &left, const Vector &right, Vector &result, const ExpressionType comp,
                                PREDICATE predicate) {
	D_ASSERT(result.GetType() == LogicalType::BOOLEAN);
	Vector comparator_result(LogicalType::TINYINT);
	VectorOperations::Comparator(left, right, comparator_result, comp);
	const auto count = comparator_result.size();
	auto cmp_data = comparator_result.Values<int8_t>();
	if (left.GetVectorType() == VectorType::CONSTANT_VECTOR && right.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		// the comparison of two constants is a constant
		auto entry = cmp_data[0];
		auto value = entry.IsValid() ? Value::BOOLEAN(predicate(entry.GetValue())) : Value(LogicalType::BOOLEAN);
		result.Reference(value, count_t(count));
		return;
	}
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::Writer<bool>(result, count);
	for (idx_t i = 0; i < count; i++) {
		auto entry = cmp_data[i];
		if (!entry.IsValid()) {
			result_data.WriteNull();
		} else {
			result_data.WriteValue(predicate(entry.GetValue()));
		}
	}
}

static void EqualsExecute(const Vector &left, const Vector &right, Vector &result) {
	if (!TryPrimitiveComparisonExecute<Equals>(left, right, result)) {
		ComparatorToBoolean(left, right, result, ExpressionType::COMPARE_EQUAL, [](int8_t v) { return v == 0; });
	}
}

static void NotEqualsExecute(const Vector &left, const Vector &right, Vector &result) {
	if (!TryPrimitiveComparisonExecute<NotEquals>(left, right, result)) {
		ComparatorToBoolean(left, right, result, ExpressionType::COMPARE_NOTEQUAL, [](int8_t v) { return v != 0; });
	}
}

static void GreaterThanExecute(const Vector &left, const Vector &right, Vector &result) {
	if (!TryPrimitiveComparisonExecute<GreaterThan>(left, right, result)) {
		ComparatorToBoolean(left, right, result, ExpressionType::COMPARE_GREATERTHAN, [](int8_t v) { return v > 0; });
	}
}

static void GreaterThanEqualsExecute(const Vector &left, const Vector &right, Vector &result) {
	if (!TryPrimitiveComparisonExecute<GreaterThanEquals>(left, right, result)) {
		ComparatorToBoolean(left, right, result, ExpressionType::COMPARE_GREATERTHANOREQUALTO,
		                    [](int8_t v) { return v >= 0; });
	}
}

static void LessThanExecute(const Vector &left, const Vector &right, Vector &result) {
	// NOLINTNEXTLINE: flip right / left (left < right is equal to right > left)
	if (!TryPrimitiveComparisonExecute<GreaterThan>(right, left, result)) {
		ComparatorToBoolean(left, right, result, ExpressionType::COMPARE_LESSTHAN, [](int8_t v) { return v < 0; });
	}
}

static void LessThanEqualsExecute(const Vector &left, const Vector &right, Vector &result) {
	// NOLINTNEXTLINE: flip right / left (left <= right is equal to right >= left)
	if (!TryPrimitiveComparisonExecute<GreaterThanEquals>(right, left, result)) {
		ComparatorToBoolean(left, right, result, ExpressionType::COMPARE_LESSTHANOREQUALTO,
		                    [](int8_t v) { return v <= 0; });
	}
}

static void DistinctFromExecute(const Vector &left, const Vector &right, Vector &result) {
	D_ASSERT(result.GetType() == LogicalType::BOOLEAN);
	Vector comparator_result(LogicalType::TINYINT);
	VectorOperations::DistinctComparator(left, right, comparator_result);
	const idx_t count = comparator_result.size();
	auto cmp_data = comparator_result.Values<int8_t>();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::Writer<bool>(result, count);
	for (idx_t i = 0; i < count; i++) {
		result_data.WriteValue(cmp_data[i].GetValueUnsafe() != 0);
	}
}

static void NotDistinctFromExecute(const Vector &left, const Vector &right, Vector &result) {
	D_ASSERT(result.GetType() == LogicalType::BOOLEAN);
	Vector comparator_result(LogicalType::TINYINT);
	VectorOperations::DistinctComparator(left, right, comparator_result);
	const idx_t count = comparator_result.size();
	auto cmp_data = comparator_result.Values<int8_t>();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::Writer<bool>(result, count);
	for (idx_t i = 0; i < count; i++) {
		result_data.WriteValue(cmp_data[i].GetValueUnsafe() == 0);
	}
}

//===--------------------------------------------------------------------===//
template <class OP>
static bool TryPrimitiveSelectOperation(const Vector &left, const Vector &right,
                                        optional_ptr<const SelectionVector> sel, idx_t count,
                                        optional_ptr<SelectionVector> true_sel, optional_ptr<SelectionVector> false_sel,
                                        idx_t &result) {
#if DUCKDB_SMALLER_BINARY(primitive_select_execute)
	return false;
#else
	switch (left.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		result =
		    BinaryExecutor::Select<int8_t, int8_t, OP>(left, right, sel.get(), count, true_sel.get(), false_sel.get());
		return true;
	case PhysicalType::INT16:
		result = BinaryExecutor::Select<int16_t, int16_t, OP>(left, right, sel.get(), count, true_sel.get(),
		                                                      false_sel.get());
		return true;
	case PhysicalType::INT32:
		result = BinaryExecutor::Select<int32_t, int32_t, OP>(left, right, sel.get(), count, true_sel.get(),
		                                                      false_sel.get());
		return true;
	case PhysicalType::INT64:
		result = BinaryExecutor::Select<int64_t, int64_t, OP>(left, right, sel.get(), count, true_sel.get(),
		                                                      false_sel.get());
		return true;
	case PhysicalType::UINT8:
		result = BinaryExecutor::Select<uint8_t, uint8_t, OP>(left, right, sel.get(), count, true_sel.get(),
		                                                      false_sel.get());
		return true;
	case PhysicalType::UINT16:
		result = BinaryExecutor::Select<uint16_t, uint16_t, OP>(left, right, sel.get(), count, true_sel.get(),
		                                                        false_sel.get());
		return true;
	case PhysicalType::UINT32:
		result = BinaryExecutor::Select<uint32_t, uint32_t, OP>(left, right, sel.get(), count, true_sel.get(),
		                                                        false_sel.get());
		return true;
	case PhysicalType::UINT64:
		result = BinaryExecutor::Select<uint64_t, uint64_t, OP>(left, right, sel.get(), count, true_sel.get(),
		                                                        false_sel.get());
		return true;
	case PhysicalType::INT128:
		result = BinaryExecutor::Select<hugeint_t, hugeint_t, OP>(left, right, sel.get(), count, true_sel.get(),
		                                                          false_sel.get());
		return true;
	case PhysicalType::UINT128:
		result = BinaryExecutor::Select<uhugeint_t, uhugeint_t, OP>(left, right, sel.get(), count, true_sel.get(),
		                                                            false_sel.get());
		return true;
	case PhysicalType::FLOAT:
		result =
		    BinaryExecutor::Select<float, float, OP>(left, right, sel.get(), count, true_sel.get(), false_sel.get());
		return true;
	case PhysicalType::DOUBLE:
		result =
		    BinaryExecutor::Select<double, double, OP>(left, right, sel.get(), count, true_sel.get(), false_sel.get());
		return true;
	case PhysicalType::INTERVAL:
		result = BinaryExecutor::Select<interval_t, interval_t, OP>(left, right, sel.get(), count, true_sel.get(),
		                                                            false_sel.get());
		return true;
	case PhysicalType::VARCHAR:
		result = BinaryExecutor::Select<string_t, string_t, OP>(left, right, sel.get(), count, true_sel.get(),
		                                                        false_sel.get());
		return true;
	default:
		return false;
	}
#endif
}

template <class PREDICATE>
static idx_t ComparatorSelectOperation(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel,
                                       idx_t count, const ExpressionType comp, optional_ptr<SelectionVector> true_sel,
                                       optional_ptr<SelectionVector> false_sel, PREDICATE predicate) {
	Vector comparator_result(LogicalType::TINYINT, count);
	VectorOperations::ComparatorFill(left, right, comparator_result, count, comp);
	auto cmp_data = comparator_result.Values<int8_t>();

	if (!sel) {
		sel = FlatVector::IncrementalSelectionVector();
	}
	idx_t true_count = 0;
	idx_t false_count = 0;
	for (idx_t i = 0; i < count; i++) {
		auto result_idx = sel->get_index(i);
		auto entry = cmp_data[i];
		if (!entry.IsValid()) {
			// NULL result: goes to false_sel
			if (false_sel) {
				false_sel->set_index(false_count, result_idx);
			}
			false_count++;
		} else if (predicate(entry.GetValue())) {
			if (true_sel) {
				true_sel->set_index(true_count, result_idx);
			}
			true_count++;
		} else {
			if (false_sel) {
				false_sel->set_index(false_count, result_idx);
			}
			false_count++;
		}
	}
	return true_count;
}

static idx_t EqualsSelect(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel, idx_t count,
                          optional_ptr<SelectionVector> true_sel, optional_ptr<SelectionVector> false_sel) {
	idx_t result;
	if (TryPrimitiveSelectOperation<Equals>(left, right, sel, count, true_sel, false_sel, result)) {
		return result;
	}
	return ComparatorSelectOperation(left, right, sel, count, ExpressionType::COMPARE_EQUAL, true_sel, false_sel,
	                                 [](int8_t v) { return v == Comparator::VALUES_ARE_EQUAL; });
}

static idx_t NotEqualsSelect(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel,
                             idx_t count, optional_ptr<SelectionVector> true_sel,
                             optional_ptr<SelectionVector> false_sel) {
	idx_t result;
	if (TryPrimitiveSelectOperation<NotEquals>(left, right, sel, count, true_sel, false_sel, result)) {
		return result;
	}
	return ComparatorSelectOperation(left, right, sel, count, ExpressionType::COMPARE_NOTEQUAL, true_sel, false_sel,
	                                 [](int8_t v) { return v != Comparator::VALUES_ARE_EQUAL; });
}

static idx_t GreaterThanSelect(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel,
                               idx_t count, optional_ptr<SelectionVector> true_sel,
                               optional_ptr<SelectionVector> false_sel) {
	idx_t result;
	if (TryPrimitiveSelectOperation<GreaterThan>(left, right, sel, count, true_sel, false_sel, result)) {
		return result;
	}
	return ComparatorSelectOperation(left, right, sel, count, ExpressionType::COMPARE_GREATERTHAN, true_sel, false_sel,
	                                 [](int8_t v) { return v > 0; });
}

static idx_t GreaterThanEqualsSelect(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel,
                                     idx_t count, optional_ptr<SelectionVector> true_sel,
                                     optional_ptr<SelectionVector> false_sel) {
	idx_t result;
	if (TryPrimitiveSelectOperation<GreaterThanEquals>(left, right, sel, count, true_sel, false_sel, result)) {
		return result;
	}
	return ComparatorSelectOperation(left, right, sel, count, ExpressionType::COMPARE_GREATERTHANOREQUALTO, true_sel,
	                                 false_sel, [](int8_t v) { return v >= 0; });
}

static idx_t LessThanSelect(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel,
                            idx_t count, optional_ptr<SelectionVector> true_sel,
                            optional_ptr<SelectionVector> false_sel) {
	idx_t result;
	if (TryPrimitiveSelectOperation<GreaterThan>(right, left, sel, count, true_sel, false_sel, result)) {
		return result;
	}
	return ComparatorSelectOperation(left, right, sel, count, ExpressionType::COMPARE_LESSTHAN, true_sel, false_sel,
	                                 [](int8_t v) { return v < 0; });
}

static idx_t LessThanEqualsSelect(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel,
                                  idx_t count, optional_ptr<SelectionVector> true_sel,
                                  optional_ptr<SelectionVector> false_sel) {
	idx_t result;
	if (TryPrimitiveSelectOperation<GreaterThanEquals>(right, left, sel, count, true_sel, false_sel, result)) {
		return result;
	}
	return ComparatorSelectOperation(left, right, sel, count, ExpressionType::COMPARE_LESSTHANOREQUALTO, true_sel,
	                                 false_sel, [](int8_t v) { return v <= 0; });
}

template <class PREDICATE>
static idx_t DistinctComparatorSelect(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel,
                                      idx_t count, optional_ptr<SelectionVector> true_sel,
                                      optional_ptr<SelectionVector> false_sel, PREDICATE predicate) {
	Vector comparator_result(LogicalType::TINYINT, count);
	VectorOperations::DistinctComparatorFill(left, right, comparator_result, count);
	auto cmp_data = comparator_result.Values<int8_t>();
	idx_t true_count = 0;
	idx_t false_count = 0;
	for (idx_t i = 0; i < count; i++) {
		auto result_idx = sel ? sel->get_index(i) : i;
		if (predicate(cmp_data[i].GetValueUnsafe())) {
			if (true_sel) {
				true_sel->set_index(true_count, result_idx);
			}
			true_count++;
		} else {
			if (false_sel) {
				false_sel->set_index(false_count, result_idx);
			}
			false_count++;
		}
	}
	return true_count;
}

static idx_t DistinctFromSelect(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel,
                                idx_t count, optional_ptr<SelectionVector> true_sel,
                                optional_ptr<SelectionVector> false_sel) {
	return DistinctComparatorSelect(left, right, sel, count, true_sel, false_sel, [](int8_t v) { return v != 0; });
}

static idx_t NotDistinctFromSelect(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel,
                                   idx_t count, optional_ptr<SelectionVector> true_sel,
                                   optional_ptr<SelectionVector> false_sel) {
	return DistinctComparatorSelect(left, right, sel, count, true_sel, false_sel, [](int8_t v) { return v == 0; });
}

template <ExpressionType TYPE>
void ComparisonFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	(void)state;
	const auto &left = args.data[0];
	const auto &right = args.data[1];

	if constexpr (TYPE == ExpressionType::COMPARE_EQUAL) {
		EqualsExecute(left, right, result);
	} else if constexpr (TYPE == ExpressionType::COMPARE_NOTEQUAL) {
		NotEqualsExecute(left, right, result);
	} else if constexpr (TYPE == ExpressionType::COMPARE_LESSTHAN) {
		LessThanExecute(left, right, result);
	} else if constexpr (TYPE == ExpressionType::COMPARE_GREATERTHAN) {
		GreaterThanExecute(left, right, result);
	} else if constexpr (TYPE == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
		LessThanEqualsExecute(left, right, result);
	} else if constexpr (TYPE == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
		GreaterThanEqualsExecute(left, right, result);
	} else if constexpr (TYPE == ExpressionType::COMPARE_DISTINCT_FROM) {
		DistinctFromExecute(left, right, result);
	} else if constexpr (TYPE == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
		NotDistinctFromExecute(left, right, result);
	} else {
		throw InternalException("Unknown comparison type!");
	}
}

#if !DUCKDB_SMALLER_BINARY(comparison_select)
template <ExpressionType TYPE>
idx_t ComparisonSelect(DataChunk &args, ExpressionState &state, optional_ptr<const SelectionVector> sel,
                       optional_ptr<SelectionVector> true_sel, optional_ptr<SelectionVector> false_sel) {
	(void)state;
	const auto &left = args.data[0];
	const auto &right = args.data[1];
	auto count = args.size();

	if constexpr (TYPE == ExpressionType::COMPARE_EQUAL) {
		return EqualsSelect(left, right, sel, count, true_sel, false_sel);
	} else if constexpr (TYPE == ExpressionType::COMPARE_NOTEQUAL) {
		return NotEqualsSelect(left, right, sel, count, true_sel, false_sel);
	} else if constexpr (TYPE == ExpressionType::COMPARE_LESSTHAN) {
		return LessThanSelect(left, right, sel, count, true_sel, false_sel);
	} else if constexpr (TYPE == ExpressionType::COMPARE_GREATERTHAN) {
		return GreaterThanSelect(left, right, sel, count, true_sel, false_sel);
	} else if constexpr (TYPE == ExpressionType::COMPARE_LESSTHANOREQUALTO) {
		return LessThanEqualsSelect(left, right, sel, count, true_sel, false_sel);
	} else if constexpr (TYPE == ExpressionType::COMPARE_GREATERTHANOREQUALTO) {
		return GreaterThanEqualsSelect(left, right, sel, count, true_sel, false_sel);
	} else if constexpr (TYPE == ExpressionType::COMPARE_DISTINCT_FROM) {
		return DistinctFromSelect(left, right, sel, count, true_sel, false_sel);
	} else if constexpr (TYPE == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
		return NotDistinctFromSelect(left, right, sel, count, true_sel, false_sel);
	} else {
		throw InternalException("Unknown comparison type!");
	}
}
#endif

template <ExpressionType TYPE>
ExpressionType ComparisonGetExpressionType(FunctionToStringInput &input) {
	(void)input;
	return TYPE;
}

//! Cast the arguments of a comparison to a common type - both for comparison operators and for "="(a, b)
template <ExpressionType TYPE>
static unique_ptr<FunctionData> ComparisonBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &arguments = input.GetArguments();
	auto left_type = ExpressionBinder::GetExpressionReturnType(*arguments[0]);
	auto right_type = ExpressionBinder::GetExpressionReturnType(*arguments[1]);
	LogicalType input_type;
	if (!BoundComparisonExpression::TryBindComparison(context, left_type, right_type, input_type, TYPE,
	                                                  input.GetDecimalWidthLimit())) {
		throw BinderException("Cannot compare values of type %s and type %s - an explicit cast is required",
		                      left_type.ToString(), right_type.ToString());
	}
	for (auto &argument : arguments) {
		argument = BoundCastExpression::AddCastToType(context, std::move(argument), input_type,
		                                              input_type.id() == LogicalTypeId::ENUM);
		ExpressionBinder::PushCollation(context, argument, input_type);
	}
	return nullptr;
}

//! The arguments of a deserialized comparison have already been cast - so it should not be bound again
static void ComparisonSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
                                const BoundScalarFunction &function) {
}

static unique_ptr<FunctionData> ComparisonDeserialize(Deserializer &deserializer, BoundScalarFunction &function) {
	return nullptr;
}

template <ExpressionType TYPE>
static ScalarFunction GetComparisonFunctionInternal(const string &name) {
	ScalarFunction comparison_fun(Identifier(name), {}, LogicalType::BOOLEAN, ComparisonFunction<TYPE>,
	                              ComparisonBind<TYPE>);
	comparison_fun.GetSignature().AddParameter("left", LogicalType::ANY).AddParameter("right", LogicalType::ANY);
	comparison_fun.SetGetExpressionTypeCallback(ComparisonGetExpressionType<TYPE>);
	comparison_fun.SetSerializeCallback(ComparisonSerialize);
	comparison_fun.SetDeserializeCallback(ComparisonDeserialize);
	if constexpr (TYPE == ExpressionType::COMPARE_DISTINCT_FROM || TYPE == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
		comparison_fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	}
#if !DUCKDB_SMALLER_BINARY(comparison_select)
	comparison_fun.SetSelectCallback(ComparisonSelect<TYPE>);
#endif
	return comparison_fun;
}

static ScalarFunction GetComparisonFunction(ExpressionType type) {
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
		return OperatorEqualFun::GetFunction();
	case ExpressionType::COMPARE_NOTEQUAL:
		return OperatorNotEqualFun::GetFunction();
	case ExpressionType::COMPARE_LESSTHAN:
		return OperatorLessThanFun::GetFunction();
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return OperatorLessThanEqualsFun::GetFunction();
	case ExpressionType::COMPARE_GREATERTHAN:
		return OperatorGreaterThanFun::GetFunction();
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return OperatorGreaterThanEqualsFun::GetFunction();
	case ExpressionType::COMPARE_DISTINCT_FROM:
		return IsDistinctFromFun::GetFunction();
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		return IsNotDistinctFromFun::GetFunction();
	default:
		throw InternalException("Unknown comparison type!");
	}
}

ScalarFunction OperatorEqualFun::GetFunction() {
	return GetComparisonFunctionInternal<ExpressionType::COMPARE_EQUAL>(OperatorEqualFun::Name);
}

ScalarFunction OperatorNotEqualFun::GetFunction() {
	return GetComparisonFunctionInternal<ExpressionType::COMPARE_NOTEQUAL>(OperatorNotEqualFun::Name);
}

ScalarFunction OperatorLessThanFun::GetFunction() {
	return GetComparisonFunctionInternal<ExpressionType::COMPARE_LESSTHAN>(OperatorLessThanFun::Name);
}

ScalarFunction OperatorLessThanEqualsFun::GetFunction() {
	return GetComparisonFunctionInternal<ExpressionType::COMPARE_LESSTHANOREQUALTO>(OperatorLessThanEqualsFun::Name);
}

ScalarFunction OperatorGreaterThanFun::GetFunction() {
	return GetComparisonFunctionInternal<ExpressionType::COMPARE_GREATERTHAN>(OperatorGreaterThanFun::Name);
}

ScalarFunction OperatorGreaterThanEqualsFun::GetFunction() {
	return GetComparisonFunctionInternal<ExpressionType::COMPARE_GREATERTHANOREQUALTO>(
	    OperatorGreaterThanEqualsFun::Name);
}

ScalarFunction IsDistinctFromFun::GetFunction() {
	return GetComparisonFunctionInternal<ExpressionType::COMPARE_DISTINCT_FROM>(IsDistinctFromFun::Name);
}

ScalarFunction IsNotDistinctFromFun::GetFunction() {
	return GetComparisonFunctionInternal<ExpressionType::COMPARE_NOT_DISTINCT_FROM>(IsNotDistinctFromFun::Name);
}

//===--------------------------------------------------------------------===//
// BoundComparisonExpression
//===--------------------------------------------------------------------===//
unique_ptr<Expression> BoundComparisonExpression::Create(ExpressionType type, unique_ptr<Expression> left,
                                                         unique_ptr<Expression> right) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(left));
	children.push_back(std::move(right));

	auto result = make_uniq<BoundFunctionExpression>(BoundScalarFunction(GetComparisonFunction(type)),
	                                                 std::move(children), nullptr, true);
	return std::move(result);
}

unique_ptr<Expression> BoundComparisonExpression::Bind(ClientContext &context, ExpressionType type,
                                                       unique_ptr<Expression> left, unique_ptr<Expression> right,
                                                       optional_ptr<Binder> binder) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(left));
	children.push_back(std::move(right));

	BoundScalarFunction function(GetComparisonFunction(type));
	BindScalarFunctionInput input(context, function, children, binder);
	function.GetBindCallback()(input);
	return make_uniq<BoundFunctionExpression>(std::move(function), std::move(children), nullptr, true);
}

bool BoundComparisonExpression::IsComparison(ExpressionType type) {
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_NOTEQUAL:
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
	case ExpressionType::COMPARE_DISTINCT_FROM:
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		return true;
	default:
		return false;
	}
}

bool BoundComparisonExpression::IsComparison(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	return IsComparison(expr.GetExpressionType());
}

const Expression &BoundComparisonExpression::Left(const BoundFunctionExpression &comparison_expr) {
	return *comparison_expr.GetChildren()[0];
}

const Expression &BoundComparisonExpression::Right(const BoundFunctionExpression &comparison_expr) {
	return *comparison_expr.GetChildren()[1];
}

unique_ptr<Expression> &BoundComparisonExpression::LeftMutable(BoundFunctionExpression &comparison_expr) {
	return comparison_expr.GetChildrenMutable()[0];
}

unique_ptr<Expression> &BoundComparisonExpression::RightMutable(BoundFunctionExpression &comparison_expr) {
	return comparison_expr.GetChildrenMutable()[1];
}

void BoundComparisonExpression::SetType(BoundFunctionExpression &comparison_expr, ExpressionType new_type) {
	auto arguments = comparison_expr.Function().GetArguments();

	comparison_expr.SetExpressionTypeUnsafe(new_type);
	comparison_expr.FunctionMutable() = BoundScalarFunction(GetComparisonFunction(new_type));
	comparison_expr.FunctionMutable().GetArguments() = std::move(arguments);
	comparison_expr.BindInfoMutable().reset();
	comparison_expr.IsOperatorMutable() = true;
}

void BoundComparisonExpression::FlipType(BoundFunctionExpression &comparison_expr) {
	SetType(comparison_expr, FlipComparisonExpression(comparison_expr.GetExpressionType()));
}

} // namespace duckdb
