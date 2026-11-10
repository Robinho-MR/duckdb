//===----------------------------------------------------------------------===//
//                         DuckDB
//
// icu_bridge.hpp
//
// The boundary between the stable-API surface of the extension and the vendored calendar and collation engine. It only
// uses the standard library, so the surface never sees the engine headers (and the engine never sees duckdb_cpp.hpp).
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace icu_bridge {

//! Raised when the input is not well formed data, e.g. invalid UTF-8 handed to a collation
struct DataCorruptionError : public std::runtime_error {
	using std::runtime_error::runtime_error;
};

//! The parts of an instant a calendar can report
enum class Part : uint8_t {
	ERA,
	YEAR,
	MONTH,
	DAY,
	DAY_OF_WEEK,
	DAY_OF_YEAR,
	HOUR,
	MINUTE,
	SECOND,
	MILLISECOND,
	MICROSECOND,
	ZONE_OFFSET
};

//! Resolves a time zone name in place (case, UTC+NN offsets). Returns false and fills error for unknown zones.
bool ResolveTimeZone(std::string &name, std::string *error);
//! Resolves a calendar name in place. Returns false and fills error for unknown calendars.
bool ResolveCalendar(std::string &name, std::string *error);
//! The time zone of the host, UTC when unknown
std::string HostTimeZone();
std::vector<std::string> CalendarNames();

//! A calendar bound to a time zone. Instants are microseconds since the epoch.
class Calendar {
public:
	~Calendar();
	Calendar(const Calendar &) = delete;
	Calendar &operator=(const Calendar &) = delete;

	//! Both names must be resolved
	static std::unique_ptr<Calendar> Create(const std::string &time_zone, const std::string &calendar);
	void SetTimeZone(const std::string &time_zone);

	//! UTC instant -> wall clock microseconds of the same instant in the zone
	bool ToLocal(int64_t instant, int64_t &local);
	//! Wall clock microseconds -> UTC instant
	bool FromLocal(int64_t local, int64_t &instant);
	bool GetPart(int64_t instant, Part part, int64_t &result);
	//! Renders the instant as "YYYY-MM-DD HH:MM:SS[.ffffff]+HH[:MM[:SS]]" in the zone. False when the year is out of
	//! the range [1, 9999].
	bool Format(int64_t instant, std::string &result);

private:
	friend struct CalendarAccess;
	Calendar();
	struct Impl;
	std::unique_ptr<Impl> impl;
};

struct ZoneRow {
	std::string name;
	std::string abbrev;
	int64_t utc_offset_seconds = 0;
	bool is_dst = false;
};
std::vector<ZoneRow> ListTimeZones();

//! Sort keys of one collation
class Collation {
public:
	~Collation();
	Collation(const Collation &) = delete;
	Collation &operator=(const Collation &) = delete;

	//! A named collation (such as "de" or "de_AT"), or the tagged collation when the tag is not empty
	static std::unique_ptr<Collation> Create(const std::string &name, const std::string &tag);
	static std::vector<std::string> Names();

	//! Reusable buffers, one per executing thread
	class Buffer {
	public:
		Buffer();
		~Buffer();
		struct Impl;
		std::unique_ptr<Impl> impl;
	};
	//! Writes the sort key of the string, without the terminating byte, to key
	void SortKey(const char *data, size_t size, Buffer &buffer, const char *&key, size_t &key_size) const;

private:
	Collation();
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace icu_bridge
