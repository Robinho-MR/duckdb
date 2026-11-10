//===----------------------------------------------------------------------===//
//                         DuckDB
//
// icu_stable_areas.hpp
//
// One registration function per area of the extension. Each lives in its own pair of files so the areas can be
// ported independently:
//   icu_datepart.cpp        date part extraction (icu-datepart.cpp)
//   icu_arithmetic.cpp      interval arithmetic, date_sub/date_diff, age (icu-dateadd.cpp, icu-datesub.cpp)
//   icu_trunc_bucket.cpp    date_trunc, time_bucket, make_timestamptz and the date/time casts (icu-datetrunc.cpp,
//                           icu-timebucket.cpp, icu-makedate.cpp, the remaining casts of icu-timezone.cpp)
//   icu_range.cpp           range / generate_series over timestamps (icu-table-range.cpp, icu-list-range.cpp)
//   icu_strptime.cpp        strptime, strftime and the string casts (icu-strptime.cpp)
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb_cpp_extension.hpp"

namespace icu_stable {

void RegisterDatePartFunctions(duckdb::cxx::Extension &extension, duckdb::cxx::Factory &factory);
void RegisterArithmeticFunctions(duckdb::cxx::Extension &extension, duckdb::cxx::Factory &factory);
void RegisterTruncBucketFunctions(duckdb::cxx::Extension &extension, duckdb::cxx::Factory &factory);
void RegisterRangeFunctions(duckdb::cxx::Extension &extension, duckdb::cxx::Factory &factory);
void RegisterStrptimeFunctions(duckdb::cxx::Extension &extension, duckdb::cxx::Factory &factory);

} // namespace icu_stable
