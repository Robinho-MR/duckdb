//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/identifier_container.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

// included from duckdb/common/identifier.hpp - Identifier is defined at this point

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <stdexcept>

namespace duckdb {

//! Result of resolving a name among a set of identifiers
enum class IdentifierMatch : uint8_t {
	//! No identifier matches the name, case-sensitively or case-insensitively
	NOT_FOUND,
	//! The name matches an identifier exactly, or case-insensitively matches exactly one identifier
	FOUND,
	//! The name has no exact match and case-insensitively matches multiple identifiers
	AMBIGUOUS
};

//! Exact (case-sensitive) hashing / comparison of identifiers - used for the keys stored in identifier containers
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

//! Case-insensitive ordering, with ties between names that only differ in case broken by their exact value
struct IdentifierExactCompare {
	bool operator()(const Identifier &a, const Identifier &b) const {
		if (a < b) {
			return true;
		}
		if (b < a) {
			return false;
		}
		return a.GetIdentifierName() < b.GetIdentifierName();
	}
};

//! Index from a case-insensitive name to the exact keys of a container that fold to it, in insertion order
class IdentifierFoldIndex {
public:
	void Add(const Identifier &name) {
		index[name].push_back(name);
	}
	void Remove(const Identifier &name) {
		auto entry = index.find(name);
		if (entry == index.end()) {
			return;
		}
		auto &names = entry->second;
		for (auto it = names.begin(); it != names.end(); ++it) {
			if (it->GetIdentifierName() == name.GetIdentifierName()) {
				names.erase(it);
				break;
			}
		}
		if (names.empty()) {
			index.erase(entry);
		}
	}
	//! The exact keys that the name matches case-insensitively (or nullptr)
	const vector<Identifier> *Find(const Identifier &name) const {
		auto entry = index.find(name);
		return entry == index.end() ? nullptr : &entry->second;
	}
	void Clear() {
		index.clear();
	}

private:
	unordered_map<Identifier, vector<Identifier>, IdentifierHashFunction, IdentifierEquality> index;
};

//! Shared lookup logic of the identifier containers: keys are stored exactly (so "col" and "COL" can both be present),
//! and a name resolves to the exact key if there is one, otherwise to the unique case-insensitive match.
template <class CONTAINER, class KEY_OF>
class IdentifierContainerBase {
public:
	using iterator = typename CONTAINER::iterator;
	using const_iterator = typename CONTAINER::const_iterator;
	using size_type = idx_t;

public:
	//! Resolve a name: the exact key, otherwise the unique case-insensitive match
	IdentifierMatch Lookup(const Identifier &name, const_iterator &result) const {
		result = entries.find(name);
		if (result != entries.end()) {
			return IdentifierMatch::FOUND;
		}
		auto names = fold_index.Find(name);
		if (!names) {
			return IdentifierMatch::NOT_FOUND;
		}
		if (names->size() > 1) {
			return IdentifierMatch::AMBIGUOUS;
		}
		result = entries.find((*names)[0]);
		return IdentifierMatch::FOUND;
	}
	IdentifierMatch Lookup(const Identifier &name) const {
		const_iterator result;
		return Lookup(name, result);
	}
	//! Resolve a name - returns end() if it is not found or ambiguous
	const_iterator find(const Identifier &name) const { // NOLINT: match stl API
		const_iterator result;
		return Lookup(name, result) == IdentifierMatch::FOUND ? result : entries.end();
	}
	iterator find(const Identifier &name) { // NOLINT: match stl API
		auto entry = entries.find(name);
		if (entry != entries.end()) {
			return entry;
		}
		auto names = fold_index.Find(name);
		if (!names || names->size() != 1) {
			return entries.end();
		}
		return entries.find((*names)[0]);
	}
	idx_t count(const Identifier &name) const { // NOLINT: match stl API
		return find(name) == entries.end() ? 0 : 1;
	}
	//! Find the key that matches the name exactly (case-sensitively)
	const_iterator FindExact(const Identifier &name) const {
		return entries.find(name);
	}
	iterator FindExact(const Identifier &name) {
		return entries.find(name);
	}
	bool Contains(const Identifier &name) const {
		return count(name) > 0;
	}
	bool ContainsExact(const Identifier &name) const {
		return entries.find(name) != entries.end();
	}
	//! Whether any key matches the name case-insensitively
	bool ContainsCaseInsensitive(const Identifier &name) const {
		return fold_index.Find(name) != nullptr;
	}
	//! All keys that match the name case-insensitively, in insertion order
	vector<Identifier> GetCaseInsensitiveMatches(const Identifier &name) const {
		auto names = fold_index.Find(name);
		return names ? *names : vector<Identifier>();
	}

	//! Erase the key the name resolves to; returns the number of erased keys
	idx_t erase(const Identifier &name) { // NOLINT: match stl API
		auto entry = find(name);
		if (entry == entries.end()) {
			return 0;
		}
		erase(entry);
		return 1;
	}
	//! Erase the exact key; returns whether it was present
	bool EraseExact(const Identifier &name) {
		auto entry = entries.find(name);
		if (entry == entries.end()) {
			return false;
		}
		erase(const_iterator(entry));
		return true;
	}
	iterator erase(const_iterator entry) { // NOLINT: match stl API
		fold_index.Remove(KEY_OF()(*entry));
		return entries.erase(entry);
	}
	void clear() { // NOLINT: match stl API
		entries.clear();
		fold_index.Clear();
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
	//! Range lookups on the exact keys (only for ordered containers)
	iterator lower_bound(const Identifier &name) { // NOLINT: match stl API
		return entries.lower_bound(name);
	}
	iterator upper_bound(const Identifier &name) { // NOLINT: match stl API
		return entries.upper_bound(name);
	}
	bool operator==(const IdentifierContainerBase &other) const {
		return entries == other.entries;
	}
	bool operator!=(const IdentifierContainerBase &other) const {
		return !(*this == other);
	}

protected:
	template <class... ARGS>
	std::pair<iterator, bool> InsertExact(ARGS &&...args) {
		auto result = entries.emplace(std::forward<ARGS>(args)...);
		if (result.second) {
			fold_index.Add(KEY_OF()(*result.first));
		}
		return result;
	}

protected:
	//! The entries, keyed by their exact name
	CONTAINER entries;
	//! Case-insensitive name -> exact keys
	IdentifierFoldIndex fold_index;
};

struct IdentifierPairKey {
	template <class PAIR>
	const Identifier &operator()(const PAIR &entry) const {
		return entry.first;
	}
};

struct IdentifierSelfKey {
	const Identifier &operator()(const Identifier &entry) const {
		return entry;
	}
};

//! A map keyed by identifiers that are case-insensitive unless ambiguous.
//! Keys are stored exactly: "col" and "COL" can both be present. Lookups (find/count/at/erase) return the exact match
//! if there is one, otherwise the case-insensitive match if there is exactly one. insert/emplace add exact keys.
//! operator[] returns the entry the name resolves to, and only inserts the exact key if the name does not resolve.
template <class T, class CONTAINER>
class IdentifierMap : public IdentifierContainerBase<CONTAINER, IdentifierPairKey> {
	using base = IdentifierContainerBase<CONTAINER, IdentifierPairKey>;

public:
	using key_type = Identifier;
	using mapped_type = T;
	using value_type = typename CONTAINER::value_type;
	using iterator = typename base::iterator;
	using const_iterator = typename base::const_iterator;

public:
	IdentifierMap() = default;
	IdentifierMap(std::initializer_list<value_type> values) { // NOLINT: allow implicit construction like std::map
		for (auto &value : values) {
			insert(value);
		}
	}
	template <class INPUT_ITERATOR>
	IdentifierMap(INPUT_ITERATOR first, INPUT_ITERATOR last) {
		insert(first, last);
	}

	T &operator[](const Identifier &name) {
		auto entry = base::find(name);
		if (entry != base::end()) {
			return entry->second;
		}
		return base::InsertExact(name, T()).first->second;
	}
	T &at(const Identifier &name) { // NOLINT: match stl API
		auto entry = base::find(name);
		if (entry == base::end()) {
			throw std::out_of_range("IdentifierMap::at");
		}
		return entry->second;
	}
	const T &at(const Identifier &name) const { // NOLINT: match stl API
		auto entry = base::find(name);
		if (entry == base::end()) {
			throw std::out_of_range("IdentifierMap::at");
		}
		return entry->second;
	}
	//! Insert the exact key - returns false (and leaves the map unchanged) if the exact key already exists
	std::pair<iterator, bool> insert(const value_type &value) { // NOLINT: match stl API
		return base::InsertExact(value);
	}
	std::pair<iterator, bool> insert(value_type &&value) { // NOLINT: match stl API
		return base::InsertExact(std::move(value));
	}
	template <class PAIR>
	std::pair<iterator, bool> insert(PAIR &&value) { // NOLINT: match stl API
		return base::InsertExact(std::forward<PAIR>(value));
	}
	template <class INPUT_ITERATOR>
	void insert(INPUT_ITERATOR first, INPUT_ITERATOR last) { // NOLINT: match stl API
		for (; first != last; ++first) {
			insert(*first);
		}
	}
	template <class... ARGS>
	std::pair<iterator, bool> emplace(ARGS &&...args) { // NOLINT: match stl API
		return base::InsertExact(std::forward<ARGS>(args)...);
	}
	//! Insert or overwrite the value of the exact key
	void SetExact(const Identifier &name, T value) {
		auto entry = base::FindExact(name);
		if (entry != base::end()) {
			entry->second = std::move(value);
			return;
		}
		base::InsertExact(name, std::move(value));
	}
	//! Get the value of the exact key, inserting a default-constructed value if it does not exist
	T &GetOrInsertExact(const Identifier &name) {
		auto entry = base::FindExact(name);
		if (entry != base::end()) {
			return entry->second;
		}
		return base::InsertExact(name, T()).first->second;
	}
	void reserve(idx_t count) { // NOLINT: match stl API
	}
};

//! A set of identifiers that are case-insensitive unless ambiguous (see IdentifierMap)
template <class CONTAINER>
class IdentifierSet : public IdentifierContainerBase<CONTAINER, IdentifierSelfKey> {
	using base = IdentifierContainerBase<CONTAINER, IdentifierSelfKey>;

public:
	using key_type = Identifier;
	using value_type = Identifier;
	using iterator = typename base::iterator;
	using const_iterator = typename base::const_iterator;

public:
	IdentifierSet() = default;
	IdentifierSet(std::initializer_list<Identifier> values) { // NOLINT: allow implicit construction like std::set
		insert(values.begin(), values.end());
	}
	template <class INPUT_ITERATOR>
	IdentifierSet(INPUT_ITERATOR first, INPUT_ITERATOR last) {
		insert(first, last);
	}

	//! Insert the exact name - returns false (and leaves the set unchanged) if the exact name already exists
	std::pair<iterator, bool> insert(const Identifier &value) { // NOLINT: match stl API
		return base::InsertExact(value);
	}
	template <class INPUT_ITERATOR>
	void insert(INPUT_ITERATOR first, INPUT_ITERATOR last) { // NOLINT: match stl API
		for (; first != last; ++first) {
			insert(Identifier(*first));
		}
	}
	template <class... ARGS>
	std::pair<iterator, bool> emplace(ARGS &&...args) { // NOLINT: match stl API
		return base::InsertExact(std::forward<ARGS>(args)...);
	}
	void reserve(idx_t count) { // NOLINT: match stl API
	}
};

template <typename T>
using identifier_map_t = IdentifierMap<T, unordered_map<Identifier, T, IdentifierExactHash, IdentifierExactEquality>>;

using identifier_set_t = IdentifierSet<unordered_set<Identifier, IdentifierExactHash, IdentifierExactEquality>>;

template <typename T>
using identifier_tree_t = IdentifierMap<T, map<Identifier, T, IdentifierExactCompare>>;

//! Resolve a name among a list of names: the exact match if there is one, otherwise the unique case-insensitive match
IdentifierMatch ResolveIdentifier(const vector<Identifier> &names, const Identifier &name, idx_t &index);
//! Whether "name" refers to "target" when resolved among "names" (see ResolveIdentifier)
bool IdentifierRefersTo(const vector<Identifier> &names, const Identifier &name, const Identifier &target);

} // namespace duckdb
