# ICU on the stable C++ API (proof of concept)

`-DICU_STABLE_API=ON` builds the ICU extension from `stable/` instead of the legacy surface (`icu_extension.cpp`,
`icu-*.cpp`). The default build is unchanged.

The surface files (`icu_stable_extension.cpp`, `icu_datepart.cpp`, `icu_arithmetic.cpp`, `icu_trunc_bucket.cpp`,
`icu_range.cpp`, `icu_strptime.cpp`, `icu_timezone.cpp`) include only `duckdb_cpp_extension.hpp` and the std-only
`icu_bridge*.hpp` headers. The engine code (the vendored calendar and collation engine in `datetime/` and `collation/`,
and the per-value kernels copied from the legacy files, including `StrpTimeFormat`/`StrfTimeFormat` usage) lives in
`icu_bridge*.cpp`, which is the only place that includes DuckDB engine headers.

## What is ported

| Area | Functions |
| --- | --- |
| Options | `TimeZone`, `Calendar` (validated and normalized in the update callback) |
| Table functions | `icu_calendar_names()`, `pg_timezone_names()`, `range` / `generate_series` over `TIMESTAMPTZ` |
| Collations | every locale collation, `icu_noaccent`, `icu_sort_key`, the hex `icu_collate_<locale>` functions |
| Casts | `TIMESTAMP`(_S/_MS/_NS), `DATE` <-> `TIMESTAMPTZ`(_NS), `TIMESTAMPTZ` -> `DATE` / `TIME WITH TIME ZONE`, `TIME` -> `TIME WITH TIME ZONE`, `VARCHAR` <-> `TIMESTAMPTZ`(_NS), `VARCHAR` -> `TIME WITH TIME ZONE` |
| Date parts | `date_part` (single and list of parts), every single-part function, `last_day`, `monthname`, `dayname`, `epoch`, `julian` |
| Arithmetic | `TIMESTAMPTZ` +/- `INTERVAL`, `TIMESTAMPTZ - TIMESTAMPTZ`, `date_sub`, `date_diff`, `age` |
| Truncation | `date_trunc`, `time_bucket`, `make_timestamptz` |
| Strings | `strptime`, `try_strptime`, `strftime` |
| Time zones | `timezone(zone, ts)` for all timestamp types and `TIMETZ`, `AT TIME ZONE`, `current_date`, `today`, `get_current_time`, `current_localtime`, `current_localtimestamp`, `disable_timestamptz_casts` |
| Lists | `range` / `generate_series` returning `TIMESTAMPTZ[]` |

## Differences from the legacy extension

These are the gaps of the stable API that the port runs into. Each of them is a missing capability, not a missing port.

- **In-out table functions.** The stable `TableFunction` takes constant arguments only, so `range(a, b, c)` over
  `TIMESTAMPTZ` columns (`FROM t, range(t.s, t.e, ...)`, lateral and non-foldable arguments) fails once the ICU overload
  is registered. The scalar list forms work with columns.
- **Cast bind callbacks.** A cast cannot declare itself as depending on a setting (`CONSISTENT_WITHIN_QUERY`), so a cast of a
  constant can be folded and reused after `SET TimeZone` (`prepared_time_zone.test`) and the casts are accepted in index
  expressions (`create_index_time_zone_dependent.test`). A disabled cast (`disable_timestamptz_casts`) is reported when
  the cast executes, not when it is bound.
- **Argument properties and statistics bounds.** `date_trunc`, `time_bucket`, `date_sub`, `date_diff`, `era` and
  `isoyear` lose the monotonicity hints (`PropagateFiniteMonotoneBounds`). Results are the same.
- **Per-bind stability.** `date_part('epoch', x)` stays `CONSISTENT_WITHIN_QUERY` (the legacy marks it `CONSISTENT`).
- **Bind-time rewrite.** The legacy `datepart('epoch', ts)` is bound to the `epoch` function; here it stays `datepart`
  (`icu_datepart_roundtrip.test` expects the former in `EXPLAIN (SQL)` output).
- **Plan serialization** of the collation functions: stable scalar functions have no serialize callbacks.
- **`time_bucket` on `TIMESTAMPTZ_NS` with a non-constant width.** The legacy kernel reads the nanoseconds as microseconds;
  the port returns the correct bucket, so `test_icu_time_bucket_timestamptz_ns.test:483` differs.
- The engine still includes DuckDB headers (`StringUtil`, exceptions, `vector`, the date/time helpers), so the extension
  is statically linked only. Replacing those with standard library equivalents is what makes a loadable build possible.

## Tests

`test/sql/timezone/test_icu_stable_api.test` covers the core surface. The existing ICU tests (`test/sql/collate/`,
`test/sql/timezone/`, `test/sql/function/timestamp/`, `test/sql/function/date/`, `test/sql/table_function/`, the
strptime, strftime and csv timestamp tests, ...) run against this build, the failures are the points above.
