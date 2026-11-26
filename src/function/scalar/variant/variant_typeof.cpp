#include "duckdb/function/scalar/variant_utils.hpp"
#include "duckdb/function/scalar/variant_functions.hpp"
#include "duckdb/common/types/variant_iterator.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/algorithm.hpp"

namespace duckdb {

static void VariantTypeofFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	auto count = input.size();

	D_ASSERT(input.ColumnCount() == 1);
	const auto &variant_vec = input.data[0];
	D_ASSERT(variant_vec.GetType() == LogicalType::VARIANT());

	VariantIterator variant(variant_vec);

	auto result_data = FlatVector::Writer<string_t>(result, count);
	//! Reused across rows to avoid allocating per row
	string type_str;
	vector<string_t> object_keys;
	for (idx_t i = 0; i < count; i++) {
		if (!variant.RowIsValid(i)) {
			result_data.WriteValue(string_t("VARIANT_NULL"));
			continue;
		}

		auto node = variant.Root(i);
		if (node.HasAlias()) {
			//! The name of the type, including its modifiers
			type_str = node.GetType().ToString();
			result_data.WriteValue(string_t(type_str.c_str()));
			continue;
		}
		auto type_id = node.GetTypeId();
		switch (type_id) {
		case LogicalTypeId::DECIMAL: {
			auto decimal = node.GetDecimalProperties();
			type_str = StringUtil::Format("DECIMAL(%d, %d)", decimal.width, decimal.scale);
			break;
		}
		case LogicalTypeId::STRUCT:
			object_keys.clear();
			for (auto &entry : node.GetObjectChildren(VariantIterationOrder::LEXICOGRAPHIC)) {
				object_keys.push_back(entry.key);
			}
			type_str = "OBJECT(";
			for (idx_t key_idx = 0; key_idx < object_keys.size(); key_idx++) {
				if (key_idx > 0) {
					type_str += ", ";
				}
				type_str.append(object_keys[key_idx].GetData(), object_keys[key_idx].GetSize());
			}
			type_str += ")";
			break;
		case LogicalTypeId::LIST:
			type_str = StringUtil::Format("ARRAY(%d)", node.GetArrayChildren().size());
			break;
		default:
			type_str = EnumUtil::ToString(VariantTypeFromLogicalTypeId(type_id, node.GetBool()));
			break;
		}
		result_data.WriteValue(string_t(type_str.c_str()));
	}
}

ScalarFunction VariantTypeofFun::GetFunction() {
	auto variant_type = LogicalType::VARIANT();
	auto res = ScalarFunction("variant_typeof", {}, LogicalType::VARCHAR, VariantTypeofFunction);
	res.GetSignature().AddParameter("input_variant", variant_type);
	res.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return res;
}

} // namespace duckdb
