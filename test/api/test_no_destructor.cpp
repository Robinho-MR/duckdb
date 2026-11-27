#include "catch.hpp"
#include "duckdb/common/enums/file_compression_type.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/common/no_destructor.hpp"
#include "duckdb/common/types/date.hpp"

#include <type_traits>

using namespace duckdb;

namespace {

struct DestructorCounter {
	explicit DestructorCounter(int &destroyed_p, int value_p) : destroyed(destroyed_p), value(value_p) {
	}
	~DestructorCounter() {
		destroyed++;
	}
	int &destroyed;
	int value;
};

} // namespace

TEST_CASE("Test NoDestructor never runs the destructor", "[api]") {
	int destroyed = 0;
	{
		NoDestructor<DestructorCounter> holder(destroyed, 42);
		REQUIRE(holder->value == 42);
		REQUIRE((*holder).value == 42);
		REQUIRE(holder.Get() == &*holder);
	}
	REQUIRE(destroyed == 0);
	static_assert(std::is_trivially_destructible<NoDestructor<DestructorCounter>>::value,
	              "NoDestructor must be trivially destructible");
	static_assert(std::is_trivially_destructible<NoDestructor<string>>::value, "NoDestructor must be trivial");
}

TEST_CASE("Test FileCompressionType values", "[api]") {
	static_assert(std::is_trivially_destructible<FileCompressionType>::value, "must not need a static destructor");
	static_assert(std::is_trivially_destructible<FileOpenFlags>::value, "must not need a static destructor");

	REQUIRE(FileCompressionType().IsUncompressed());
	REQUIRE(FileCompressionType::UNCOMPRESSED.IsUncompressed());
	REQUIRE(FileCompressionType::AUTO_DETECT.IsAutoDetect());
	REQUIRE(FileCompressionType::GZIP.IsCompressed());
	REQUIRE(FileCompressionType::ZSTD.IsCompressed());
	REQUIRE(!FileCompressionType::AUTO_DETECT.IsCompressed());

	// aliases and casing are normalized
	REQUIRE(FileCompressionType("GZIP") == FileCompressionType::GZIP);
	REQUIRE(FileCompressionType("Zstd") == FileCompressionType::ZSTD);
	REQUIRE(FileCompressionType("infer") == FileCompressionType::AUTO_DETECT);
	REQUIRE(FileCompressionType("auto") == FileCompressionType::AUTO_DETECT);
	REQUIRE(FileCompressionType("auto_detect") == FileCompressionType::AUTO_DETECT);
	REQUIRE(FileCompressionType("none") == FileCompressionType::UNCOMPRESSED);
	REQUIRE(FileCompressionType("") == FileCompressionType::UNCOMPRESSED);
	REQUIRE(FileCompressionType("uncompressed") == FileCompressionType::UNCOMPRESSED);
	REQUIRE(FileCompressionType::GZIP.ToString() == "gzip");
	REQUIRE(FileCompressionType::ZSTD.ToString() == "zstd");
	REQUIRE(FileCompressionType::AUTO_DETECT.ToString() == "auto_detect");
	REQUIRE(FileCompressionType::UNCOMPRESSED.ToString() == "uncompressed");
	REQUIRE(FileCompressionType::GZIP != FileCompressionType::ZSTD);

	// custom compression types keep their name and compare by name
	FileCompressionType custom("My-Codec");
	REQUIRE(custom.IsCompressed());
	REQUIRE(custom.ToString() == "my-codec");
	REQUIRE(custom == FileCompressionType("MY-CODEC"));
	REQUIRE(custom != FileCompressionType("other-codec"));
	REQUIRE(custom != FileCompressionType::GZIP);
	REQUIRE(&custom.ToString() == &FileCompressionType("my-codec").ToString());
}

TEST_CASE("Test file flag constants", "[api]") {
	auto flags = FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_WRITE;
	REQUIRE(flags.OpenForReading());
	REQUIRE(flags.OpenForWriting());
	REQUIRE(!flags.OpenForAppending());
	REQUIRE(FileFlags::FILE_FLAGS_APPEND.OpenForAppending());
	REQUIRE(FileFlags::FILE_FLAGS_FILE_CREATE.CreateFileIfNotExists());
	REQUIRE(FileFlags::FILE_FLAGS_READ.Compression().IsUncompressed());

	auto compressed = FileFlags::FILE_FLAGS_READ | FileCompressionType::GZIP;
	REQUIRE(compressed.Compression() == FileCompressionType::GZIP);
}

TEST_CASE("Test date names", "[api]") {
	REQUIRE(Date::MonthNames()[0].GetString() == "January");
	REQUIRE(Date::MonthNames()[11].GetString() == "December");
	REQUIRE(Date::MonthNamesAbbreviated()[1].GetString() == "Feb");
	REQUIRE(Date::DayNames()[0].GetString() == "Sunday");
	REQUIRE(Date::DayNames()[6].GetString() == "Saturday");
	REQUIRE(Date::DayNamesAbbreviated()[3].GetString() == "Wed");
}
