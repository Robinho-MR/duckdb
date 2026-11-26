//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/identifier_lookup_map.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/assert.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/vector.hpp"

#include <algorithm>
#include <functional>

namespace duckdb {

//! Result of resolving a name in an IdentifierLookupMap
enum class IdentifierMatch : uint8_t {
	//! No key matches the name, case-sensitively or case-insensitively
	NOT_FOUND,
	//! The name matches a key exactly, or case-insensitively matches exactly one key
	FOUND,
	//! The name has no exact match and case-insensitively matches multiple keys
	AMBIGUOUS
};

struct IdentifierExactHash {
	hash_t operator()(const Identifier &id) const {
		return std::hash<string>()(id.GetIdentifierName());
	}
};

struct IdentifierExactEquality {
	bool operator()(const Identifier &a, const Identifier &b) const {
		return a.GetIdentifierName() == b.GetIdentifierName();
	}
};

//! A map keyed by identifiers that are case-insensitive unless ambiguous.
//! Keys are stored case-sensitively: "col" and "COL" can both be present. A lookup returns the exact
//! (case-sensitive) match if there is one, otherwise the case-insensitive match if there is exactly one.
//! If a name only matches multiple keys case-insensitively (e.g. "cOl" with both "col" and "COL"), it is ambiguous.
template <class T>
class IdentifierLookupMap {
private:
	using map_t = unordered_map<Identifier, T, IdentifierExactHash, IdentifierExactEquality>;

public:
	using iterator = typename map_t::iterator;
	using const_iterator = typename map_t::const_iterator;

public:
	//! Resolve a name: exact match first, otherwise a unique case-insensitive match
	IdentifierMatch Lookup(const Identifier &name, const_iterator &result) const {
		result = entries.find(name);
		if (result != entries.end()) {
			return IdentifierMatch::FOUND;
		}
		auto folded_entry = folded.find(name);
		if (folded_entry == folded.end()) {
			return IdentifierMatch::NOT_FOUND;
		}
		if (folded_entry->second.size() > 1) {
			return IdentifierMatch::AMBIGUOUS;
		}
		D_ASSERT(folded_entry->second.size() == 1);
		result = entries.find(folded_entry->second[0]);
		D_ASSERT(result != entries.end());
		return IdentifierMatch::FOUND;
	}
	IdentifierMatch Lookup(const Identifier &name) const {
		const_iterator result;
		return Lookup(name, result);
	}
	//! Resolve a name, returning end() if it is not found or ambiguous
	const_iterator find(const Identifier &name) const { // NOLINT: match stl API
		const_iterator result;
		return Lookup(name, result) == IdentifierMatch::FOUND ? result : entries.end();
	}
	iterator find(const Identifier &name) { // NOLINT: match stl API
		const_iterator result;
		if (Lookup(name, result) != IdentifierMatch::FOUND) {
			return entries.end();
		}
		return entries.find(result->first);
	}
	//! Find a key that matches the name exactly (case-sensitively)
	const_iterator FindExact(const Identifier &name) const {
		return entries.find(name);
	}
	//! Whether the name resolves to a key
	bool Contains(const Identifier &name) const {
		return Lookup(name) == IdentifierMatch::FOUND;
	}
	//! Whether the name matches a key exactly (case-sensitively)
	bool ContainsExact(const Identifier &name) const {
		return entries.find(name) != entries.end();
	}
	//! Whether any key matches the name case-insensitively
	bool ContainsCaseInsensitive(const Identifier &name) const {
		return folded.find(name) != folded.end();
	}
	//! All keys that match the name case-insensitively, in insertion order
	vector<Identifier> GetCaseInsensitiveMatches(const Identifier &name) const {
		auto folded_entry = folded.find(name);
		if (folded_entry == folded.end()) {
			return vector<Identifier>();
		}
		return folded_entry->second;
	}

	//! Insert a key; returns false (and leaves the map unchanged) if the exact key already exists
	bool Insert(const Identifier &name, T value) {
		auto result = entries.emplace(name, std::move(value));
		if (!result.second) {
			return false;
		}
		folded[name].push_back(name);
		return true;
	}
	//! Get the value for the exact key, inserting a default-constructed value if it does not exist
	T &GetOrInsert(const Identifier &name) {
		auto entry = entries.find(name);
		if (entry == entries.end()) {
			Insert(name, T());
			entry = entries.find(name);
		}
		return entry->second;
	}
	//! Insert or overwrite the value for the exact key
	void Set(const Identifier &name, T value) {
		auto entry = entries.find(name);
		if (entry != entries.end()) {
			entry->second = std::move(value);
			return;
		}
		Insert(name, std::move(value));
	}
	//! Erase the exact key; returns whether it was present
	bool Erase(const Identifier &name) {
		if (entries.erase(name) == 0) {
			return false;
		}
		auto folded_entry = folded.find(name);
		D_ASSERT(folded_entry != folded.end());
		auto &names = folded_entry->second;
		names.erase(std::find_if(names.begin(), names.end(), [&](const Identifier &entry) {
			return entry.GetIdentifierName() == name.GetIdentifierName();
		}));
		if (names.empty()) {
			folded.erase(folded_entry);
		}
		return true;
	}
	void clear() { // NOLINT: match stl API
		entries.clear();
		folded.clear();
	}

	idx_t size() const { // NOLINT: match stl API
		return entries.size();
	}
	bool empty() const { // NOLINT: match stl API
		return entries.empty();
	}
	iterator begin() { // NOLINT: match stl API
		return entries.begin();
	}
	iterator end() { // NOLINT: match stl API
		return entries.end();
	}
	const_iterator begin() const { // NOLINT: match stl API
		return entries.begin();
	}
	const_iterator end() const { // NOLINT: match stl API
		return entries.end();
	}

private:
	//! Exact (case-sensitive) key -> value
	map_t entries;
	//! Case-insensitive key -> the exact keys that fold to it, in insertion order
	identifier_map_t<vector<Identifier>> folded;
};

//! Resolve a name among a list of names: the exact match if there is one, otherwise the unique case-insensitive match
IdentifierMatch ResolveIdentifier(const vector<Identifier> &names, const Identifier &name, idx_t &index);
//! Whether "name" refers to "target" when resolved among "names" (see ResolveIdentifier)
bool IdentifierRefersTo(const vector<Identifier> &names, const Identifier &name, const Identifier &target);

//! A set of identifiers that are case-insensitive unless ambiguous (see IdentifierLookupMap)
class IdentifierLookupSet {
public:
	IdentifierMatch Lookup(const Identifier &name) const {
		return map.Lookup(name);
	}
	bool Contains(const Identifier &name) const {
		return map.Contains(name);
	}
	bool ContainsExact(const Identifier &name) const {
		return map.ContainsExact(name);
	}
	//! Insert a name; returns false if the exact name already exists
	bool Insert(const Identifier &name) {
		return map.Insert(name, true);
	}
	idx_t size() const { // NOLINT: match stl API
		return map.size();
	}
	bool empty() const { // NOLINT: match stl API
		return map.empty();
	}

private:
	IdentifierLookupMap<bool> map;
};

} // namespace duckdb
