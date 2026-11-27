#include "duckdb/common/enums/file_compression_type.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/no_destructor.hpp"
#include "duckdb/common/unordered_set.hpp"

namespace duckdb {

const FileCompressionType FileCompressionType::AUTO_DETECT = FileCompressionType(Kind::AUTO_DETECT);
const FileCompressionType FileCompressionType::UNCOMPRESSED = FileCompressionType(Kind::UNCOMPRESSED);
const FileCompressionType FileCompressionType::GZIP = FileCompressionType(Kind::GZIP);
const FileCompressionType FileCompressionType::ZSTD = FileCompressionType(Kind::ZSTD);

static const string &InternCompressionName(string name) {
	static NoDestructor<mutex> intern_lock;
	static NoDestructor<unordered_set<string>> interned;
	lock_guard<mutex> guard(*intern_lock);
	return *interned->insert(std::move(name)).first;
}

FileCompressionType::FileCompressionType(string compression) : kind(Kind::CUSTOM), custom(nullptr) {
	compression = StringUtil::Lower(compression);
	if (compression == "infer" || compression == "auto" || compression == "auto_detect") {
		kind = Kind::AUTO_DETECT;
	} else if (compression == "none" || compression.empty() || compression == "uncompressed") {
		kind = Kind::UNCOMPRESSED;
	} else if (compression == "gzip") {
		kind = Kind::GZIP;
	} else if (compression == "zstd") {
		kind = Kind::ZSTD;
	} else {
		custom = &InternCompressionName(std::move(compression));
	}
}

bool FileCompressionType::operator==(const FileCompressionType &other) const {
	return kind == other.kind && custom == other.custom;
}

bool FileCompressionType::operator!=(const FileCompressionType &other) const {
	return !(*this == other);
}

bool FileCompressionType::IsCompressed() const {
	return !IsUncompressed() && !IsAutoDetect();
}

bool FileCompressionType::IsUncompressed() const {
	return kind == Kind::UNCOMPRESSED;
}

bool FileCompressionType::IsAutoDetect() const {
	return kind == Kind::AUTO_DETECT;
}

const string &FileCompressionType::ToString() const {
	static const NoDestructor<string> uncompressed("uncompressed");
	static const NoDestructor<string> auto_detect("auto_detect");
	static const NoDestructor<string> gzip("gzip");
	static const NoDestructor<string> zstd("zstd");
	switch (kind) {
	case Kind::UNCOMPRESSED:
		return *uncompressed;
	case Kind::AUTO_DETECT:
		return *auto_detect;
	case Kind::GZIP:
		return *gzip;
	case Kind::ZSTD:
		return *zstd;
	default:
		return *custom;
	}
}

string CompressionExtensionFromType(const FileCompressionType &type) {
	if (type == FileCompressionType::GZIP) {
		return ".gz";
	}
	if (type == FileCompressionType::ZSTD) {
		return ".zst";
	}
	throw NotImplementedException("Compression Extension of file compression type is not implemented");
}

bool IsFileCompressed(string path, const FileCompressionType &type) {
	auto extension = CompressionExtensionFromType(type);
	std::size_t question_mark_pos = std::string::npos;
	if (!StringUtil::StartsWith(path, "\\\\?\\")) {
		question_mark_pos = path.find('?');
	}
	path = path.substr(0, question_mark_pos);
	if (StringUtil::EndsWith(path, extension)) {
		return true;
	}
	return false;
}

} // namespace duckdb
