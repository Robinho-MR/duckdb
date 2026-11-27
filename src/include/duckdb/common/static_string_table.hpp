//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/static_string_table.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/string.hpp"

namespace duckdb {

//! A view over a constexpr array of string literals. It is trivially destructible and constructed at compile time, so
//! unlike a static set it needs no initialization, no lock and no NoDestructor.
//!
//!   static constexpr const char *const KEYWORDS[] = {"SELECT", "FROM"};
//!   return StaticStringSet(KEYWORDS).Contains(word);
class StaticStringSet {
public:
	template <idx_t N>
	constexpr StaticStringSet(const char *const (&entries_p)[N]) // NOLINT: allow implicit conversion from an array
	    : entries(entries_p), count(N) {
	}

public:
	constexpr const char *const *begin() const { // NOLINT: match stl API
		return entries;
	}
	constexpr const char *const *end() const { // NOLINT: match stl API
		return entries + count;
	}
	constexpr idx_t size() const { // NOLINT: match stl API
		return count;
	}

	//! Whether any entry compares equal to the value with ==
	template <class T>
	bool Contains(const T &value) const {
		for (auto entry : *this) {
			if (value == entry) {
				return true;
			}
		}
		return false;
	}
	//! Whether any entry equals the value, ignoring ASCII case
	bool ContainsCaseInsensitive(const string &value) const {
		for (auto entry : *this) {
			if (EqualsCaseInsensitive(entry, value)) {
				return true;
			}
		}
		return false;
	}

	static bool EqualsCaseInsensitive(const char *literal, const string &value) {
		idx_t i = 0;
		for (; literal[i] != '\0'; i++) {
			if (i >= value.size() || ToLower(literal[i]) != ToLower(value[i])) {
				return false;
			}
		}
		return i == value.size();
	}

private:
	static constexpr char ToLower(char c) {
		return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
	}

private:
	const char *const *entries;
	idx_t count;
};

//! A view over a constexpr array of (string literal, value) pairs, see StaticStringSet.
template <class T>
class StaticStringMap {
public:
	struct Entry {
		const char *key;
		T value;
	};

public:
	template <idx_t N>
	constexpr StaticStringMap(const Entry (&entries_p)[N]) // NOLINT: allow implicit conversion from an array
	    : entries(entries_p), count(N) {
	}

public:
	constexpr const Entry *begin() const { // NOLINT: match stl API
		return entries;
	}
	constexpr const Entry *end() const { // NOLINT: match stl API
		return entries + count;
	}

	//! The value of the entry whose key equals the argument, if any
	optional_ptr<const T> Find(const string &key) const {
		for (auto &entry : *this) {
			if (key == entry.key) {
				return entry.value;
			}
		}
		return nullptr;
	}
	//! The value of the entry whose key equals the argument ignoring ASCII case, if any
	optional_ptr<const T> FindCaseInsensitive(const string &key) const {
		for (auto &entry : *this) {
			if (StaticStringSet::EqualsCaseInsensitive(entry.key, key)) {
				return entry.value;
			}
		}
		return nullptr;
	}

private:
	const Entry *entries;
	idx_t count;
};

} // namespace duckdb
