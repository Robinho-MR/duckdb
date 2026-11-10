//===----------------------------------------------------------------------===//
//                         DuckDB
//
// icu_bridge_range.hpp
//
// Calendar stepping used by range and generate_series over TIMESTAMPTZ. Std-only.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "icu_bridge.hpp"

#include <cstdint>

namespace icu_bridge {

//! Adds a calendar interval to an instant in the zone of the calendar. Infinite instants are returned as is. Returns
//! false when the result does not fit a timestamp.
bool RangeAddInterval(Calendar &calendar, int64_t instant, int32_t months, int32_t days, int64_t micros,
                      int64_t &result);

} // namespace icu_bridge
