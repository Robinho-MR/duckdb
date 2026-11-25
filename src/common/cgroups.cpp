#include "duckdb/common/cgroups.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/printer.hpp"

#include <cinttypes>
#include <fstream>
#include <sstream>

namespace duckdb {

#if defined(__linux__) && !defined(DUCKDB_WASM)

namespace {

static constexpr const char *CGROUP_PATH = "/proc/self/cgroup";

//! Reads a small kernel-provided file. These are read directly: they describe the process itself and are not subject
//! to the file access settings of the database
static bool TryReadSystemFile(const string &path, string &result) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return false;
	}
	std::ostringstream content;
	content << file.rdbuf();
	result = content.str();
	return true;
}

struct CGroupEntry {
public:
	CGroupEntry(idx_t hierarchy_id, vector<string> &&controller_list, const string &cgroup_path)
	    : hierarchy_id(hierarchy_id), controller_list(std::move(controller_list)), cgroup_path(cgroup_path) {
	}

public:
	bool IsRoot() const {
		if (hierarchy_id != 0) {
			return false;
		}
		if (controller_list.size() != 1) {
			return false;
		}
		return controller_list[0].empty();
	}

public:
	idx_t hierarchy_id;
	vector<string> controller_list;
	string cgroup_path;
};

static vector<CGroupEntry> ParseGroupEntries() {
	vector<CGroupEntry> result;
	string cgroup_file_content;
	if (!TryReadSystemFile(CGROUP_PATH, cgroup_file_content)) {
		return result;
	}

	auto lines = StringUtil::Split(cgroup_file_content, "\n");
	for (auto &line : lines) {
		//! NOTE: this can not use StringUtil::Split, as it counts '::' as a single delimiter
		vector<string> parts;
		auto it = line.begin();
		while (it != line.end()) {
			//! Don't make more than 3 splits
			auto next = parts.size() == 2 ? line.end() : std::find_if(it, line.end(), [](char c) { return c == ':'; });

			parts.emplace_back(it, next);
			if (next == line.end()) {
				break;
			}
			it = std::next(next);
		}

		if (parts.size() < 3) {
			//! cgroup entries are in this format:
			// hierarchy-ID:controller-list:cgroup-path
			break;
		}
		auto hierarchy_id = std::stoi(parts[0]);
		auto controller_list = StringUtil::Split(parts[1], ",");
		auto cgroup_path = parts[2] == "/" ? "" : parts[2];
		result.emplace_back(hierarchy_id, std::move(controller_list), cgroup_path);
	}
	return result;
}

static optional_idx GetCPUCountV2(const string &cgroup_path) {
	static constexpr const char *CPU_MAX = "/sys/fs/cgroup%s/cpu.max";

	auto cpu_max = StringUtil::Format(CPU_MAX, cgroup_path);

	//! See https://docs.kernel.org/scheduler/sched-bwc.html
	//! run-time replenished within a period (in microseconds)
	int64_t quota;
	//! the length of a period (in microseconds)
	int64_t period;

	// cgroup v2
	string contents;
	if (!TryReadSystemFile(cpu_max, contents)) {
		return optional_idx();
	}
	if (std::sscanf(contents.c_str(), "%" SCNd64 " %" SCNd64 "", &quota, &period) != 2) {
		return optional_idx();
	}

	if (quota > 0 && period > 0) {
		return idx_t(std::ceil((double)quota / (double)period));
	}
	return optional_idx();
}

static optional_idx GetCPUCountV1(const string &cgroup_path) {
	static constexpr const char *CFS_QUOTA = "/sys/fs/cgroup/cpu%s/cpu.cfs_quota_us";
	static constexpr const char *CFS_PERIOD = "/sys/fs/cgroup/cpu%s/cpu.cfs_period_us";

	auto cfs_quota = StringUtil::Format(CFS_QUOTA, cgroup_path);
	auto cfs_period = StringUtil::Format(CFS_PERIOD, cgroup_path);

	//! See https://docs.kernel.org/scheduler/sched-bwc.html
	//! run-time replenished within a period (in microseconds)
	int64_t quota;
	//! the length of a period (in microseconds)
	int64_t period;

	// cgroup v1
	string quota_contents;
	string period_contents;
	if (!TryReadSystemFile(cfs_quota, quota_contents) || !TryReadSystemFile(cfs_period, period_contents)) {
		return optional_idx();
	}
	if (std::sscanf(quota_contents.c_str(), "%" SCNd64 "", &quota) != 1) {
		return optional_idx();
	}
	if (std::sscanf(period_contents.c_str(), "%" SCNd64 "", &period) != 1) {
		return optional_idx();
	}

	if (quota > 0 && period > 0) {
		return idx_t(std::ceil((double)quota / (double)period));
	}
	return optional_idx();
}

static optional_idx ReadMemoryLimit(const string &file_path) {
	string buffer;
	if (!TryReadSystemFile(file_path, buffer)) {
		return optional_idx();
	}

	auto contents = string_t(buffer.c_str(), UnsafeNumericCast<uint32_t>(buffer.size()));
	idx_t value;
	if (TryCast::Operation<string_t, idx_t>(contents, value)) {
		return optional_idx(value);
	}
	return optional_idx();
}

} // namespace

optional_idx CGroups::GetMemoryLimit() {
	static constexpr const char *MEMORY_LIMIT_IN_BYTES = "/sys/fs/cgroup/memory%s/memory.limit_in_bytes";
	static constexpr const char *MEMORY_MAX = "/sys/fs/cgroup%s/memory.max";

	optional_idx root_entry;
	optional_idx memory_entry;

	auto cgroup_entries = ParseGroupEntries();
	for (idx_t i = 0; i < cgroup_entries.size(); i++) {
		auto &entry = cgroup_entries[i];
		auto &controller_list = entry.controller_list;
		if (entry.IsRoot()) {
			root_entry = i;
			continue;
		}
		for (auto &controller : controller_list) {
			if (controller == "memory") {
				memory_entry = i;
				break;
			}
		}
	}

	// TODO: we currently fall back to the root directory, because in virtual environments
	// the cgroups are often mapped to the root directory.
	// To properly handle this, we should parse and use the mapping in `/proc/self/mountinfo`
	if (memory_entry.IsValid()) {
		auto &entry = cgroup_entries[memory_entry.GetIndex()];
		auto path = StringUtil::Format(MEMORY_LIMIT_IN_BYTES, entry.cgroup_path);
		auto memory_limit = ReadMemoryLimit(path.c_str());
		if (memory_limit.IsValid()) {
			return memory_limit;
		}
		//! try falling back to the root directory
		path = StringUtil::Format(MEMORY_LIMIT_IN_BYTES, "");
		memory_limit = ReadMemoryLimit(path.c_str());
		if (memory_limit.IsValid()) {
			return memory_limit;
		}
	}
	if (root_entry.IsValid()) {
		auto &entry = cgroup_entries[root_entry.GetIndex()];
		auto path = StringUtil::Format(MEMORY_MAX, entry.cgroup_path);
		auto memory_limit = ReadMemoryLimit(path.c_str());
		if (memory_limit.IsValid()) {
			return memory_limit;
		}
		//! try falling back to the root directory
		path = StringUtil::Format(MEMORY_MAX, "");
		memory_limit = ReadMemoryLimit(path.c_str());
		if (memory_limit.IsValid()) {
			return memory_limit;
		}
	}
	return optional_idx();
}

idx_t CGroups::GetCPULimit(idx_t physical_cores) {
	optional_idx root_entry;
	optional_idx cpu_entry;

	auto cgroup_entries = ParseGroupEntries();
	for (idx_t i = 0; i < cgroup_entries.size(); i++) {
		auto &entry = cgroup_entries[i];
		auto &controller_list = entry.controller_list;
		if (entry.IsRoot()) {
			root_entry = i;
			continue;
		}
		for (auto &controller : controller_list) {
			if (controller == "cpu") {
				cpu_entry = i;
				break;
			}
		}
	}

	// TODO: we currently fall back to the root directory, because in virtual environments
	// the cgroups are often mapped to the root directory.
	// To properly handle this, we should parse and use the mapping in `/proc/self/mountinfo`
	if (cpu_entry.IsValid()) {
		auto &entry = cgroup_entries[cpu_entry.GetIndex()];
		auto res = GetCPUCountV1(entry.cgroup_path);
		if (res.IsValid()) {
			return res.GetIndex();
		}
		//! try falling back to the root directory
		res = GetCPUCountV1("");
		if (res.IsValid()) {
			return res.GetIndex();
		}
	}
	if (root_entry.IsValid()) {
		auto &entry = cgroup_entries[root_entry.GetIndex()];
		auto res = GetCPUCountV2(entry.cgroup_path);
		if (res.IsValid()) {
			return res.GetIndex();
		}
		//! try falling back to the root directory
		res = GetCPUCountV2("");
		if (res.IsValid()) {
			return res.GetIndex();
		}
	}
	return physical_cores;
}

#else

optional_idx CGroups::GetMemoryLimit() {
	return optional_idx();
}

idx_t CGroups::GetCPULimit(idx_t physical_cores) {
	return physical_cores;
}

#endif

} // namespace duckdb
