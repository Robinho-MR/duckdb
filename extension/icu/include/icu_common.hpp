//===----------------------------------------------------------------------===//
//                         DuckDB
//
// icu_common.hpp
//
// The few utilities the calendar and collation engines share, on the standard
// library and the stable C++ API only.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb_cpp.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#define D_ASSERT                    assert
#define DUCKDB_EXPLICIT_FALLTHROUGH [[fallthrough]]

namespace duckdb_icu {

using duckdb::cxx::idx_t;
using std::lock_guard;
using std::make_pair;
using std::mutex;
using std::pair;
using std::string;
using std::unique_ptr;
using std::unordered_map;
using std::vector;

template <class T, class... ARGS>
unique_ptr<T> make_uniq(ARGS &&...args) { // NOLINT: mimic the engine's spelling
	return unique_ptr<T>(new T(std::forward<ARGS>(args)...));
}

template <class T>
struct NumericLimits {
	static constexpr T Minimum() {
		return std::numeric_limits<T>::lowest();
	}
	static constexpr T Maximum() {
		return std::numeric_limits<T>::max();
	}
};

template <class T>
constexpr T MinValue(T a, T b) {
	return a < b ? a : b;
}

template <class T>
constexpr T MaxValue(T a, T b) {
	return a > b ? a : b;
}

struct StringUtil {
	static bool CharacterIsDigit(char c) {
		return c >= '0' && c <= '9';
	}
	static bool CharacterIsAlpha(char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
	}
	static bool CharacterIsAlphaNumeric(char c) {
		return CharacterIsAlpha(c) || CharacterIsDigit(c);
	}
	static char CharacterToLower(char c) {
		return (c >= 'A' && c <= 'Z') ? char(c + ('a' - 'A')) : c;
	}
	static string Lower(const string &str) {
		string result(str);
		for (auto &c : result) {
			c = CharacterToLower(c);
		}
		return result;
	}
	static bool Equals(const string &a, const string &b) {
		return a == b;
	}
	static bool Equals(const char *a, const char *b) {
		return strcmp(a, b) == 0;
	}
	static bool CIEquals(const string &a, const string &b) {
		if (a.size() != b.size()) {
			return false;
		}
		for (idx_t i = 0; i < a.size(); i++) {
			if (CharacterToLower(a[i]) != CharacterToLower(b[i])) {
				return false;
			}
		}
		return true;
	}
	static bool StartsWith(const string &str, const string &prefix) {
		return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
	}
	static bool CIStartsWith(const string &str, const string &prefix) {
		return str.size() >= prefix.size() && CIEquals(str.substr(0, prefix.size()), prefix);
	}
	//! Splits on every delimiter, keeping empty parts but not a trailing empty one, as the engine does
	static vector<string> Split(const string &str, char delimiter) {
		std::stringstream ss(str);
		vector<string> lines;
		string temp;
		while (std::getline(ss, temp, delimiter)) {
			lines.push_back(temp);
		}
		return lines;
	}
	//! Splits on every delimiter and drops empty parts; the input itself when nothing is left, as the engine does
	static vector<string> Split(const string &input, const string &split) {
		vector<string> splits;
		idx_t last = 0;
		while (last <= input.size()) {
			auto next = input.find(split, last);
			if (next == string::npos) {
				next = input.size();
			}
			auto part = input.substr(last, next - last);
			if (!part.empty()) {
				splits.push_back(part);
			}
			last = next + split.size();
		}
		if (splits.empty()) {
			splits.push_back(input);
		}
		return splits;
	}
	static string Join(const vector<string> &input, const string &separator) {
		string result;
		for (idx_t i = 0; i < input.size(); i++) {
			if (i > 0) {
				result += separator;
			}
			result += input[i];
		}
		return result;
	}
	//! Joins the first `count` elements, each transformed by `f`
	template <class C, class FUNC>
	static string Join(const C &input, idx_t count, const string &separator, const FUNC &f) {
		string result;
		for (idx_t i = 0; i < count; i++) {
			if (i > 0) {
				result += separator;
			}
			result += f(input[i]);
		}
		return result;
	}
	static string Replace(string source, const string &from, const string &to) {
		if (from.empty()) {
			return source;
		}
		idx_t pos = 0;
		while ((pos = source.find(from, pos)) != string::npos) {
			source.replace(pos, from.size(), to);
			pos += to.size();
		}
		return source;
	}

	//! The `n` strings most similar to `target` by Jaro-Winkler similarity, at least `threshold` similar but the first
	static vector<string> TopNJaroWinkler(const vector<string> &strings, const string &target, idx_t n = 5,
	                                      double threshold = 0.5);
	//! A newline, `candidate` and the quoted candidates, or empty without candidates
	static string CandidatesMessage(const vector<string> &candidates, const string &candidate);

	//! printf-like formatting with the engine's conventions: every %s, %d, %u, %llu, %lld or %i takes the next
	//! argument, whatever its type
	template <class... ARGS>
	static string Format(const string &fmt, ARGS &&...args) {
		vector<string> values;
		(void)std::initializer_list<int> {(values.push_back(ToString(std::forward<ARGS>(args))), 0)...};
		string result;
		idx_t next = 0;
		for (idx_t i = 0; i < fmt.size(); i++) {
			if (fmt[i] != '%' || i + 1 >= fmt.size()) {
				result += fmt[i];
				continue;
			}
			if (fmt[i + 1] == '%') {
				result += '%';
				i++;
				continue;
			}
			auto end = i + 1;
			while (end < fmt.size() && (fmt[end] == 'l' || fmt[end] == 'h' || fmt[end] == 'z')) {
				end++;
			}
			if (end < fmt.size() && strchr("sdui", fmt[end]) && next < values.size()) {
				result += values[next++];
				i = end;
				continue;
			}
			result += fmt[i];
		}
		return result;
	}

private:
	static string ToString(const string &value) {
		return value;
	}
	static string ToString(const char *value) {
		return value ? value : "";
	}
	template <class T>
	static string ToString(const T &value) {
		std::ostringstream ss;
		ss << value;
		return ss.str();
	}
};

//! The engine's exception types, raised through the stable C++ API so that they surface with the same error type
#define DUCKDB_ICU_EXCEPTION(NAME, BASE)                                                                               \
	class NAME : public duckdb::cxx::BASE {                                                                            \
	public:                                                                                                            \
		explicit NAME(const string &message) : duckdb::cxx::BASE(message) {                                            \
		}                                                                                                              \
		template <class... ARGS>                                                                                       \
		explicit NAME(const string &fmt, ARGS &&...args)                                                               \
		    : duckdb::cxx::BASE(StringUtil::Format(fmt, std::forward<ARGS>(args)...)) {                                \
		}                                                                                                              \
	}

DUCKDB_ICU_EXCEPTION(InternalException, InternalException);
DUCKDB_ICU_EXCEPTION(InvalidInputException, InvalidInputException);
DUCKDB_ICU_EXCEPTION(NotImplementedException, NotImplementedException);
DUCKDB_ICU_EXCEPTION(OutOfMemoryException, OutOfMemoryException);
DUCKDB_ICU_EXCEPTION(ConversionException, ConversionException);
DUCKDB_ICU_EXCEPTION(OutOfRangeException, OutOfRangeException);
DUCKDB_ICU_EXCEPTION(BinderException, BinderException);
DUCKDB_ICU_EXCEPTION(DataCorruptionException, DataCorruptionException);

#undef DUCKDB_ICU_EXCEPTION

} // namespace duckdb_icu
