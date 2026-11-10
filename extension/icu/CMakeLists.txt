cmake_minimum_required(VERSION 2.8.12...3.29)

project(ICUExtension)

# Before any subdirectory or target, which would otherwise inherit the
# directory-wide definition
option(ICU_STABLE_API
       "Build the ICU extension on the stable C++ API (proof of concept)" OFF)

if(ICU_STABLE_API)
  set_extension_version_sources(icu stable/icu_stable_extension.cpp)
else()
  set_extension_version_sources(icu icu_extension.cpp)
endif()

include_directories(include)
include_directories(collation/include)
include_directories(datetime/include)
include_directories(${CMAKE_SOURCE_DIR}/third_party/zstd/include)

set(ICU_ENGINE_FILES
    collation/collation_collator.cpp
    collation/collation_loader.cpp
    collation/collation_normalizer.cpp
    collation/generated/collation_data.cpp
    datetime/astronomy.cpp
    datetime/calendar.cpp
    datetime/chinese.cpp
    datetime/coptic.cpp
    datetime/grego.cpp
    datetime/gregorian.cpp
    datetime/hebrew.cpp
    datetime/islamic.cpp
    datetime/japanese.cpp
    datetime/persian.cpp
    datetime/timezone.cpp
    datetime/tz_loader.cpp
    datetime/generated/tz_data.cpp)

set(ICU_EXTENSION_FILES
    ${ICU_ENGINE_FILES}
    icu_extension.cpp
    icu-current.cpp
    icu-dateadd.cpp
    icu-datefunc.cpp
    icu-datepart.cpp
    icu-datesub.cpp
    icu-datetrunc.cpp
    icu-makedate.cpp
    icu-list-range.cpp
    icu-table-range.cpp
    icu-strptime.cpp
    icu-timebucket.cpp
    icu-timezone.cpp)

if(ICU_STABLE_API)
  # Only the surface is written against the stable API, the calendar and
  # collation engine is still compiled from its sources
  set(ICU_STABLE_FILES ${ICU_ENGINE_FILES} stable/icu_bridge.cpp
                       stable/icu_stable_extension.cpp)
  build_static_extension_capi_unstable(icu ${ICU_STABLE_FILES})
  disable_target_warnings(icu_extension)
  target_include_directories(icu_extension PRIVATE stable)
  target_link_libraries(icu_extension duckdb_cpp_api duckdb_zstd)
  link_threads(icu_extension "")
else()
  build_extension_library(icu NO_WARNINGS ${ICU_EXTENSION_FILES})
  link_threads(icu_extension "")
  target_link_libraries(icu_extension duckdb_zstd)
  target_link_libraries(icu_loadable_extension duckdb_zstd)
endif()
install(
  TARGETS icu_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
