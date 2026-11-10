//===----------------------------------------------------------------------===//
//                         DuckDB
//
// icu_bridge_internal.hpp
//
// Engine-side view of the bridge types, for the bridge implementation files only (icu_bridge*.cpp). The surface files
// never include this header.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "icu_bridge.hpp"

#include "calendar.hpp"

namespace icu_bridge {

struct Calendar::Impl {
	std::unique_ptr<duckdb::datetime::Calendar> calendar;
	//! The (resolved) names the calendar was created with
	std::string time_zone;
	std::string calendar_name;
};

//! Access to the engine calendar of a bridge calendar
struct CalendarAccess {
	static duckdb::datetime::Calendar &Native(Calendar &calendar) {
		return *calendar.impl->calendar;
	}
	static const std::string &TimeZoneName(const Calendar &calendar) {
		return calendar.impl->time_zone;
	}
	static const std::string &CalendarName(const Calendar &calendar) {
		return calendar.impl->calendar_name;
	}
};

} // namespace icu_bridge
