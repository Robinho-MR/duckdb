#include "duckdb/common/identifier.hpp"
#include "duckdb/common/identifier_lookup_map.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/uuid.hpp"

#include <ostream>

namespace duckdb {

Identifier GenerateInternalName(const char *prefix) {
	return Identifier(prefix + UUID::ToString(UUID::GenerateRandomUUID()));
}

bool Identifier::StartsWith(const string &prefix) const {
	return StringUtil::CIStartsWith(value, prefix);
}

bool Identifier::EndsWith(const string &suffix) const {
	return StringUtil::CIEndsWith(value, suffix);
}

hash_t Identifier::Hash() const {
	return StringUtil::CIHash(value);
}

bool operator==(const Identifier &a, const Identifier &b) {
	return StringUtil::CIEquals(a.GetIdentifierName(), b.GetIdentifierName());
}
bool operator==(const Identifier &a, const string &b) {
	return StringUtil::CIEquals(a.GetIdentifierName(), b);
}
bool operator==(const string &a, const Identifier &b) {
	return StringUtil::CIEquals(a, b.GetIdentifierName());
}
bool operator==(const Identifier &a, const char *b) {
	return StringUtil::CIEquals(a.GetIdentifierName(), b);
}
bool operator==(const char *a, const Identifier &b) {
	return StringUtil::CIEquals(a, b.GetIdentifierName());
}

bool operator<(const Identifier &a, const Identifier &b) {
	return StringUtil::CILessThan(a.GetIdentifierName(), b.GetIdentifierName());
}

IdentifierMatch ResolveIdentifier(const vector<Identifier> &names, const Identifier &name, idx_t &index) {
	index = DConstants::INVALID_INDEX;
	idx_t match_count = 0;
	for (idx_t i = 0; i < names.size(); i++) {
		if (names[i].GetIdentifierName() == name.GetIdentifierName()) {
			index = i;
			return IdentifierMatch::FOUND;
		}
		if (names[i] == name) {
			if (match_count == 0) {
				index = i;
			}
			match_count++;
		}
	}
	if (match_count == 0) {
		return IdentifierMatch::NOT_FOUND;
	}
	if (match_count > 1) {
		index = DConstants::INVALID_INDEX;
		return IdentifierMatch::AMBIGUOUS;
	}
	return IdentifierMatch::FOUND;
}

bool IdentifierRefersTo(const vector<Identifier> &names, const Identifier &name, const Identifier &target) {
	if (name.GetIdentifierName() == target.GetIdentifierName()) {
		return true;
	}
	if (name != target) {
		return false;
	}
	// the names only differ in case: "name" refers to "target" only if it is the unique case-insensitive match
	idx_t index;
	if (ResolveIdentifier(names, name, index) != IdentifierMatch::FOUND) {
		return false;
	}
	return names[index].GetIdentifierName() == target.GetIdentifierName();
}

std::ostream &operator<<(std::ostream &os, const Identifier &id) {
	return os << id.GetIdentifierName();
}

} // namespace duckdb
