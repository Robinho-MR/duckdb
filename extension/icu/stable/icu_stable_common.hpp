//===----------------------------------------------------------------------===//
//                         DuckDB
//
// icu_stable_common.hpp
//
// Helpers shared by the files of the stable-API surface. Includes only the C++ API and the std-only bridge.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb_cpp_extension.hpp"

#include "icu_bridge.hpp"

#include <cstdint>
#include <limits>
#include <string>

namespace cxx = duckdb::cxx;

namespace icu_stable {

using cxx::idx_t;

constexpr int64_t MICROS_PER_DAY = 86400LL * 1000000LL;

//! Timestamps use the extreme int64 values as -infinity and infinity
inline bool IsFinite(int64_t micros) {
	return micros != std::numeric_limits<int64_t>::max() && micros != -std::numeric_limits<int64_t>::max();
}

//! Throws a conversion error
[[noreturn]] void ThrowConversion(const std::string &message);

//! The failed rows of a cast. A normal cast throws at the first one. A try cast writes NULL for them and reports the
//! first failure with Report() once the batch is written, which makes the cast return "not every row converted".
class CastFailures {
public:
	explicit CastFailures(cxx::CastFunction::ExecInput &input) : try_mode(input.GetMode() == cxx::CastMode::TRY) {
	}

	void Fail(cxx::Vector &out, idx_t row, const std::string &message) {
		if (!try_mode) {
			ThrowConversion(message);
		}
		out.SetNull(row);
		if (!failed) {
			failed = true;
			first_message = message;
		}
	}

	void Report() const {
		if (failed) {
			ThrowConversion(first_message);
		}
	}

private:
	bool try_mode;
	bool failed = false;
	std::string first_message;
};

//! The TimeZone and Calendar settings of a context
struct Settings {
	std::string time_zone = "UTC";
	std::string calendar = "gregorian";

	bool operator==(const Settings &other) const {
		return time_zone == other.time_zone && calendar == other.calendar;
	}
};

Settings ReadSettings(cxx::Context &context);

//! The calendar of the settings, cached per thread because creating one parses the time zone. The reference is valid
//! until the next call on the same thread.
icu_bridge::Calendar &SettingsCalendar(const Settings &settings);

//! The calendar of an arbitrary time zone name (using the calendar of the settings), cached per thread. Returns nullptr
//! and fills the error for an unknown zone.
icu_bridge::Calendar *ZoneCalendar(const Settings &settings, const std::string &zone, std::string &error);

} // namespace icu_stable
