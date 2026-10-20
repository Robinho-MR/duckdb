#include "catch.hpp"
#include "duckdb/common/bignum.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/bignum.hpp"

using namespace duckdb;

TEST_CASE("BIGNUM readers reject blobs smaller than the header", "[bignum]") {
	// a valid BIGNUM blob is at least BIGNUM_HEADER_SIZE + 1 bytes
	char short_blob[Bignum::BIGNUM_HEADER_SIZE] = {0, 0, 0};
	for (idx_t size = 0; size <= Bignum::BIGNUM_HEADER_SIZE; size++) {
		string_t blob(short_blob, UnsafeNumericCast<uint32_t>(size));
		bignum_t value(blob);

		REQUIRE_THROWS_AS(BignumIntermediate(value), DataCorruptionException);
		REQUIRE_THROWS_AS(BignumIntermediate(data_ptr_cast(short_blob), size), DataCorruptionException);

		int64_t result;
		bool strict = false;
		REQUIRE_THROWS_AS(Bignum::BignumToInt<int64_t>(value, result, strict), DataCorruptionException);
	}

	// the smallest valid blob (header + one data byte) is accepted
	char valid_blob[Bignum::BIGNUM_HEADER_SIZE + 1] = {static_cast<char>(0x80), 0, 1, 0};
	bignum_t value(string_t(valid_blob, sizeof(valid_blob)));
	REQUIRE_NOTHROW(BignumIntermediate(value));
}
