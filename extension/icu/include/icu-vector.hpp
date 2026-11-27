//===----------------------------------------------------------------------===//
//                         DuckDB
//
// icu-vector.hpp
//
// Reading the arguments and writing the results of the ICU functions, on top
// of the vector views of the stable C++ API.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb_cpp.hpp"
#include "icu_common.hpp"

#include <tuple>

namespace duckdb_icu {

namespace cxx = duckdb::cxx;

//! Writes the rows of a FLAT result vector, NULLs included
template <class T>
class ResultWriter {
public:
	ResultWriter(cxx::Vector &result, idx_t count)
	    : result(result), count(count), data(result.template GetDataMutable<T>()) {
	}

	void Set(idx_t row, const T &value) {
		data[row] = value;
	}
	void SetNull(idx_t row) {
		Mask().SetInvalid(row);
	}

private:
	cxx::ValidityMask &Mask() {
		if (!mask.words) {
			mask = result.GetValidityMutable();
			mask.SetAllValid(count);
		}
		return mask;
	}

	cxx::Vector &result;
	idx_t count;
	T *data;
	cxx::ValidityMask mask {nullptr};
};

//! Writes the rows of a FLAT VARCHAR result vector, NULLs included
class StringResultWriter {
public:
	StringResultWriter(cxx::Vector &result, idx_t count) : result(result), count(count) {
	}

	void Set(idx_t row, std::string_view value) {
		result.AssignStringUnsafe(row, value);
	}
	void SetNull(idx_t row) {
		Mask().SetInvalid(row);
	}

private:
	cxx::ValidityMask &Mask() {
		if (!mask.words) {
			mask = result.GetValidityMutable();
			mask.SetAllValid(count);
		}
		return mask;
	}

	cxx::Vector &result;
	idx_t count;
	cxx::ValidityMask mask {nullptr};
};

//! A view of a vector, flattening a representation a view cannot read first, e.g. a sequence or FSST strings
inline cxx::VectorView ReadableView(const cxx::Vector &vector) {
	if (vector.GetVectorType() == cxx::VectorType::OTHER) {
		vector.Flatten();
	}
	return vector.GetView();
}

//! Reads the rows of an argument vector of a primitive type
template <class T>
class ArgReader {
public:
	explicit ArgReader(const cxx::Vector &vector) : view(ReadableView(vector)) {
	}

	bool IsValid(idx_t row) const {
		return view.IsValid(row);
	}
	T Get(idx_t row) const {
		return view.Data<T>()[view.SelAt(row)];
	}

	cxx::VectorView view;
};

//! Reads the rows of a VARCHAR argument vector
class StringArgReader {
public:
	explicit StringArgReader(const cxx::Vector &vector) : view(ReadableView(vector)) {
	}

	bool IsValid(idx_t row) const {
		return view.IsValid(row);
	}
	std::string_view Get(idx_t row) const {
		return view.Data<cxx::varchar_t>()[view.SelAt(row)].view();
	}

	cxx::VectorView view;
};

//! The reader of an argument type: string_view arguments read VARCHAR vectors
template <class T>
struct ArgReaderOf {
	using type = ArgReader<T>;
};
template <>
struct ArgReaderOf<std::string_view> {
	using type = StringArgReader;
};

//! Whether an argument vector holds a single value for all rows
inline bool IsConstant(const cxx::Vector &vector) {
	return vector.GetVectorType() == cxx::VectorType::CONSTANT;
}

//! Invokes `fun(row, args...)` for every row where no argument is NULL, and `null_fun(row)` for the others. The
//! argument types are primitives, or std::string_view for VARCHAR.
template <class... ARGS, class FUN, class NULL_FUN, size_t... Is>
void ForEachRowImpl(cxx::ScalarFunction::ExecInput &input, FUN &&fun, NULL_FUN &&null_fun, std::index_sequence<Is...>) {
	std::tuple<typename ArgReaderOf<ARGS>::type...> readers {typename ArgReaderOf<ARGS>::type(input.GetArg(Is))...};
	const auto count = input.GetRowCount();
	for (idx_t row = 0; row < count; row++) {
		if (!(... && std::get<Is>(readers).IsValid(row))) {
			null_fun(row);
			continue;
		}
		fun(row, std::get<Is>(readers).Get(row)...);
	}
}

template <class... ARGS, class FUN, class NULL_FUN>
void ForEachRow(cxx::ScalarFunction::ExecInput &input, FUN &&fun, NULL_FUN &&null_fun) {
	if (input.GetArgCount() != sizeof...(ARGS)) {
		throw InternalException("Expected %d arguments, got %d", int(sizeof...(ARGS)), int(input.GetArgCount()));
	}
	ForEachRowImpl<ARGS...>(input, std::forward<FUN>(fun), std::forward<NULL_FUN>(null_fun),
	                        std::index_sequence_for<ARGS...> {});
}

//! Computes a primitive result per row: `fun(args...)` returns the value, or nullopt for NULL
template <class RESULT, class... ARGS, class FUN>
void ExecuteRows(cxx::ScalarFunction::ExecInput &input, FUN &&fun) {
	auto result = input.GetResult();
	ResultWriter<RESULT> writer(result, input.GetRowCount());
	ForEachRow<ARGS...>(
	    input,
	    [&](idx_t row, ARGS... args) {
		    std::optional<RESULT> value = fun(args...);
		    if (value) {
			    writer.Set(row, *value);
		    } else {
			    writer.SetNull(row);
		    }
	    },
	    [&](idx_t row) { writer.SetNull(row); });
}

//! Computes a VARCHAR result per row: `fun(out, args...)` writes the text into `out` and returns true, or returns
//! false for NULL
template <class... ARGS, class FUN>
void ExecuteStringRows(cxx::ScalarFunction::ExecInput &input, FUN &&fun) {
	auto result = input.GetResult();
	StringResultWriter writer(result, input.GetRowCount());
	string buffer;
	ForEachRow<ARGS...>(
	    input,
	    [&](idx_t row, ARGS... args) {
		    buffer.clear();
		    if (fun(buffer, args...)) {
			    writer.Set(row, buffer);
		    } else {
			    writer.SetNull(row);
		    }
	    },
	    [&](idx_t row) { writer.SetNull(row); });
}

//! Writes the rows of a FLAT LIST result vector of a primitive child type, NULLs included
template <class T>
class ListResultWriter {
public:
	ListResultWriter(cxx::Vector &result_p, idx_t count)
	    : result(result_p), child(result_p.GetChild(0)), entries(result_p.GetDataMutable<cxx::list_entry_t>()),
	      writer(result_p, count) {
	}

	//! Starts the list of `row`, which ends with the next StartList or Finish
	void StartList(idx_t row) {
		entries[row].offset = size;
		entries[row].length = 0;
		current = &entries[row];
	}
	void Append(const T &value) {
		if (size == capacity) {
			capacity = MaxValue<idx_t>(capacity * 2, 2048);
			child.SetSize(capacity);
			data = child.GetDataMutable<T>();
		}
		data[size++] = value;
		current->length++;
	}
	void SetNull(idx_t row) {
		entries[row].offset = size;
		entries[row].length = 0;
		writer.SetNull(row);
	}
	void Finish() {
		child.SetSize(size);
	}

private:
	cxx::Vector &result;
	cxx::Vector child;
	cxx::list_entry_t *entries;
	ResultWriter<cxx::list_entry_t> writer;
	cxx::list_entry_t *current = nullptr;
	T *data = nullptr;
	idx_t size = 0;
	idx_t capacity = 0;
};

} // namespace duckdb_icu
