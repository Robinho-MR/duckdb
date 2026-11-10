//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/opener_file_system.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/file_system.hpp"

namespace duckdb {
class MultiFileList;

// The OpenerFileSystem is wrapper for a file system that provides the Context-free API calls by forwarding its own
// Context
class OpenerFileSystem : public FileSystem {
public:
	virtual FileSystem &GetFileSystem() const = 0;
	virtual Context &GetContext() const = 0;

	void VerifyCanAccessDirectory(Context &context, const string &path);
	void VerifyCanAccessFile(Context &context, const string &path);
	void VerifyCanAccessExtension(const string &path, const FileOpenFlags &flags) {
		if (flags.OpenForWriting() && !flags.EnableExtensionInstall()) {
			throw PermissionException(
			    "File '%s' cannot be opened for writing since files ending with '.duckdb_extension' are reserved for "
			    "DuckDB extensions, and these can only be installed through the INSTALL command",
			    path);
		}
	}

	//! Reserved for DuckDB's extension trust domain: the loadable binary and every file that decides whether it may
	//! be loaded (install provenance, trusted repositories). These can only be written through INSTALL / CREATE
	//! EXTENSION REPOSITORY, which pass FILE_FLAGS_ENABLE_EXTENSION_INSTALL.
	//! To add a file to the trust domain, give it a ".duckdb_extension." segment, e.g.
	//! "<name>.duckdb_extension.info" or "<name>.duckdb_extension.repo.json". Matching is on the file name only, so a
	//! directory that happens to contain the marker does not reserve the files inside it
	bool IsDuckDBExtensionName(Context &context, const string &path) {
		// match the canonical name case-insensitively: on the default macOS and Windows file systems a
		// differently-cased name refers to the same file
		auto name = StringUtil::Lower(FileSystem::ExtractName(CanonicalizePath(context, path)));
		return StringUtil::EndsWith(name, ".duckdb_extension") || StringUtil::Contains(name, ".duckdb_extension.");
	}

	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override {
		GetFileSystem().Read(handle, buffer, nr_bytes, location);
	}

	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override {
		GetFileSystem().Write(handle, buffer, nr_bytes, location);
	}

	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override {
		return GetFileSystem().Read(handle, buffer, nr_bytes);
	}

	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override {
		return GetFileSystem().Write(handle, buffer, nr_bytes);
	}

	int64_t GetFileSize(FileHandle &handle) override {
		return GetFileSystem().GetFileSize(handle);
	}
	timestamp_t GetLastModifiedTime(FileHandle &handle) override {
		return GetFileSystem().GetLastModifiedTime(handle);
	}
	string GetVersionTag(FileHandle &handle) override {
		return GetFileSystem().GetVersionTag(handle);
	}
	FileType GetFileType(FileHandle &handle) override {
		return GetFileSystem().GetFileType(handle);
	}
	FileMetadata Stats(FileHandle &handle) override {
		return GetFileSystem().Stats(handle);
	}

	void Truncate(FileHandle &handle, int64_t new_size) override {
		GetFileSystem().Truncate(handle, new_size);
	}

	void FileSync(FileHandle &handle) override {
		GetFileSystem().FileSync(handle);
	}

	bool DirectoryExists(Context &context, const string &directory) override {
		VerifyCanAccessDirectory(context, directory);
		return GetFileSystem().DirectoryExists(context, directory);
	}
	void CreateDirectory(Context &context, const string &directory) override {
		CreateDirectoryExtended(context, directory, {CreateDirectoryMode::SINGLE});
	}
	bool CreateDirectoryExtended(Context &context, const string &directory,
	                             const CreateDirectoryOptions &options) override {
		if (options.mode == CreateDirectoryMode::RECURSIVE) {
			return FileSystem::CreateDirectoryExtended(context, directory, options);
		}
		VerifyCanAccessDirectory(context, directory);
		return GetFileSystem().CreateDirectoryExtended(context, directory, options);
	}
	void CreateDirectoriesRecursive(Context &context, const string &path) override {
		CreateDirectoryExtended(context, path, {CreateDirectoryMode::RECURSIVE});
	}

	void RemoveDirectory(Context &context, const string &directory) override {
		RemoveDirectoryExtended(context, directory, {RemoveDirectoryMode::RECURSIVE});
	}
	bool RemoveDirectoryExtended(Context &context, const string &directory,
	                             const RemoveDirectoryOptions &options) override {
		VerifyCanAccessDirectory(context, directory);
		return GetFileSystem().RemoveDirectoryExtended(context, directory, options);
	}

	void MoveFile(Context &context, const string &source, const string &target) override {
		VerifyCanAccessFile(context, source);
		VerifyCanAccessFile(context, target);
		if (IsDuckDBExtensionName(context, target) && !IsDuckDBExtensionName(context, source)) {
			throw PermissionException(
			    "File '%s' cannot be moved to '%s', files ending with '.duckdb_extension' are reserved for DuckDB "
			    "extensions, and these can only be installed through the INSTALL command, or moved if both are "
			    "extensions'",
			    source, target);
		}
		GetFileSystem().MoveFile(context, source, target);
	}

	bool FileExists(Context &context, const string &filename) override {
		VerifyCanAccessFile(context, filename);
		return GetFileSystem().FileExists(context, filename);
	}

	optional<FileMetadata> GetStatsIfExists(Context &context, const OpenFileInfo &file) override {
		VerifyCanAccessFile(context, file.path);
		return GetFileSystem().GetStatsIfExists(context, file);
	}

	bool IsPipe(Context &context, const string &filename) override {
		return GetFileSystem().IsPipe(context, filename);
	}
	void RemoveFile(Context &context, const string &filename) override {
		VerifyCanAccessFile(context, filename);
		GetFileSystem().RemoveFile(context, filename);
	}

	bool TryRemoveFile(Context &context, const string &filename) override {
		VerifyCanAccessFile(context, filename);
		return GetFileSystem().TryRemoveFile(context, filename);
	}

	void RemoveFiles(Context &context, const vector<string> &filenames) override {
		for (const auto &filename : filenames) {
			VerifyCanAccessFile(context, filename);
		}
		GetFileSystem().RemoveFiles(context, filenames);
	}

	string PathSeparator(const string &path) override {
		return GetFileSystem().PathSeparator(path);
	}

	vector<OpenFileInfo> Glob(Context &context, const string &path) override {
		VerifyCanAccessFile(context, path);
		return GetFileSystem().Glob(context, path);
	}

	std::string GetName() const override {
		return "OpenerFileSystem - " + GetFileSystem().GetName();
	}

	void RegisterSubSystem(unique_ptr<FileSystem> sub_fs) override {
		GetFileSystem().RegisterSubSystem(std::move(sub_fs));
	}

	void RegisterCompressionFilesystem(unique_ptr<CompressedFileSystem> fs) override;

	void UnregisterSubSystem(const string &name) override {
		GetFileSystem().UnregisterSubSystem(name);
	}

	unique_ptr<FileSystem> ExtractSubSystem(const string &name) override {
		return GetFileSystem().ExtractSubSystem(name);
	}

	void SetDisabledFileSystems(const vector<string> &names) override {
		GetFileSystem().SetDisabledFileSystems(names);
	}

	bool SubSystemIsDisabled(const string &name) override {
		return GetFileSystem().SubSystemIsDisabled(name);
	}

	bool IsDisabledForPath(const string &path) override {
		return GetFileSystem().IsDisabledForPath(path);
	}

	vector<string> ListSubSystems() override {
		return GetFileSystem().ListSubSystems();
	}

protected:
	unique_ptr<FileHandle> OpenFileExtended(Context &context, const OpenFileInfo &file, FileOpenFlags flags) override {
		VerifyCanAccessFile(context, file.path);
		if (IsDuckDBExtensionName(context, file.path)) {
			VerifyCanAccessExtension(file.path, flags);
		}
		return GetFileSystem().OpenFile(context, file, flags);
	}

	bool SupportsOpenFileExtended() const override {
		return true;
	}

public:
	unique_ptr<MemoryMappedFile> MemoryMapFile(Context &context, const OpenFileInfo &path, FileOpenFlags flags,
	                                           const MMapOptions &options) override;

protected:
	bool ListFilesExtended(Context &context, const string &directory,
	                       const std::function<void(OpenFileInfo &info)> &callback) override {
		VerifyCanAccessDirectory(context, directory);
		return GetFileSystem().ListFiles(context, directory, callback);
	}

	bool SupportsListFilesExtended() const override {
		return true;
	}

	unique_ptr<MultiFileList> GlobFilesExtended(Context &context, const string &path,
	                                            const FileGlobInput &input) override;

	bool SupportsGlobExtended() const override {
		return true;
	}

public:
	string CanonicalizePath(Context &context, const string &path) override {
		return GetFileSystem().CanonicalizePath(context, path);
	}
	string CanonicalizePath(const string &path) override {
		return CanonicalizePath(GetContext(), path);
	}

	//! Context-free variants: forward to the Context-taking variants with the Context of this file system
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags) override {
		return FileSystem::OpenFile(GetContext(), path, flags);
	}
	unique_ptr<FileHandle> OpenFile(const OpenFileInfo &path, const FileOpenFlags &flags) override {
		return FileSystem::OpenFile(GetContext(), path, flags);
	}
	unique_ptr<MemoryMappedFile> MemoryMapFile(const OpenFileInfo &path, FileOpenFlags flags,
	                                           const MMapOptions &options) override;
	optional<FileMetadata> GetStatsIfExists(const OpenFileInfo &file) override {
		return GetStatsIfExists(GetContext(), file);
	}
	bool DirectoryExists(const string &directory) override {
		return DirectoryExists(GetContext(), directory);
	}
	void CreateDirectory(const string &directory) override {
		CreateDirectory(GetContext(), directory);
	}
	bool CreateDirectoryExtended(const string &directory, const CreateDirectoryOptions &options) override {
		return CreateDirectoryExtended(GetContext(), directory, options);
	}
	void CreateDirectoriesRecursive(const string &path) override {
		CreateDirectoriesRecursive(GetContext(), path);
	}
	void RemoveDirectory(const string &directory) override {
		RemoveDirectory(GetContext(), directory);
	}
	bool RemoveDirectoryExtended(const string &directory, const RemoveDirectoryOptions &options) override {
		return RemoveDirectoryExtended(GetContext(), directory, options);
	}
	bool ListFiles(const string &directory, const std::function<void(const string &, bool)> &callback) override {
		return FileSystem::ListFiles(GetContext(), directory, callback);
	}
	bool ListFiles(const string &directory, const std::function<void(OpenFileInfo &info)> &callback) override {
		return FileSystem::ListFiles(GetContext(), directory, callback);
	}
	void MoveFile(const string &source, const string &target) override {
		MoveFile(GetContext(), source, target);
	}
	bool FileExists(const string &filename) override {
		return FileExists(GetContext(), filename);
	}
	bool IsPipe(const string &filename) override {
		return IsPipe(GetContext(), filename);
	}
	void RemoveFile(const string &filename) override {
		RemoveFile(GetContext(), filename);
	}
	bool TryRemoveFile(const string &filename) override {
		return TryRemoveFile(GetContext(), filename);
	}
	void RemoveFiles(const vector<string> &filenames) override {
		RemoveFiles(GetContext(), filenames);
	}
	vector<OpenFileInfo> Glob(const string &path) override {
		return Glob(GetContext(), path);
	}
	unique_ptr<MultiFileList> Glob(const string &path, const FileGlobInput &input) override {
		return FileSystem::Glob(GetContext(), path, input);
	}
	unique_ptr<MultiFileList> GlobFileList(const string &path, const FileGlobInput &input) override {
		return FileSystem::GlobFileList(GetContext(), path, input);
	}
	vector<OpenFileInfo> GlobFiles(const string &pattern, const FileGlobInput &input) override {
		return FileSystem::GlobFiles(GetContext(), pattern, input);
	}
	string GetHomeDirectory() override {
		return FileSystem::GetHomeDirectory(GetContext());
	}
	string ExpandPath(const string &path) override {
		return FileSystem::ExpandPath(GetContext(), path);
	}

private:
	void VerifyCanAccessFileInternal(Context &context, const string &path, FileType type);
};

} // namespace duckdb
