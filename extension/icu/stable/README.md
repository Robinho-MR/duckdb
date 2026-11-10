# ICU on the stable C++ API (proof of concept)

`-DICU_STABLE_API=ON` builds the ICU extension from `stable/` instead of the legacy surface (`icu_extension.cpp`,
`icu-*.cpp`). The default build is unchanged.

`icu_stable_extension.cpp` includes only `duckdb_cpp_extension.hpp`. The vendored calendar and collation engine
(`datetime/`, `collation/`) is reached through `icu_bridge.hpp`, which only uses the standard library.

## What is ported

| Area | Functions |
| --- | --- |
| Options | `TimeZone`, `Calendar` (validated and normalized in the update callback) |
| Table functions | `icu_calendar_names()`, `pg_timezone_names()` |
| Collations | every locale collation, `icu_noaccent`, `icu_sort_key`, the hex `icu_collate_<locale>` functions |
| Casts | `TIMESTAMP <-> TIMESTAMPTZ`, `TIMESTAMPTZ -> VARCHAR` |
| Scalars | `timezone(zone, ts)` (both directions, `AT TIME ZONE`), `era`, `year`, `month`, `day`, `dayofweek`, `dayofyear`, `hour`, `minute`, `second`, `millisecond`, `microsecond` on `TIMESTAMPTZ`, `current_date`, `today`, `get_current_time` |

## What is not

- `date_trunc`, `date_add`/`date_sub`/`date_diff`, `+ INTERVAL`, `age`, `time_bucket`, `strptime`/`strftime`, `make_timestamptz`,
  `date_part`/`extract`, the range functions, the `TIMESTAMP_S/MS/NS` and `TIMESTAMPTZ_NS` casts, `VARCHAR -> TIMESTAMPTZ`
  and the `disable_timestamptz_casts` check at bind time.
- Plan serialization of the collation functions: stable scalar functions have no serialize callbacks.
- Years outside [1, 9999] when casting `TIMESTAMPTZ` to `VARCHAR`.
- Invalid UTF-8 in a collation input surfaces as a generic error, the stable API has no data corruption error type.
- The engine still includes DuckDB headers (`StringUtil`, exceptions, `vector`), so the extension is statically linked
  only. Replacing those with standard library equivalents is what makes a loadable build possible.

## Tests

`test/sql/timezone/test_icu_stable_api.test` covers the ported surface. The existing `test/sql/collate/` tests pass
except the invalid UTF-8 error type.
