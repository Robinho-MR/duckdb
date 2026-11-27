cmake_minimum_required(VERSION 3.5...3.29)

project(JSONExtension)

# Drops the directory-wide version definition: the version is in the describe
# function and the loadable footer
set_extension_version_sources(json)

# The json extension is written against DuckDB's stable C++ API only
# (tools/cpp/duckdb_cpp.hpp): it includes nothing of the engine, and links the
# C++ API archive. It is built twice from the same sources: * json_extension: a
# static archive that is linked into the DuckDB binaries
# (duckdb_extension_statically_link), registered through the V2 describe
# function the build generates for it * json_loadable_extension:
# json.duckdb_extension, which routes every engine call through the extension
# vtable

set(JSON_CORE_SOURCES
    src/json_extension.cpp src/json_common.cpp src/json_function_data.cpp
    src/json_types.cpp src/functions/register_core.cpp)

# Read-only scalar functions: json_type, json_valid, json_exists, json_extract*,
# json_value, json_array_length, ...
set(JSON_SCALAR_READ_SOURCES
    src/functions/register_scalar_read.cpp
    src/functions/json_array_length.cpp
    src/functions/json_exists.cpp
    src/functions/json_extract.cpp
    src/functions/json_type.cpp
    src/functions/json_valid.cpp
    src/functions/json_value.cpp
    src/functions/json_keys.cpp
    src/functions/json_contains.cpp
    src/functions/json_pretty.cpp)

# json_merge_patch, json_deep_merge, json_insert/replace/set/remove,
# json_strip_nulls, json_pretty, ...
set(JSON_SCALAR_MODIFY_SOURCES
    src/functions/register_scalar_modify.cpp
    src/functions/json_merge_patch.cpp
    src/functions/json_merge_patch_diff.cpp
    src/functions/json_deep_merge.cpp
    src/functions/json_modify.cpp
    src/functions/json_strip_nulls.cpp
    src/functions/json_normalize.cpp)

# json_create/to_json/json_object/json_array, json_group_array/json_group_object
# and their casts
set(JSON_CREATE_SOURCES
    src/functions/register_create.cpp src/functions/json_create.cpp
    src/functions/json_strftime.cpp)

# json_transform/from_json and the casts from JSON to nested types
set(JSON_TRANSFORM_SOURCES src/functions/register_transform.cpp
                           src/functions/json_transform.cpp)

# json_structure, json_group_structure, schema inference
set(JSON_STRUCTURE_SOURCES
    src/functions/register_structure.cpp src/functions/json_structure.cpp
    src/functions/json_schema_inference.cpp)

# read_json and friends, the replacement scan, the settings
set(JSON_READER_SOURCES
    src/functions/register_reader.cpp src/functions/json_date_formats.cpp
    src/functions/json_reader.cpp src/functions/read_json.cpp
    src/functions/copy_json_from.cpp)

# json_each, json_tree
set(JSON_TABLE_INOUT_SOURCES src/functions/register_table_inout.cpp)

# COPY ... (FORMAT json), GeoJSON
set(JSON_COPY_SOURCES
    src/functions/register_copy.cpp src/functions/json_copy.cpp
    src/functions/json_geojson.cpp)

set(JSON_EXTENSION_FILES
    ${JSON_CORE_SOURCES}
    ${JSON_SCALAR_READ_SOURCES}
    ${JSON_SCALAR_MODIFY_SOURCES}
    ${JSON_CREATE_SOURCES}
    ${JSON_TRANSFORM_SOURCES}
    ${JSON_STRUCTURE_SOURCES}
    ${JSON_READER_SOURCES}
    ${JSON_TABLE_INOUT_SOURCES}
    ${JSON_COPY_SOURCES})

set(JSON_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/src/include ${DUCKDB_MODULE_BASE_DIR}/tools/cpp
    ${DUCKDB_MODULE_BASE_DIR}/third_party/yyjson/include)

# Static flavor: part of the DuckDB binaries, which also carry yyjson
build_static_extension_capi_unstable(json "" ${JSON_EXTENSION_FILES})
target_include_directories(json_extension PRIVATE ${JSON_INCLUDE_DIRS})
target_link_libraries(json_extension duckdb_cpp_api)

# Loadable flavor
if(NOT EMSCRIPTEN AND NOT WASM_LOADABLE_EXTENSIONS)
  build_loadable_extension_capi_unstable(json "" ${JSON_EXTENSION_FILES})
  target_compile_definitions(json_loadable_extension
                             PRIVATE DUCKDB_CPP_API_LOADABLE)
  target_include_directories(json_loadable_extension
                             PRIVATE ${JSON_INCLUDE_DIRS})
  target_link_libraries(json_loadable_extension duckdb_cpp_api_loadable
                        duckdb_yyjson)
endif()

install(
  TARGETS json_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
