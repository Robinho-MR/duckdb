add_library_unity(duckdb_func_scalar_serialize OBJECT json_serialize_plan.cpp
                  json_serialize_sql.cpp serialize_functions.cpp)
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:duckdb_func_scalar_serialize>
    PARENT_SCOPE)
