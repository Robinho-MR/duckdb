# Porting the in-tree `json` extension to the stable C++ API

Status: proposal / draft. This document does not change any code or spec. It lists what the port needs and the C API
additions it depends on, so they can be reviewed before any spec YAML is written.

Surveyed: `extension/json` (~11.7k lines, 8.2k in `json_functions/`) against `api_spec/v2` on `main`.

## 1. What already exists in the spec

| `json` feature | Spec module | Notes |
| --- | --- | --- |
| Scalar functions (`json_extract`, `json_type`, `json_keys`, `json_contains`, `json_exists`, `json_valid`, `json_pretty`, `json_merge_patch`, `json_deep_merge`, `json_strip_nulls`, `json_normalize`, `json_modify` family, `json_array_length`) | `function/scalar.yaml`, `vector/vector.yaml`, `value/value.yaml` | Constant-argument access in bind covers precompiled paths in `json_extract`. |
| `json_create`, `json_object`, `json_array`, `json_transform`, `from_json` | same + `logical_type`, `vector` (`vector_get_child`) | Recursive nested-type dispatch; needs full child-vector access. |
| `json_structure`, `json_group_structure`, `json_group_array`, `json_group_object` | `function/aggregate.yaml` | State/combine/finalize/destroy callbacks all present. |
| Casts (`VARCHAR`/`JSON`, `LIST(JSON)`, to/from nested types) | `function/cast.yaml` | `cast_function_set_implicit_cast_cost` replaces `CastFunctionSet::ImplicitCastCost`. `ReinterpretCast` becomes `vector_reference`. |
| `JSON` logical type | `logical_type/custom_type.yaml` | `custom_type_set_name` + `custom_type_set_base_type(VARCHAR)`. Verify the alias compares equal to the engine's `LogicalType::JSON()`. |
| `FROM 'x.json'` replacement scan | `function/replacement_scan.yaml` | |
| `COPY ... (FORMAT json)` and GeoJSON copy | `function/copy.yaml` | Both `copy_to_*` and `copy_from_*` exist. |
| `read_json`, `read_ndjson`, `read_json_objects`, `read_json_auto` | `function/table.yaml` (`multi_file_function_*`) | Single-file scan wrapped by `multi_file_function`, as in the ORC port. Globs, hive partitioning, `filename`, `union_by_name` come from the wrapper. |
| Projection pushdown, progress, partition data, claim-batch | `function/table.yaml` | `table_function_init_*_get_column_index` gives the projected columns. |
| File access (open, read_at, seek, size, glob) | `filesystem/file_system.yaml` | |

Rewrites without new API: the `JSONExecutors` templates (`json_executors.hpp`) and the yyjson allocator (backed by
`arena.yaml`) are rebuilt on the stable vector reader/writer.

## 2. Required C API additions

Signatures follow the spec's conventions (`duckdb_` prefix, `error_info` out-parameter, `unstable` lifecycle first).

### 2.1 In-out table functions (blocks `json_each`, `json_tree`)

`table.yaml` has no table function that consumes an input chunk from a lateral join. The engine side is
`TableFunction::in_out_function` / `in_out_function_final` with `OperatorResultType`.

```c
typedef enum {
	DUCKDB_IN_OUT_RESULT_INVALID = 0,
	DUCKDB_IN_OUT_RESULT_NEED_MORE_INPUT,  // input chunk fully consumed, send the next one
	DUCKDB_IN_OUT_RESULT_HAVE_MORE_OUTPUT, // output chunk is full, call again with the same input
	DUCKDB_IN_OUT_RESULT_FINISHED          // no more output wanted from this function
} duckdb_in_out_result;

// Borrowed handle passed to the in-out and in-out-final callbacks.
typedef struct _duckdb_table_function_in_out_info *duckdb_table_function_in_out_info;

typedef void (*duckdb_table_function_in_out_callback)(duckdb_table_function_in_out_info info,
                                                      duckdb_data_chunk_view input, duckdb_data_chunk output,
                                                      duckdb_error_info *err);

duckdb_error_info duckdb_table_function_set_in_out_callback(duckdb_table_function function,
                                                            duckdb_table_function_in_out_callback callback);
duckdb_error_info duckdb_table_function_set_in_out_final_callback(duckdb_table_function function,
                                                                  duckdb_table_function_in_out_callback callback);

void *duckdb_table_function_in_out_get_user_data(duckdb_table_function_in_out_info info);
void *duckdb_table_function_in_out_get_bind_data(duckdb_table_function_in_out_info info);
void *duckdb_table_function_in_out_get_global_state(duckdb_table_function_in_out_info info);
void *duckdb_table_function_in_out_get_local_state(duckdb_table_function_in_out_info info);
void duckdb_table_function_in_out_set_result(duckdb_table_function_in_out_info info, duckdb_in_out_result result);
```

Semantics to specify:

- A function with an in-out callback accepts non-constant arguments, which arrive as columns of `input`. Bind sees
  their types (and values when constant) through the existing `function_bind_info`.
- Mutually exclusive with the exec callback; setting both is an error at registration.
- The `input` row cursor is owned by the function: on `HAVE_MORE_OUTPUT` the engine calls again with the same input,
  so the function keeps its position in local state (as `JSONTableInOutLocalState` does today).
- The existing `table_function_set_progress_callback` stays valid for the single-input-row case that `json_each` uses.

### 2.2 Virtual columns on table functions (blocks `json_each`, `json_tree`)

`json_each` exposes `json`, `root`, `rowid` and an "empty" column through `get_virtual_columns`. The ORC port added a
fixed `FILE_ROW_NUMBER` virtual column on a fork branch; this generalizes it.

```c
#define DUCKDB_VIRTUAL_COLUMN_START (1ULL << 63) // first identifier in the virtual range

// Declares a virtual column at bind time; selectable by name, absent from `SELECT *`.
duckdb_error_info duckdb_table_function_bind_add_virtual_column(duckdb_table_function_bind_info info, idx_t identifier,
                                                                const char *name, duckdb_logical_type type);

// Identifiers with engine-defined meaning.
#define DUCKDB_COLUMN_IDENTIFIER_ROW_ID ...
#define DUCKDB_COLUMN_IDENTIFIER_EMPTY  ...
```

`table_function_init_*_get_column_index` already reports projected columns; it must be specified to return virtual
identifiers (>= `DUCKDB_VIRTUAL_COLUMN_START`) for virtual columns. If this lands together with the ORC
`FILE_ROW_NUMBER` change, that change becomes a reserved identifier on top of this.

### 2.3 Explicit file compression (blocks `read_json(compression=...)`, `COPY ... (COMPRESSION ...)`)

`file_open_options` has read/write/create flags and an untyped name/value hint, but no way to ask for gzip/zstd
(de)compression. JSON takes an explicit `compression` option and also auto-detects from the extension.

```c
typedef enum {
	DUCKDB_FILE_COMPRESSION_AUTO_DETECT = 0, // extension / magic bytes
	DUCKDB_FILE_COMPRESSION_NONE,
	DUCKDB_FILE_COMPRESSION_GZIP,
	DUCKDB_FILE_COMPRESSION_ZSTD
} duckdb_file_compression;

void duckdb_file_open_options_set_compression(duckdb_file_open_options options, duckdb_file_compression compression);
```

If the maintainers prefer no new enum, a reserved `file_open_options_set_value` name (`"compression"`) is the cheaper
alternative, at the cost of a stringly-typed contract.

### 2.4 SQL and plan serialization: no C API, move into core

`json_serialize_sql`, `json_deserialize_sql`, `json_execute_serialized_sql` (table function + pragma) and
`json_serialize_plan` use `Parser`, the generated statement `Serialize`/`Deserialize` code (`serialize_json.cpp`,
`JsonSerializer`/`JsonDeserializer`), `Planner`, `Binder` and `LogicalOperator` serialization. The JSON they emit is
the engine's internal, version-coupled serialization format.

Exposing the AST or plan through the stable API would freeze that format. Recommendation: **do not add C API for
this.** Move these functions, `JsonSerializer` and `JsonDeserializer` into core and drop them from the extension.
Side effects to review: the functions become available without loading `json` (autoload already makes this mostly
invisible), and core must link `duckdb_yyjson` for them.

Fallback if keeping them in the extension is required: `duckdb_statement_serialize_json(stmt, flags, &out)`,
`duckdb_statement_deserialize_json(conn, json, &stmt)` and `duckdb_connection_serialize_plan_json(conn, sql, flags,
&out)` in `sql_statement.yaml`, documented as returning an unstable, engine-defined format.

## 3. Checks before implementing

These are assumptions, not verified:

- `custom_type` alias equality with `LogicalType::JSON()`, and whether core code paths (CSV/Parquet type handling)
  key off the engine's JSON alias rather than the registered type.
- Nested-vector access is complete enough for `json_create`/`json_transform` (list entries, struct children, union,
  map, arrays) without flattening the whole input.
- `copy_from_*` can drive the same single-file scan as `read_json`, including `auto_detect` sampling.
- Multi-file wrapper handles JSON's `maximum_sample_files` and per-file type inference; `read_json` binds each file
  itself, so schema unification across files has to come from `union_by_name`.

## 4. Proposed order

1. Extension skeleton, loader, JSON custom type, vector/yyjson helper layer.
2. Scalar functions, starting with `json_extract`, `json_type`, `json_valid` as the vertical slice.
3. `json_create`, `json_transform`, casts, `json_structure` and the group aggregates.
4. `read_json` family as a single-file scan under `multi_file_function`; replacement scan; `COPY` to/from (needs 2.3).
5. API work 2.1 and 2.2, then `json_each` / `json_tree`.
6. Move serialization functions into core (2.4).

Steps 1-3 need no API changes. Steps 4, 5 and 6 each depend on one section above.
