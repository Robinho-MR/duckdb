//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_vector.hpp
//
// Typed access to the rows of a vector, on top of the stable C++ API's Vector / VectorView.
//
//  * ValueView<T> reads rows of any layout (FLAT, CONSTANT, DICTIONARY), optionally through a window (`offset`), which
//    is how the elements of a list are read without gathering them
//  * FlatWriter<T> / StringWriter fill the first `count` rows of an output vector in order
//  * ResultWriter<R> is the writer the executors use: R is bool, a number, std::string_view (a string, copied into the
//    vector) or ListResult<E> (a list of E, nesting works)
//
//===----------------------------------------------------------------------===//

#pragma once

#include "json_common.hpp"

namespace duckdb {
namespace json {

//! Read access to the rows of a vector. T is the C++ type of the vector's elements (bool, int64_t, double, varchar_t,
//! list_entry_t, ...): see the primitive types in duckdb_cpp.hpp. For nested vectors (LIST, STRUCT, ...) it reads the
//! parent rows (list_entry_t for a LIST, none for a STRUCT: use IsValid only, T = uint8_t); the children are separate
//! vectors, reached with Vector::GetChild.
template <class T>
class ValueView {
public:
	class Entry {
	public:
		Entry(bool valid_p, idx_t index_p, const T &value_p) : valid(valid_p), index(index_p), value(value_p) {
		}
		bool IsValid() const {
			return valid;
		}
		const T &GetValue() const {
			D_ASSERT(valid);
			return value;
		}
		//! The row of the view this entry is
		idx_t GetIndex() const {
			return index;
		}

	private:
		bool valid;
		idx_t index;
		T value;
	};

	//! Reads rows [offset, offset + count) of the vector as rows [0, count) of the view
	explicit ValueView(const Vector &vector, idx_t offset = 0) : offset(offset) {
		if (vector.GetVectorType() == cxx::VectorType::OTHER) {
			vector.Flatten();
		}
		view = vector.GetView();
	}

	//! Whether row `i` holds a value
	bool IsValid(idx_t i) const {
		return view.IsValid(offset + i);
	}
	//! The value of row `i`, which must hold a value
	T Get(idx_t i) const {
		D_ASSERT(IsValid(i));
		return view.template Data<T>()[view.SelAt(offset + i)];
	}
	Entry operator[](idx_t i) const {
		if (!IsValid(i)) {
			return Entry(false, i, T());
		}
		return Entry(true, i, Get(i));
	}
	//! Where row `i` lives in the vector's element array: the index of the matching row of a child vector
	idx_t SourceRow(idx_t i) const {
		return view.SelAt(offset + i);
	}
	//! Whether every row of the vector holds a value
	bool AllValid() const {
		return view.AllValid();
	}

private:
	cxx::VectorView view;
	idx_t offset;
};

//! The NULLs of a vector that is being written. The validity mask is only allocated once a NULL is written: a vector
//! without one is known to hold only values, which is much cheaper for whatever reads it afterwards.
class LazyValidity {
public:
	LazyValidity(Vector &vector_p, idx_t count_p) : vector(vector_p), count(count_p) {
		// A vector that was used before may still carry the NULLs of that use
		if (vector.GetView().validity) {
			vector.GetValidityMutable().SetAllValid(count);
		}
	}

	void SetInvalid(idx_t row) {
		if (!fetched) {
			mask = vector.GetValidityMutable();
			mask.SetAllValid(count);
			fetched = true;
		}
		mask.SetInvalid(row);
	}

private:
	Vector &vector;
	idx_t count;
	bool fetched = false;
	cxx::ValidityMask mask {nullptr};
};

//! Gives a vector the number of rows that are going to be written to it
inline Vector &Sized(Vector &vector, idx_t count) {
	vector.SetSize(count);
	return vector;
}

//! Writes the first `count` rows of a vector, in order. Takes care of the vector's size.
template <class T>
class FlatWriter {
public:
	FlatWriter(Vector &vector_p, idx_t count_p) : count(count_p), validity(Sized(vector_p, count_p), count_p) {
		data = vector_p.GetDataMutable<T>();
	}

	void WriteValue(const T &value) {
		D_ASSERT(row < count);
		data[row++] = value;
	}
	//! Writes a NULL row; `placeholder` is what the row's element holds (a list needs a sensible offset)
	void WriteNull(const T &placeholder = T()) {
		D_ASSERT(row < count);
		data[row] = placeholder;
		validity.SetInvalid(row);
		row++;
	}
	//! The row the next write lands in
	idx_t Row() const {
		return row;
	}

private:
	idx_t count;
	idx_t row = 0;
	T *data;
	LazyValidity validity;
};

//! Writes the strings of a vector. The bytes are copied into the vector's own string heap: nothing the caller passes
//! has to outlive the call. The text is NOT validated as UTF-8 (everything written by the extension comes from
//! yyjson, which has validated it); use WriteValidated for text from elsewhere.
class StringWriter {
public:
	StringWriter(Vector &vector_p, idx_t count_p) : count(count_p), validity(Sized(vector_p, count_p), count_p) {
		heap.emplace(vector_p.GetHeap());
		values = vector_p.GetDataMutable<varchar_t>();
	}

	void WriteValue(string_view text) {
		D_ASSERT(row < count);
		values[row++] = heap->AddStringUnsafe(text);
	}
	void WriteValidated(string_view text) {
		D_ASSERT(row < count);
		values[row++] = heap->AddString(text);
	}
	void WriteNull() {
		D_ASSERT(row < count);
		values[row] = varchar_t();
		validity.SetInvalid(row++);
	}
	idx_t Row() const {
		return row;
	}

private:
	idx_t count;
	idx_t row = 0;
	LazyValidity validity;
	std::optional<cxx::Arena> heap;
	varchar_t *values = nullptr;
};

//===--------------------------------------------------------------------===//
// ResultWriter
//===--------------------------------------------------------------------===//
//! A list as a function result: nullopt items are NULL elements
template <class E>
struct ListResult {
	vector<std::optional<E>> items;
};

//! Writes one result row per Write call, in order, for the first `count` rows of `vector`. Call Finish() when done.
//! R = bool or a number: written as is. R = string_view: copied into the vector. R = ListResult<E>: a list; the
//! elements are kept until Finish(), so string_views in them have to stay valid until then.
template <class R, class = void>
class ResultWriter {
public:
	ResultWriter(Vector &vector, idx_t count) : writer(vector, count) {
	}
	void Write(const std::optional<R> &value) {
		if (value.has_value()) {
			writer.WriteValue(*value);
		} else {
			writer.WriteNull();
		}
	}
	void Finish() {
	}

private:
	FlatWriter<R> writer;
};

template <>
class ResultWriter<string_view> {
public:
	ResultWriter(Vector &vector, idx_t count) : writer(vector, count) {
	}
	void Write(const std::optional<string_view> &value) {
		if (value.has_value()) {
			writer.WriteValue(*value);
		} else {
			writer.WriteNull();
		}
	}
	void Finish() {
	}

private:
	StringWriter writer;
};

template <class E>
class ResultWriter<ListResult<E>> {
public:
	ResultWriter(Vector &vector_p, idx_t count_p) : vec(vector_p), writer(vector_p, count_p) {
	}
	void Write(const std::optional<ListResult<E>> &value) {
		if (!value.has_value()) {
			writer.WriteNull(list_entry_t {items.size(), 0});
			return;
		}
		writer.WriteValue(list_entry_t {items.size(), value->items.size()});
		items.insert(items.end(), value->items.begin(), value->items.end());
	}
	void Write(ListResult<E> &&value) {
		writer.WriteValue(list_entry_t {items.size(), value.items.size()});
		items.insert(items.end(), std::make_move_iterator(value.items.begin()),
		             std::make_move_iterator(value.items.end()));
	}
	//! Writes the elements of all rows into the child vector
	void Finish() {
		auto child = vec.GetChild(0);
		ResultWriter<E> child_writer(child, items.size());
		for (auto &item : items) {
			child_writer.Write(item);
		}
		child_writer.Finish();
	}

private:
	Vector &vec;
	FlatWriter<list_entry_t> writer;
	vector<std::optional<E>> items;
};

} // namespace json
} // namespace duckdb
