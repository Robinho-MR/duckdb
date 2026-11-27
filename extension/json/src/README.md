# json extension on the stable C++ API

`extension/json/src` is the json extension, written against the stable C++ API `tools/cpp/duckdb_cpp.hpp` (namespace
`duckdb::cxx`). Rules: include `duckdb_cpp.hpp` (+ `duckdb_cpp_extension.hpp` in `json_extension.cpp` only) and `yyjson.hpp`, never a
`duckdb/...` header. Code is in `namespace duckdb::json`, which `using`-declares the `cxx` types it needs.

## Layout and build

- `CMakeLists.txt`: one `JSON_<AREA>_SOURCES` block per area (CORE, SCALAR_READ, SCALAR_MODIFY, CREATE, TRANSFORM,
  STRUCTURE, READER, TABLE_INOUT, COPY).
- Built twice from the same sources: `json_extension` (static archive, linked into `duckdb`/`unittest`, registered by the
  generated V2 describe function) and `json_loadable_extension` (`json.duckdb_extension`, `DUCKDB_CPP_API_LOADABLE`, every
  engine call goes through the vtable). Don't use anything flavor specific outside `json_extension.cpp`.
- `functions/register_<area>.cpp` defines `void Register<Area>(Extension &, Context &)`, called by `json_extension.cpp`.
  A package owns its register file and the files it adds; no shared file needs editing.
- `include/`: `json_common.hpp/.cpp` (JSONCommon: flags, parsing, paths, `Error` classes, key maps),
  `json_allocator.hpp` (yyjson arena), `json_vector.hpp` (readers/writers), `json_functions.hpp` (local state, bind data,
  registration helpers, all `Register*` declarations), `json_executors.hpp` (`JSONExecutors`).

Build and test (repo root): `make reldebug`, then e.g. `build/reldebug/test/unittest "test/sql/json/*"`. Syntax check
one file: `g++ -std=c++17 -fsyntax-only -Iextension/json/src/include -Itools/cpp -Ithird_party/yyjson/include f.cpp`. To
test the loadable flavor, configure with `-DSTATICALLY_LINK_EXTENSIONS=core_functions;parquet;icu` (json no longer linked):
`LOAD json` then loads `json.duckdb_extension`.

## Idioms

**Scalar read function.** Write a callable `std::optional<R>(yyjson_val *, yyjson_alc *)`, wrap it in exec callbacks
that call `JSONExecutors::{UnaryExecute,BinaryExecute,ExecuteMany}<R>(input, fun)`, register with
`RegisterJSONReadFunctions(...)` (VARCHAR and JSON input, `(json)`, `(json, path)`, `(json, index)`, `(json, [paths])`
overloads; see `functions/json_type.cpp`, the shortest example). R is `bool`, a number, `std::string_view` (copied into
the result, so it may point into the document or the arena) or `ListResult<E>`. Other shapes: use
`RegisterJSONReadFunction` once per overload, or the plain `ScalarFunction` API (`ScalarFunction::Create(factory)`,
`WithSignature`, `SetExecCallback`, `extension.Register(f)`); re-`SetName`/`SetUserData` and register again for aliases.
Callbacks are plain function pointers, not `std::function`: state goes through user data / bind data / init data.

**Local state.** `function.SetInitCallback(JSONFunctionLocalState::Init)`, then
`JSONFunctionLocalState::ResetAndGet(input)` at the top of exec gives the thread's `JSONAllocator`
(`.json_allocator.GetYYAlc()`); everything yyjson allocated for the previous batch is freed by the reset. Cast
callbacks have no local state: use `ThreadLocalJSONAllocator()` and `Reset()` it yourself.

**Bind data.** `BindInput::SetBindData<T>(args...)` in the bind callback, `input.GetBindData<T>()` (const) in exec. Give
`T` an `operator==` (the engine compares bind data). `JSONReadFunctionData` is the constant-path data. Arguments:
`TryGetConstantArgument(i)`, `GetArgType(i)`. A return type that depends on arguments: declare `ANY`, set a resolve-types
callback and call `SetReturnType` (see `JSONReadFunctionData::ResolveTypes`; it reads the return type from user data).

**Reading vectors.** `ValueView<T>(vec, offset)`: `IsValid(i)`, `Get(i)`; handles FLAT/CONSTANT/DICTIONARY (and flattens
`OTHER`). T is the element type: `varchar_t` (`.view()`, `.data_mut()` for yyjson), `int64_t`, `list_entry_t`, ... Nested:
LIST rows are `ValueView<list_entry_t>`, the elements `ValueView<T>(vec.GetChild(0))` indexed `offset + j`; STRUCT
fields are `vec.GetChild(k)` indexed by the parent's `SourceRow(i)`. Flatten a nested input (`vec.Flatten()`) before
`GetChild` if it is not FLAT.

**Writing vectors.** `ResultWriter<R>(result, count)` + `Write(optional<R>)` + `Finish()` (always call Finish),
or `FlatWriter<T>` / `StringWriter` for one vector (they `SetSize` it and manage the validity mask). Strings are copied
into the vector's heap (`StringWriter::WriteValue` skips UTF-8 validation: only pass text yyjson produced, else
`WriteValidated`). Lists: parent entries first, then `GetChild(0)`'s `SetSize(total)` and fill (`ListResult` does it).

**Errors.** Throw: `InvalidInputException`, `BinderException`, `ConversionException`, `NotImplementedException`,
`InternalException` (all in `json_common.hpp`, over `cxx::Exception` + an error code, so the engine reports the right
error type). Use `BinderException` in bind/resolve callbacks. In `CastMode::TRY` write NULL instead of throwing.

## Notes on the API

- `Factory::CreateType(SQLNULL)` is refused; the NULL type is `ParseType("\"NULL\"")` (used for the NULL->JSON cast).
- `Vector::Reference` needs identical types, alias included: JSON->VARCHAR cannot be a zero-copy reinterpret, strings are
  copied. Likewise there is no way to keep memory outside the vector alive for a result (legacy `AddBuffer`): every string
  result is copied once more.
- Cast callbacks have no per-thread state: they use a thread_local arena.
- `json_each`/`json_tree` are table in-out functions: an `Operator` set with `TableFunction::SetOperator`, whose input
  rows are the arguments. The read_json single-file functions (except `read_single_json_file`) are handed to their
  `MultiFileFunction` unregistered, so they are not callable on their own.
- `json_serialize_sql`, `json_deserialize_sql`, `json_execute_serialized_sql` and `json_serialize_plan` produce the
  engine's internal AST/plan format, so they are builtin functions of the engine rather than part of this extension.
