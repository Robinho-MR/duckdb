#include "duckdb/common/opener_file_system.hpp"
#include "duckdb/common/compressed_file_system.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/common/memory_mapped_file.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

void OpenerFileSystem::RegisterCompressionFilesystem(unique_ptr<CompressedFileSystem> fs) {
	GetFileSystem().RegisterCompressionFilesystem(std::move(fs));
}

unique_ptr<MemoryMappedFile> OpenerFileSystem::MemoryMapFile(Context &context, const OpenFileInfo &path,
                                                             FileOpenFlags flags, const MMapOptions &options) {
	VerifyCanAccessFile(context, path.path);
	if (IsDuckDBExtensionName(context, path.path)) {
		VerifyCanAccessExtension(path.path, flags);
	}
	return GetFileSystem().MemoryMapFile(context, path, flags, options);
}

unique_ptr<MemoryMappedFile> OpenerFileSystem::MemoryMapFile(const OpenFileInfo &path, FileOpenFlags flags,
                                                             const MMapOptions &options) {
	return MemoryMapFile(GetContext(), path, flags, options);
}

unique_ptr<MultiFileList> OpenerFileSystem::GlobFilesExtended(Context &context, const string &path,
                                                              const FileGlobInput &input) {
	VerifyCanAccessFile(context, path);
	return GetFileSystem().Glob(context, path, input);
}

void OpenerFileSystem::VerifyCanAccessFileInternal(Context &context, const string &path, FileType type) {
	auto &config = context.GetDatabase().config;
	if (!config.CanAccessFile(context, path, type)) {
		throw PermissionException("Cannot access %s \"%s\" - file system operations are disabled by configuration",
		                          type == FileType::FILE_TYPE_DIR ? "directory" : "file", path);
	}
}

void OpenerFileSystem::VerifyCanAccessFile(Context &context, const string &path) {
	VerifyCanAccessFileInternal(context, path, FileType::FILE_TYPE_REGULAR);
}

void OpenerFileSystem::VerifyCanAccessDirectory(Context &context, const string &path) {
	VerifyCanAccessFileInternal(context, path, FileType::FILE_TYPE_DIR);
}

} // namespace duckdb
