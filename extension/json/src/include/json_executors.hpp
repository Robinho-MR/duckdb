//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_executors.hpp
//
// The exec callbacks of the JSON read functions are one line each: they pick the executor that matches the shape of
// the function and hand it the function that computes one value from the parsed document.
//
//     static std::optional<string_view> GetType(yyjson_val *val, yyjson_alc *alc) { ... }
//     static void PathExec(ScalarFunction::ExecInput &input) {
//         JSONExecutors::BinaryExecute<string_view>(input, GetType);
//     }
//
// The result type R is bool, a number, std::string_view or ListResult<E> (see json_vector.hpp). A string_view result
// is copied into the result vector right away, so it may point into the document or into the arena.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "json_functions.hpp"

namespace duckdb {
namespace json {

struct JSONExecutors {
public:
	//! Single-argument JSON read function, i.e. json_type('[1, 2, 3]')
	template <class R, class FUN>
	static void UnaryExecute(ScalarFunction::ExecInput &input, FUN &&fun) {
		auto &lstate = JSONFunctionLocalState::ResetAndGet(input);
		auto alc = lstate.json_allocator.GetYYAlc();
		const auto count = input.GetRowCount();

		auto result = input.GetResult();
		ValueView<varchar_t> inputs(input.GetArg(0));
		ResultWriter<R> writer(result, count);
		for (idx_t i = 0; i < count; i++) {
			if (!inputs.IsValid(i)) {
				writer.Write(std::nullopt);
				continue;
			}
			auto doc = JSONCommon::ReadDocument(inputs.Get(i), JSONCommon::READ_FLAG, alc);
			writer.Write(fun(doc->root, alc));
		}
		writer.Finish();
	}

	//! Two-argument JSON read function (with path query), i.e. json_type('[1, 2, 3]', '$[0]'). The path is the bind
	//! data (JSONReadFunctionData): a constant path (a wildcard path makes the result a list), or a column.
	template <class R, bool SET_NULL_IF_NOT_FOUND = true, class FUN>
	static void BinaryExecute(ScalarFunction::ExecInput &input, FUN &&fun) {
		const auto &info = input.GetBindData<JSONReadFunctionData>();
		auto &lstate = JSONFunctionLocalState::ResetAndGet(input);
		auto alc = lstate.json_allocator.GetYYAlc();
		const auto count = input.GetRowCount();

		auto result = input.GetResult();
		ValueView<varchar_t> inputs(input.GetArg(0));
		if (info.constant) { // Constant path
			const char *ptr = info.ptr;
			const idx_t &len = info.len;
			if (info.path_type == JSONCommon::JSONPathType::REGULAR) {
				ResultWriter<R> writer(result, count);
				for (idx_t i = 0; i < count; i++) {
					if (!inputs.IsValid(i)) {
						writer.Write(std::nullopt);
						continue;
					}
					auto doc = JSONCommon::ReadDocument(inputs.Get(i), JSONCommon::READ_FLAG, alc);
					// Use the path elements parsed at bind time when available
					auto val = info.use_elements ? JSONCommon::GetPathElements(doc->root, info.elements)
					                             : JSONCommon::GetUnsafe(doc->root, ptr, len);
					if (SET_NULL_IF_NOT_FOUND && !val) {
						writer.Write(std::nullopt);
					} else {
						writer.Write(fun(val, alc));
					}
				}
				writer.Finish();
			} else {
				D_ASSERT(info.path_type == JSONCommon::JSONPathType::WILDCARD);
				vector<yyjson_val *> vals;
				ResultWriter<ListResult<R>> writer(result, count);
				for (idx_t i = 0; i < count; i++) {
					if (!inputs.IsValid(i)) {
						writer.Write(std::optional<ListResult<R>>());
						continue;
					}
					vals.clear();
					auto doc = JSONCommon::ReadDocument(inputs.Get(i), JSONCommon::READ_FLAG, alc);
					JSONCommon::GetWildcardPath(doc->root, ptr, len, vals);

					ListResult<R> list;
					list.items.reserve(vals.size());
					for (auto &val : vals) {
						D_ASSERT(val != nullptr); // Wildcard extract shouldn't give back nullptrs
						list.items.push_back(fun(val, alc));
					}
					writer.Write(std::move(list));
				}
				writer.Finish();
			}
		} else { // Columnref path
			D_ASSERT(info.path_type == JSONCommon::JSONPathType::REGULAR);
			ResultWriter<R> writer(result, count);
			auto path_vector = input.GetArg(1);
			ValueView<varchar_t> string_paths(path_vector);
			ValueView<int64_t> integral_paths(path_vector);
			for (idx_t i = 0; i < count; i++) {
				const bool path_valid = info.integral ? integral_paths.IsValid(i) : string_paths.IsValid(i);
				if (!inputs.IsValid(i) || !path_valid) {
					writer.Write(std::nullopt);
					continue;
				}
				auto doc = JSONCommon::ReadDocument(inputs.Get(i), JSONCommon::READ_FLAG, alc);
				yyjson_val *val;
				if (info.integral) {
					val = JSONCommon::Get(doc->root, std::to_string(integral_paths.Get(i)), true);
				} else {
					auto path = string_paths.Get(i);
					val = JSONCommon::Get(doc->root, string_view(path.data(), path.size()), false);
				}
				if (SET_NULL_IF_NOT_FOUND && !val) {
					writer.Write(std::nullopt);
				} else {
					writer.Write(fun(val, alc));
				}
			}
			writer.Finish();
		}
	}

	//! JSON read function with list of path queries, i.e. json_type('[1, 2, 3]', ['$[0]', '$[1]'])
	template <class R, bool SET_NULL_IF_NOT_FOUND = true, class FUN>
	static void ExecuteMany(ScalarFunction::ExecInput &input, FUN &&fun) {
		const auto &info = input.GetBindData<JSONReadManyFunctionData>();
		auto &lstate = JSONFunctionLocalState::ResetAndGet(input);
		auto alc = lstate.json_allocator.GetYYAlc();
		D_ASSERT(info.ptrs.size() == info.lens.size());

		const auto count = input.GetRowCount();
		const idx_t num_paths = info.ptrs.size();

		auto result = input.GetResult();
		ValueView<varchar_t> inputs(input.GetArg(0));
		ResultWriter<ListResult<R>> writer(result, count);
		for (idx_t i = 0; i < count; i++) {
			if (!inputs.IsValid(i)) {
				writer.Write(std::optional<ListResult<R>>());
				continue;
			}
			auto doc = JSONCommon::ReadDocument(inputs.Get(i), JSONCommon::READ_FLAG, alc);
			ListResult<R> list;
			list.items.reserve(num_paths);
			for (idx_t path_i = 0; path_i < num_paths; path_i++) {
				auto val = info.use_elements[path_i]
				               ? JSONCommon::GetPathElements(doc->root, info.elements[path_i])
				               : JSONCommon::GetUnsafe(doc->root, info.ptrs[path_i], info.lens[path_i]);
				if (SET_NULL_IF_NOT_FOUND && !val) {
					list.items.emplace_back(std::nullopt);
				} else {
					list.items.push_back(fun(val, alc));
				}
			}
			writer.Write(std::move(list));
		}
		writer.Finish();
	}
};

} // namespace json
} // namespace duckdb
