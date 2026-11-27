cmake_minimum_required(VERSION 3.5...3.29)

project(ICUExtension)

# The icu extension is written against DuckDB's stable C++ API only
# (tools/cpp/duckdb_cpp.hpp): it includes nothing of the engine. It is built
# twice from the same sources: icu_extension, a static archive linked into the
# DuckDB binaries, and icu_loadable_extension (icu.duckdb_extension), which
# routes every engine call through the extension vtable.

# Drops the directory-wide version definition: the version is in the describe
# function and the loadable footer
set_extension_version_sources(icu)

set(ICU_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/collation/include
    ${CMAKE_CURRENT_LIST_DIR}/datetime/include
    ${DUCKDB_MODULE_BASE_DIR}/tools/cpp
    ${DUCKDB_MODULE_BASE_DIR}/third_party/zstd/include
    ${DUCKDB_MODULE_BASE_DIR}/third_party/jaro_winkler)

# The collation and calendar engines and the functions, shared by both flavors:
# they only use the header-only types of the C++ API
add_library(
  icu_sources OBJECT
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
  datetime/generated/tz_data.cpp
  icu_common.cpp
  icu-datetime.cpp
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
target_include_directories(icu_sources PRIVATE ${ICU_INCLUDE_DIRS})
set_target_properties(icu_sources PROPERTIES POSITION_INDEPENDENT_CODE ON)
disable_target_warnings(icu_sources)

set(ICU_EXTENSION_FILES icu_extension.cpp $<TARGET_OBJECTS:icu_sources>)

build_static_extension_capi_unstable(icu "" ${ICU_EXTENSION_FILES})
target_include_directories(icu_extension PRIVATE ${ICU_INCLUDE_DIRS})
target_link_libraries(icu_extension duckdb_cpp_api duckdb_zstd)
link_threads(icu_extension "")

if(NOT EMSCRIPTEN AND NOT WASM_LOADABLE_EXTENSIONS)
  build_loadable_extension_capi_unstable(icu "" ${ICU_EXTENSION_FILES})
  target_compile_definitions(icu_loadable_extension
                             PRIVATE DUCKDB_CPP_API_LOADABLE)
  target_include_directories(icu_loadable_extension PRIVATE ${ICU_INCLUDE_DIRS})
  target_link_libraries(icu_loadable_extension duckdb_cpp_api_loadable
                        duckdb_zstd)
endif()

install(
  TARGETS icu_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
