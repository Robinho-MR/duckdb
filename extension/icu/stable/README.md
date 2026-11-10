# ICU on the stable C++ API (proof of concept)

`-DICU_STABLE_API=ON` builds the ICU extension from `stable/` instead of the legacy surface (`icu_extension.cpp`,
`icu-*.cpp`). The default build is unchanged.

The surface files (`icu_stable_extension.cpp`, `icu_datepart.cpp`, `icu_arithmetic.cpp`, `icu_trunc_bucket.cpp`,
`icu_range.cpp`, `icu_strptime.cpp`, `icu_timezone.cpp`) include only `duckdb_cpp_extension.hpp` and the std-only
`icu_bridge*.hpp` headers. The engine code (the vendored calendar and collation engine in `datetime/` and `collation/`,
and the per-value kernels copied from the legacy files, including `StrpTimeFormat`/`StrfTimeFormat` usage) lives in
`icu_bridge*.cpp`, which is the only place that includes DuckDB engine headers.

## What is ported

Everything the legacy extension registers:

| Area | Functions |
| --- | --- |
| Options | `TimeZone`, `Calendar` (validated and normalized in the update callback) |
| Table functions | `icu_calendar_names()`, `pg_timezone_names()`, `range` / `generate_series` over `TIMESTAMPTZ` (in-out, so column and lateral arguments work) |
| Collations | every locale collation, `icu_noaccent`, `icu_sort_key`, the hex `icu_collate_<locale>` functions (with serialize callbacks) |
| Casts | `TIMESTAMP`(_S/_MS/_NS), `DATE` <-> `TIMESTAMPTZ`(_NS), `TIMESTAMPTZ` -> `DATE` / `TIME WITH TIME ZONE`, `TIME` -> `TIME WITH TIME ZONE`, `VARCHAR` <-> `TIMESTAMPTZ`(_NS), `VARCHAR` -> `TIME WITH TIME ZONE`; all `CONSISTENT_WITHIN_QUERY`, `disable_timestamptz_casts` is checked when a cast is bound |
| Date parts | `date_part` (single and list of parts; a constant part is bound to the single-part function), every single-part function, `last_day`, `monthname`, `dayname`, `epoch`, `julian` |
| Arithmetic | `TIMESTAMPTZ` +/- `INTERVAL`, `TIMESTAMPTZ - TIMESTAMPTZ`, `date_sub`, `date_diff`, `age` |
| Truncation | `date_trunc`, `time_bucket`, `make_timestamptz` |
| Strings | `strptime`, `try_strptime`, `strftime` |
| Time zones | `timezone(zone, ts)` for all timestamp types and `TIMETZ`, `AT TIME ZONE`, `current_date`, `today`, `get_current_time`, `current_localtime`, `current_localtimestamp`, `disable_timestamptz_casts` |
| Lists | `range` / `generate_series` returning `TIMESTAMPTZ[]` |

Declared like the legacy functions: the monotonicity of `era`, `isoyear`, `date_sub`, `date_diff`, `date_trunc` and
`time_bucket` (with the bind-time clears for the parts that are not monotone) and the finite monotone bounds of the
statistics of `era`, `isoyear`, `date_sub` and `date_diff`.

## Differences from the legacy extension

- **`time_bucket` on `TIMESTAMPTZ_NS` with a non-constant width.** The legacy kernel reads the nanoseconds as
  microseconds; the port returns the correct bucket, so `test_icu_time_bucket_timestamptz_ns.test:483` expects the legacy
  result and differs.
- **Settings are read when a function runs**, where the legacy extension captured them when the call was bound. The
  functions that depend on them are `CONSISTENT_WITHIN_QUERY`, so the result only differs for a prepared statement that
  runs after `SET TimeZone` without being rebound.
- The engine still includes DuckDB headers in the bridge (`StringUtil`, exceptions, `vector`, the date/time helpers,
  `StrpTimeFormat`), so the extension is statically linked only.

## Tests

`test/sql/timezone/test_icu_stable_api.test` covers the core surface. The existing ICU tests (`test/sql/collate/`,
`test/sql/timezone/`, `test/sql/function/timestamp/`, `test/sql/function/date/`, `test/sql/table_function/`, the
strptime, strftime and csv timestamp tests, the index and prepared statement tests, ...) run against this build. In the
full default `unittest` run only the `time_bucket` case above fails (besides the loadable-extension tests, which need the
demo extensions built).
