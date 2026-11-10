#include "duckdb/storage/external_file_cache/caching_file_system_wrapper.hpp"

#include "duckdb/common/compressed_file_system.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

//===----------------------------------------------------------------------===//
// CachingFileHandleWrapper implementation
//===----------------------------------------------------------------------===//
CachingFileHandleWrapper::CachingFileHandleWrapper(shared_ptr<CachingFileSystemWrapper> file_system,
                                                   unique_ptr<CachingFileHandle> handle, const FileOpenFlags &flags)
    : FileHandle(*file_system, handle->GetPath(), flags), caching_file_system(std::move(file_system)),
      caching_handle(std::move(handle)) {
	D_ASSERT(!flags.OpenForWriting());
	D_ASSERT(!flags.OpenForAppending());
}

CachingFileHandleWrapper::~CachingFileHandleWrapper() {
}

void CachingFileHandleWrapper::Close() {
	if (caching_handle) {
		caching_handle.reset();
	}
}

//===----------------------------------------------------------------------===//
// CachingFileSystemWrapper implementation
//===----------------------------------------------------------------------===//
CachingFileSystemWrapper::CachingFileSystemWrapper(FileSystem &file_system, DatabaseInstance &db, CachingMode mode)
    : caching_file_system(file_system, db), underlying_file_system(file_system), caching_mode(mode) {
}

CachingFileSystemWrapper::CachingFileSystemWrapper(FileSystem &file_system, Context &context, CachingMode mode)
    : caching_file_system(file_system, context.GetDatabase()), underlying_file_system(file_system), caching_mode(mode) {
}

bool CachingFileSystemWrapper::ShouldUseCache(const string &path) const {
	if (caching_mode == CachingMode::ALWAYS_CACHE) {
		return true;
	}
	if (caching_mode == CachingMode::NO_CACHING) {
		return false;
	}
	D_ASSERT(caching_mode == CachingMode::CACHE_REMOTE_ONLY);
	return FileSystem::IsRemoteFile(path);
}

CachingFileHandle *CachingFileSystemWrapper::GetCachingHandleIfPossible(FileHandle &handle) {
	const auto &filepath = handle.GetPath();
	if (!ShouldUseCache(filepath)) {
		return nullptr;
	}
	auto &wrapper = handle.Cast<CachingFileHandleWrapper>();
	return wrapper.caching_handle.get();
}

CachingFileSystemWrapper::~CachingFileSystemWrapper() {
}

std::string CachingFileSystemWrapper::GetName() const {
	return "CachingFileSystemWrapper";
}

//===----------------------------------------------------------------------===//
// Write Operations (Not Supported - Read-Only Filesystem)
//===----------------------------------------------------------------------===//
void CachingFileSystemWrapper::Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	throw NotImplementedException("CachingFileSystemWrapper: Write operations are not supported. "
	                              "CachingFileSystemWrapper is a read-only caching filesystem.");
}

int64_t CachingFileSystemWrapper::Write(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	throw NotImplementedException("CachingFileSystemWrapper: Write operations are not supported. "
	                              "CachingFileSystemWrapper is a read-only caching filesystem.");
}

bool CachingFileSystemWrapper::Trim(FileHandle &handle, idx_t offset_bytes, idx_t length_bytes) {
	throw NotImplementedException("CachingFileSystemWrapper: Trim operations are not supported. "
	                              "CachingFileSystemWrapper is a read-only caching filesystem.");
}

void CachingFileSystemWrapper::Truncate(FileHandle &handle, int64_t new_size) {
	throw NotImplementedException("CachingFileSystemWrapper: Truncate operations are not supported. "
	                              "CachingFileSystemWrapper is a read-only caching filesystem.");
}

void CachingFileSystemWrapper::FileSync(FileHandle &handle) {
	throw NotImplementedException("CachingFileSystemWrapper: FileSync operations are not supported. "
	                              "CachingFileSystemWrapper is a read-only caching filesystem.");
}

//===----------------------------------------------------------------------===//
// OpenFile Operations
//===----------------------------------------------------------------------===//
unique_ptr<FileHandle> CachingFileSystemWrapper::OpenFile(Context &context, const string &path, FileOpenFlags flags) {
	return OpenFile(context, OpenFileInfo(path), flags);
}

unique_ptr<FileHandle> CachingFileSystemWrapper::OpenFile(Context &context, const OpenFileInfo &path,
                                                          FileOpenFlags flags) {
	if (SupportsOpenFileExtended()) {
		return OpenFileExtended(context, path, std::move(flags));
	}
	throw NotImplementedException("CachingFileSystemWrapper: OpenFile is not implemented!");
}

unique_ptr<FileHandle> CachingFileSystemWrapper::OpenFileExtended(Context &context, const OpenFileInfo &path,
                                                                  FileOpenFlags flags) {
	if (flags.OpenForWriting()) {
		throw NotImplementedException("CachingFileSystemWrapper: Cannot open file for writing. "
		                              "CachingFileSystemWrapper is a read-only caching filesystem.");
	}
	if (!flags.OpenForReading()) {
		throw NotImplementedException("CachingFileSystemWrapper: File must be opened for reading. "
		                              "CachingFileSystemWrapper is a read-only caching filesystem.");
	}

	if (ShouldUseCache(path.path)) {
		auto caching_handle = caching_file_system.OpenFile(context, path, flags);
		return make_uniq<CachingFileHandleWrapper>(shared_from_this(), std::move(caching_handle), flags);
	}
	// Bypass cache, use underlying file system directly.
	return underlying_file_system.OpenFile(context, path, flags);
}

bool CachingFileSystemWrapper::SupportsOpenFileExtended() const {
	return true;
}

//===----------------------------------------------------------------------===//
// Read Operations
//===----------------------------------------------------------------------===//
void CachingFileSystemWrapper::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	auto *caching_handle = GetCachingHandleIfPossible(handle);
	if (!caching_handle) {
		return underlying_file_system.Read(handle, buffer, nr_bytes, location);
	}

	auto group = caching_handle->Read(NumericCast<idx_t>(nr_bytes), location);
	group.CopyTo(static_cast<data_ptr_t>(buffer), NumericCast<idx_t>(nr_bytes));
}

int64_t CachingFileSystemWrapper::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	const idx_t current_position = SeekPosition(handle);
	const idx_t max_read = NumericCast<idx_t>(GetFileSize(handle)) - current_position;
	nr_bytes = MinValue<int64_t>(NumericCast<int64_t>(max_read), nr_bytes);
	Read(handle, buffer, nr_bytes, current_position);
	Seek(handle, current_position + NumericCast<idx_t>(nr_bytes));
	return nr_bytes;
}

//===----------------------------------------------------------------------===//
// File Metadata Operations
//===----------------------------------------------------------------------===//
int64_t CachingFileSystemWrapper::GetFileSize(FileHandle &handle) {
	auto *caching_handle = GetCachingHandleIfPossible(handle);
	if (!caching_handle) {
		return underlying_file_system.GetFileSize(handle);
	}

	return NumericCast<int64_t>(caching_handle->GetFileSize());
}

timestamp_t CachingFileSystemWrapper::GetLastModifiedTime(FileHandle &handle) {
	auto *caching_handle = GetCachingHandleIfPossible(handle);
	if (!caching_handle) {
		return underlying_file_system.GetLastModifiedTime(handle);
	}

	return caching_handle->GetLastModifiedTime();
}

string CachingFileSystemWrapper::GetVersionTag(FileHandle &handle) {
	auto *caching_handle = GetCachingHandleIfPossible(handle);
	if (!caching_handle) {
		return underlying_file_system.GetVersionTag(handle);
	}

	return caching_handle->GetVersionTag();
}

FileType CachingFileSystemWrapper::GetFileType(FileHandle &handle) {
	auto *caching_handle = GetCachingHandleIfPossible(handle);
	if (!caching_handle) {
		return underlying_file_system.GetFileType(handle);
	}

	auto file_handle = caching_handle->GetFileHandle();
	return underlying_file_system.GetFileType(*file_handle);
}

FileMetadata CachingFileSystemWrapper::Stats(FileHandle &handle) {
	auto *caching_handle = GetCachingHandleIfPossible(handle);
	if (!caching_handle) {
		return underlying_file_system.Stats(handle);
	}

	auto file_handle = caching_handle->GetFileHandle();
	return underlying_file_system.Stats(*file_handle);
}

optional<FileMetadata> CachingFileSystemWrapper::GetStatsIfExists(Context &context, const OpenFileInfo &file) {
	return underlying_file_system.GetStatsIfExists(context, file);
}

//===----------------------------------------------------------------------===//
// Directory Operations
//===----------------------------------------------------------------------===//
bool CachingFileSystemWrapper::DirectoryExists(Context &context, const string &directory) {
	return underlying_file_system.DirectoryExists(context, directory);
}

void CachingFileSystemWrapper::CreateDirectory(Context &context, const string &directory) {
	CreateDirectoryExtended(context, directory, {CreateDirectoryMode::SINGLE});
}

bool CachingFileSystemWrapper::CreateDirectoryExtended(Context &context, const string &directory,
                                                       const CreateDirectoryOptions &options) {
	return underlying_file_system.CreateDirectoryExtended(context, directory, options);
}

void CachingFileSystemWrapper::CreateDirectoriesRecursive(Context &context, const string &path) {
	CreateDirectoryExtended(context, path, {CreateDirectoryMode::RECURSIVE});
}

void CachingFileSystemWrapper::RemoveDirectory(Context &context, const string &directory) {
	RemoveDirectoryExtended(context, directory, {RemoveDirectoryMode::RECURSIVE});
}

bool CachingFileSystemWrapper::RemoveDirectoryExtended(Context &context, const string &directory,
                                                       const RemoveDirectoryOptions &options) {
	return underlying_file_system.RemoveDirectoryExtended(context, directory, options);
}

bool CachingFileSystemWrapper::ListFiles(Context &context, const string &directory,
                                         const std::function<void(const string &, bool)> &callback) {
	return underlying_file_system.ListFiles(context, directory, callback);
}

bool CachingFileSystemWrapper::ListFilesExtended(Context &context, const string &directory,
                                                 const std::function<void(OpenFileInfo &info)> &callback) {
	// Use the public ListFiles API which will internally call `ListFilesExtended` if supported.
	return underlying_file_system.ListFiles(context, directory, callback);
}

bool CachingFileSystemWrapper::SupportsListFilesExtended() const {
	// Cannot delegate to internal filesystem's invocation since it's `protected`.
	return true;
}

void CachingFileSystemWrapper::MoveFile(Context &context, const string &source, const string &target) {
	underlying_file_system.MoveFile(context, source, target);
}

bool CachingFileSystemWrapper::FileExists(Context &context, const string &filename) {
	return underlying_file_system.FileExists(context, filename);
}

bool CachingFileSystemWrapper::IsPipe(Context &context, const string &filename) {
	return underlying_file_system.IsPipe(context, filename);
}

void CachingFileSystemWrapper::RemoveFile(Context &context, const string &filename) {
	underlying_file_system.RemoveFile(context, filename);
}

bool CachingFileSystemWrapper::TryRemoveFile(Context &context, const string &filename) {
	return underlying_file_system.TryRemoveFile(context, filename);
}

//===----------------------------------------------------------------------===//
// Path Operations
//===----------------------------------------------------------------------===//
string CachingFileSystemWrapper::GetHomeDirectory() {
	return underlying_file_system.GetHomeDirectory();
}

string CachingFileSystemWrapper::ExpandPath(const string &path) {
	return underlying_file_system.ExpandPath(path);
}

string CachingFileSystemWrapper::PathSeparator(const string &path) {
	return underlying_file_system.PathSeparator(path);
}

vector<OpenFileInfo> CachingFileSystemWrapper::Glob(Context &context, const string &path) {
	return underlying_file_system.Glob(context, path);
}

//===----------------------------------------------------------------------===//
// SubSystem Operations
//===----------------------------------------------------------------------===//
void CachingFileSystemWrapper::RegisterSubSystem(unique_ptr<FileSystem> sub_fs) {
	underlying_file_system.RegisterSubSystem(std::move(sub_fs));
}

void CachingFileSystemWrapper::RegisterCompressionFilesystem(unique_ptr<CompressedFileSystem> fs) {
	underlying_file_system.RegisterCompressionFilesystem(std::move(fs));
}

void CachingFileSystemWrapper::UnregisterSubSystem(const string &name) {
	underlying_file_system.UnregisterSubSystem(name);
}

unique_ptr<FileSystem> CachingFileSystemWrapper::ExtractSubSystem(const string &name) {
	return underlying_file_system.ExtractSubSystem(name);
}

vector<string> CachingFileSystemWrapper::ListSubSystems() {
	return underlying_file_system.ListSubSystems();
}

bool CachingFileSystemWrapper::CanHandleFile(const string &fpath) {
	return underlying_file_system.CanHandleFile(fpath);
}

//===----------------------------------------------------------------------===//
// File Handle Operations
//===----------------------------------------------------------------------===//
void CachingFileSystemWrapper::Seek(FileHandle &handle, idx_t location) {
	auto *caching_handle = GetCachingHandleIfPossible(handle);
	if (!caching_handle) {
		return underlying_file_system.Seek(handle, location);
	}

	caching_handle->Seek(location);
}

void CachingFileSystemWrapper::Reset(FileHandle &handle) {
	Seek(handle, 0);
}

idx_t CachingFileSystemWrapper::SeekPosition(FileHandle &handle) {
	auto *caching_handle = GetCachingHandleIfPossible(handle);
	if (!caching_handle) {
		return underlying_file_system.SeekPosition(handle);
	}

	return caching_handle->SeekPosition();
}

bool CachingFileSystemWrapper::IsManuallySet() {
	return underlying_file_system.IsManuallySet();
}

bool CachingFileSystemWrapper::CanSeek() {
	return underlying_file_system.CanSeek();
}

bool CachingFileSystemWrapper::OnDiskFile(FileHandle &handle) {
	auto *caching_handle = GetCachingHandleIfPossible(handle);
	if (!caching_handle) {
		return underlying_file_system.OnDiskFile(handle);
	}

	return caching_handle->OnDiskFile();
}

//===----------------------------------------------------------------------===//
// Other Operations
//===----------------------------------------------------------------------===//
unique_ptr<FileHandle> CachingFileSystemWrapper::OpenCompressedFile(Context &context, unique_ptr<FileHandle> handle,
                                                                    bool write) {
	return underlying_file_system.OpenCompressedFile(context, std::move(handle), write);
}

void CachingFileSystemWrapper::SetDisabledFileSystems(const vector<string> &names) {
	underlying_file_system.SetDisabledFileSystems(names);
}

bool CachingFileSystemWrapper::SubSystemIsDisabled(const string &name) {
	return underlying_file_system.SubSystemIsDisabled(name);
}

bool CachingFileSystemWrapper::IsDisabledForPath(const string &path) {
	return underlying_file_system.IsDisabledForPath(path);
}

} // namespace duckdb
