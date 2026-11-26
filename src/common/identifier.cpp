#include "duckdb/common/identifier.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/uuid.hpp"

#include <ostream>

namespace duckdb {

void Identifier::ThrowUnsetIdentifier() {
	throw InternalException("Attempted to get the name of an unset Identifier");
}

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
	return StringUtil::CIEquals(a.value, b.value);
}
bool operator==(const Identifier &a, const string &b) {
	return StringUtil::CIEquals(a.value, b);
}
bool operator==(const string &a, const Identifier &b) {
	return StringUtil::CIEquals(a, b.value);
}
bool operator==(const Identifier &a, const char *b) {
	return StringUtil::CIEquals(a.value, b);
}
bool operator==(const char *a, const Identifier &b) {
	return StringUtil::CIEquals(a, b.value);
}

bool operator<(const Identifier &a, const Identifier &b) {
	return StringUtil::CILessThan(a.value, b.value);
}

std::ostream &operator<<(std::ostream &os, const Identifier &id) {
	return os << id.value;
}

} // namespace duckdb
