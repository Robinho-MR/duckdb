#include "catch.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"

using namespace duckdb;

TEST_CASE("BinaryDeserializer rejects excessive nesting", "[serializer]") {
	MemoryStream stream;
	BinaryDeserializer deserializer(stream);

	// unbounded nesting is rejected instead of overflowing the stack
	idx_t depth = 0;
	REQUIRE_THROWS_AS(([&]() {
		                  for (; depth < 1000000; depth++) {
			                  deserializer.Begin();
		                  }
	                  })(),
	                  SerializationException);
	// reasonable nesting depths are still accepted
	REQUIRE(depth >= 1000);
	REQUIRE(depth < 1000000);
}
