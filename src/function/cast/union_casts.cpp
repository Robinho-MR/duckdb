#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/types/variant.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector/union_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/cast/bound_cast_data.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/cast/default_casts.hpp"

namespace duckdb {

//! Local state that holds the local states of a list of child casts
struct UnionCastLocalState : public FunctionLocalState {
	vector<unique_ptr<FunctionLocalState>> local_states;
};

static unique_ptr<FunctionLocalState> InitChildLocalState(CastLocalStateParameters &parameters,
                                                          BoundCastInfo &cast_info) {
	if (!cast_info.HasInitLocalState()) {
		return nullptr;
	}
	CastLocalStateParameters child_params(parameters, cast_info.GetCastData());
	return cast_info.InitLocalState(child_params);
}

static bool ExecuteChildCast(BoundCastInfo &cast_info, unique_ptr<FunctionLocalState> &local_state, Vector &source,
                             Vector &result, idx_t count, CastParameters &parameters) {
	CastParameters child_params(parameters, cast_info.GetCastData(), local_state);
	return cast_info.Cast(source, result, count, child_params);
}

//--------------------------------------------------------------------------------------------------
// ??? -> UNION
//--------------------------------------------------------------------------------------------------
// the source is cast to the member it can be implicitly cast to with the lowest cost, and then stored as a VARIANT
struct ToUnionMemberCastData : public BoundCastData {
	ToUnionMemberCastData(LogicalType member_type_p, BoundCastInfo member_cast_p, BoundCastInfo variant_cast_p)
	    : member_type(std::move(member_type_p)), member_cast(std::move(member_cast_p)),
	      variant_cast(std::move(variant_cast_p)) {
	}

	LogicalType member_type;
	//! source -> member type
	BoundCastInfo member_cast;
	//! member type -> VARIANT
	BoundCastInfo variant_cast;

public:
	unique_ptr<BoundCastData> Copy() const override {
		return make_uniq<ToUnionMemberCastData>(member_type, member_cast.Copy(), variant_cast.Copy());
	}
};

static unique_ptr<FunctionLocalState> InitToUnionMemberLocalState(CastLocalStateParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<ToUnionMemberCastData>();
	auto result = make_uniq<UnionCastLocalState>();
	result->local_states.push_back(InitChildLocalState(parameters, cast_data.member_cast));
	result->local_states.push_back(InitChildLocalState(parameters, cast_data.variant_cast));
	return std::move(result);
}

static bool ToUnionMemberCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<ToUnionMemberCastData>();
	auto &local_state = parameters.local_state->Cast<UnionCastLocalState>();
	Vector member(cast_data.member_type, count);
	auto success =
	    ExecuteChildCast(cast_data.member_cast, local_state.local_states[0], source, member, count, parameters);
	ExecuteChildCast(cast_data.variant_cast, local_state.local_states[1], member, result, count, parameters);
	return success;
}

static idx_t BindToUnionMember(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	D_ASSERT(UnionType::IsUnion(target));
	vector<idx_t> candidates;
	int64_t best_cost = std::numeric_limits<int64_t>::max();
	auto &members = UnionType::GetMemberTypes(target);
	for (idx_t member_idx = 0; member_idx < members.size(); member_idx++) {
		auto member_cast_cost = input.function_set.ImplicitCastCost(nullptr, source, members[member_idx].second);
		if (member_cast_cost >= 0 && member_cast_cost <= best_cost) {
			if (member_cast_cost != best_cost) {
				candidates.clear();
				best_cost = member_cast_cost;
			}
			candidates.push_back(member_idx);
		}
	}
	if (candidates.empty()) {
		auto message = StringUtil::Format(
		    "Type %s can't be cast as %s. %s can't be implicitly cast to any of the union member types: ",
		    source.ToString(), target.ToString(), source.ToString());
		for (idx_t member_idx = 0; member_idx < members.size(); member_idx++) {
			message += members[member_idx].second.ToString();
			if (member_idx + 1 < members.size()) {
				message += ", ";
			}
		}
		throw ConversionException(message);
	}
	if (candidates.size() > 1) {
		auto message = StringUtil::Format(
		    "Type %s can't be cast as %s. The cast is ambiguous, multiple possible members in target: ", source,
		    target);
		for (idx_t i = 0; i < candidates.size(); i++) {
			auto &member = members[candidates[i]];
			message += StringUtil::Format("'%s (%s)'", member.first, member.second.ToString());
			if (i + 1 < candidates.size()) {
				message += ", ";
			}
		}
		message += ". Disambiguate by casting the source value to one of the member types first.";
		throw ConversionException(message);
	}
	return candidates[0];
}

//--------------------------------------------------------------------------------------------------
// VARIANT/UNION -> UNION
//--------------------------------------------------------------------------------------------------
// a value belongs to the first member whose type matches the type of the value
// values that match no member are cast to the first member they can be cast to
struct VariantToUnionCastData : public BoundCastData {
	VariantToUnionCastData(LogicalType target_p, vector<BoundCastInfo> member_casts_p)
	    : target(std::move(target_p)), member_casts(std::move(member_casts_p)) {
	}

	LogicalType target;
	//! VARIANT -> member type, for every member
	vector<BoundCastInfo> member_casts;

public:
	unique_ptr<BoundCastData> Copy() const override {
		vector<BoundCastInfo> member_casts_copy;
		for (auto &cast : member_casts) {
			member_casts_copy.push_back(cast.Copy());
		}
		return make_uniq<VariantToUnionCastData>(target, std::move(member_casts_copy));
	}
};

static unique_ptr<FunctionLocalState> InitVariantToUnionLocalState(CastLocalStateParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<VariantToUnionCastData>();
	auto result = make_uniq<UnionCastLocalState>();
	for (auto &cast : cast_data.member_casts) {
		result->local_states.push_back(InitChildLocalState(parameters, cast));
	}
	return std::move(result);
}

//! Casts the rows of 'source' for which 'include' returns true to 'result' - other rows are NULL
template <class F>
static bool CastSelectedRows(Vector &source, Vector &result, idx_t count, BoundCastInfo &cast,
                             unique_ptr<FunctionLocalState> &local_state, CastParameters &parameters, F &&include) {
	Vector masked(source.GetType(), count);
	VectorOperations::Copy(source, masked, count, 0, 0);
	for (idx_t i = 0; i < count; i++) {
		if (!include(i)) {
			FlatVector::SetNull(masked, i, true);
		}
	}
	return ExecuteChildCast(cast, local_state, masked, result, count, parameters);
}

static bool VariantToUnionCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<VariantToUnionCastData>();
	auto &local_state = parameters.local_state->Cast<UnionCastLocalState>();
	auto &target = cast_data.target;
	auto member_count = UnionType::GetMemberCount(target);

	// compute the member of every value from its type
	Vector tag_vector(LogicalType::UTINYINT, count);
	UnionVector::GetTags(source, target, tag_vector, count);
	auto source_validity = source.Validity();
	auto tag_values = tag_vector.Values<union_tag_t>();

	vector<int64_t> tags(count, -1);
	bool all_matched = true;
	for (idx_t i = 0; i < count; i++) {
		auto tag = tag_values[i];
		if (tag.IsValid()) {
			tags[i] = tag.GetValue();
		} else if (source_validity.IsValid(i)) {
			all_matched = false;
		}
	}
	if (all_matched) {
		// every value already belongs to a member
		result.Reinterpret(source);
		return true;
	}

	// some values don't match the type of any member - assign them to the first member they can be cast to
	// a NULL member value means "no member" here, as it can't be distinguished from a NULL VARIANT
	RecursiveUnifiedVectorFormat source_format;
	Vector::RecursiveToUnifiedFormat(source, source_format);
	UnifiedVariantVectorData variant_data(source_format);
	auto is_pending = [&](idx_t i) {
		return tags[i] < 0 && variant_data.RowIsValid(i) &&
		       variant_data.GetTypeId(i, 0) != VariantLogicalType::VARIANT_NULL;
	};
	for (idx_t member_idx = 0; member_idx < member_count; member_idx++) {
		auto &member_type = UnionType::GetMemberType(target, member_idx);
		Vector member(member_type, count);
		string ignored_error;
		CastParameters try_params(parameters, cast_data.member_casts[member_idx].GetCastData(),
		                          local_state.local_states[member_idx]);
		try_params.error_message = &ignored_error;
		try_params.strict = false;
		CastSelectedRows(source, member, count, cast_data.member_casts[member_idx],
		                 local_state.local_states[member_idx], try_params, is_pending);
		auto member_validity = member.Validity();
		for (idx_t i = 0; i < count; i++) {
			if (is_pending(i) && member_validity.IsValid(i)) {
				tags[i] = UnsafeNumericCast<int64_t>(member_idx);
			}
		}
	}

	bool success = true;
	for (idx_t i = 0; i < count; i++) {
		if (is_pending(i)) {
			HandleCastError::AssignError(StringUtil::Format("Could not convert value %s to %s, it does not match "
			                                                "any of the member types",
			                                                source.GetValue(i).ToString(), target.ToString()),
			                             parameters);
			success = false;
		}
	}

	// build the legacy layout (one child per member), and convert it back to a VARIANT
	Vector legacy(UnionType::GetMemberStructType(target), count);
	auto &legacy_entries = StructVector::GetEntries(legacy);
	// the tags are not used by the conversion
	legacy_entries[0].SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::SetNull(legacy_entries[0], count_t(count));
	for (idx_t member_idx = 0; member_idx < member_count; member_idx++) {
		if (!CastSelectedRows(source, legacy_entries[member_idx + 1], count, cast_data.member_casts[member_idx],
		                      local_state.local_states[member_idx], parameters,
		                      [&](idx_t i) { return tags[i] == static_cast<int64_t>(member_idx); })) {
			success = false;
		}
	}
	FlatVector::SetSize(legacy, count);
	UnionVector::FromLegacyLayout(legacy, result, count);
	return success;
}

//--------------------------------------------------------------------------------------------------
// UNION -> UNION (by name)
//--------------------------------------------------------------------------------------------------
// if every member of the source exists (by name) in the target, a value is cast to the member with the same name
struct UnionByNameCastData : public BoundCastData {
	UnionByNameCastData(LogicalType target_p, vector<idx_t> target_indexes_p, vector<BoundCastInfo> member_casts_p)
	    : target(std::move(target_p)), target_indexes(std::move(target_indexes_p)),
	      member_casts(std::move(member_casts_p)) {
	}

	LogicalType target;
	//! For every source member, the target member with the same name
	vector<idx_t> target_indexes;
	//! For every source member, source member type -> target member type
	vector<BoundCastInfo> member_casts;

public:
	unique_ptr<BoundCastData> Copy() const override {
		vector<BoundCastInfo> member_casts_copy;
		for (auto &cast : member_casts) {
			member_casts_copy.push_back(cast.Copy());
		}
		return make_uniq<UnionByNameCastData>(target, target_indexes, std::move(member_casts_copy));
	}
};

static unique_ptr<FunctionLocalState> InitUnionByNameLocalState(CastLocalStateParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<UnionByNameCastData>();
	auto result = make_uniq<UnionCastLocalState>();
	for (auto &cast : cast_data.member_casts) {
		result->local_states.push_back(InitChildLocalState(parameters, cast));
	}
	return std::move(result);
}

static bool UnionByNameCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<UnionByNameCastData>();
	auto &local_state = parameters.local_state->Cast<UnionCastLocalState>();
	auto &source_type = source.GetType();

	Vector legacy(UnionType::GetMemberStructType(cast_data.target), count);
	auto &legacy_entries = StructVector::GetEntries(legacy);
	vector<bool> is_mapped(legacy_entries.size(), false);
	for (auto &target_idx : cast_data.target_indexes) {
		is_mapped[target_idx + 1] = true;
	}
	for (idx_t i = 0; i < legacy_entries.size(); i++) {
		if (!is_mapped[i]) {
			legacy_entries[i].SetVectorType(VectorType::CONSTANT_VECTOR);
			ConstantVector::SetNull(legacy_entries[i], count_t(count));
		}
	}
	bool success = true;
	for (idx_t member_idx = 0; member_idx < UnionType::GetMemberCount(source_type); member_idx++) {
		Vector member(UnionType::GetMemberType(source_type, member_idx), count);
		UnionVector::GetMember(source, member_idx, member, count);
		auto &target_member = legacy_entries[cast_data.target_indexes[member_idx] + 1];
		if (!ExecuteChildCast(cast_data.member_casts[member_idx], local_state.local_states[member_idx], member,
		                      target_member, count, parameters)) {
			success = false;
		}
	}
	FlatVector::SetSize(legacy, count);
	UnionVector::FromLegacyLayout(legacy, result, count);
	return success;
}

//! Binds a cast between unions by member name, if every member of the source exists in the target
static unique_ptr<BoundCastData> TryBindUnionByNameCast(BindCastInput &input, const LogicalType &source,
                                                        const LogicalType &target) {
	vector<idx_t> target_indexes;
	vector<BoundCastInfo> member_casts;
	auto &target_members = UnionType::GetMemberTypes(target);
	for (auto &source_member : UnionType::GetMemberTypes(source)) {
		optional_idx target_idx;
		for (idx_t i = 0; i < target_members.size(); i++) {
			if (target_members[i].first == source_member.first) {
				target_idx = i;
				break;
			}
		}
		if (!target_idx.IsValid()) {
			return nullptr;
		}
		target_indexes.push_back(target_idx.GetIndex());
		member_casts.push_back(
		    input.GetCastFunction(source_member.second, target_members[target_idx.GetIndex()].second));
	}
	return make_uniq<UnionByNameCastData>(target, std::move(target_indexes), std::move(member_casts));
}

//--------------------------------------------------------------------------------------------------
// Impossible casts: raise the bind error when executed, TRY_CAST turns it into NULL
//--------------------------------------------------------------------------------------------------
struct UnionImpossibleCastData : public BoundCastData {
	explicit UnionImpossibleCastData(string message_p) : message(std::move(message_p)) {
	}

	string message;

	unique_ptr<BoundCastData> Copy() const override {
		return make_uniq<UnionImpossibleCastData>(message);
	}
};

static bool UnionImpossibleCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	bool success = true;
	if (VectorOperations::HasNotNull(source)) {
		HandleCastError::AssignError(parameters.cast_data->Cast<UnionImpossibleCastData>().message, parameters);
		success = false;
	}
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::SetNull(result, count_t(count));
	return success;
}

BoundCastInfo DefaultCasts::ImplicitToUnionCast(BindCastInput &input, const LogicalType &source,
                                                const LogicalType &target) {
	D_ASSERT(UnionType::IsUnion(target));
	if (UnionType::IsUnion(source)) {
		auto by_name = TryBindUnionByNameCast(input, source, target);
		if (by_name) {
			return BoundCastInfo(UnionByNameCast, std::move(by_name), InitUnionByNameLocalState);
		}
	}
	if (source.id() == LogicalTypeId::VARIANT) {
		// VARIANT (or a UNION with other members) -> UNION: check that the values belong to a member
		vector<BoundCastInfo> member_casts;
		for (auto &member : UnionType::GetMemberTypes(target)) {
			member_casts.push_back(input.GetCastFunction(source, member.second));
		}
		return BoundCastInfo(VariantToUnionCast, make_uniq<VariantToUnionCastData>(target, std::move(member_casts)),
		                     InitVariantToUnionLocalState);
	}
	try {
		auto member_idx = BindToUnionMember(input, source, target);
		auto &member_type = UnionType::GetMemberType(target, member_idx);
		auto member_cast = input.GetCastFunction(source, member_type);
		input.stability = member_cast.GetStability();
		auto variant_cast = input.GetCastFunction(member_type, LogicalType::VARIANT());
		return BoundCastInfo(
		    ToUnionMemberCast,
		    make_uniq<ToUnionMemberCastData>(member_type, std::move(member_cast), std::move(variant_cast)),
		    InitToUnionMemberLocalState);
	} catch (ConversionException &ex) {
		return BoundCastInfo(UnionImpossibleCast, make_uniq<UnionImpossibleCastData>(ErrorData(ex).RawMessage()));
	}
}

} // namespace duckdb
