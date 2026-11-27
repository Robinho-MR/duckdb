//===----------------------------------------------------------------------===//
//                         DuckDB
//
// icu-casts.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "icu-datefunc.hpp"
#include "icu-vector.hpp"

namespace duckdb_icu {

struct ICUCasts {
	//! Whether the `disable_timestamptz_casts` setting is on in the context
	static bool TimestampTZCastsDisabled(cxx::Context &context);

	//! The bind of a cast that depends on the time zone settings: needs a context, and reads the settings into the bind
	//! data. `disabled_message` refuses the cast when the casts without an explicit time zone are disabled.
	static void BindCastData(cxx::CastFunction::BindInput &input, const char *what,
	                         const string &disabled_message = string());

	//! Registers a cast that depends on the time zone settings. With `implicit`, the cast keeps the cost the binder
	//! gave the built-in conversion between the types.
	static void Register(const ICURegistration &reg, const string &source, const string &target, bool implicit,
	                     cxx::CastFunction::BindCallback bind, cxx::CastFunction::ExecCallback exec,
	                     cxx::CastFunction::InitCallback init = nullptr);

	//! The init of a cast that resolves the time zones of its rows: a calendar cache per thread
	static void InitCalendarCache(cxx::CastFunction::InitInput &input);
};

//! Writes the rows of a cast's output, NULLs and failures included. A failure in a TRY_CAST leaves the row NULL and is
//! reported once all rows are written, which makes the engine see that not every row converted.
template <class WRITER>
class CastRows {
public:
	explicit CastRows(cxx::CastFunction::ExecInput &input)
	    : output(input.GetOutput()), count(input.GetRowCount()), writer(output, count),
	      is_try(input.GetMode() == cxx::CastMode::TRY) {
	}

	WRITER &Writer() {
		return writer;
	}
	idx_t Count() const {
		return count;
	}
	void SetNull(idx_t row) {
		writer.SetNull(row);
	}
	void Fail(idx_t row, const string &message) {
		if (!is_try) {
			throw ConversionException(message);
		}
		writer.SetNull(row);
		if (first_error.empty()) {
			first_error = message;
		}
	}
	void Finish() {
		if (!first_error.empty()) {
			throw ConversionException(first_error);
		}
	}

private:
	cxx::Vector output;
	idx_t count;
	WRITER writer;
	bool is_try;
	string first_error;
};

//! Converts every row of a cast: `fun(value, result, error)` returns true for a converted row, false with an error
//! for a failure, and false without one for a NULL
template <class SRC, class DST, class FUN>
void ExecuteCast(cxx::CastFunction::ExecInput &input, FUN &&fun) {
	CastRows<ResultWriter<DST>> rows(input);
	typename ArgReaderOf<SRC>::type values(input.GetInput());
	string error;
	for (idx_t row = 0; row < rows.Count(); row++) {
		if (!values.IsValid(row)) {
			rows.SetNull(row);
			continue;
		}
		DST result;
		error.clear();
		if (fun(values.Get(row), result, error)) {
			rows.Writer().Set(row, result);
		} else if (!error.empty()) {
			rows.Fail(row, error);
		} else {
			rows.SetNull(row);
		}
	}
	rows.Finish();
}

struct ICUMakeDate : public ICUDateFunc {
	static date_t Operation(Calendar *calendar, timestamp_tz_t instant);
	static date_t ToDate(cxx::Context &context, timestamp_tz_t instant);
	static void AddCasts(const ICURegistration &reg);
};

struct ICUToTimeTZ : public ICUDateFunc {
	static dtime_tz_t Operation(Calendar *calendar, dtime_tz_t timetz);
	//! Returns false if the instant cannot be converted. If no error_message is provided, an out-of-range time zone
	//! offset throws, otherwise the error is written to error_message.
	static bool ToTimeTZ(Calendar *calendar, timestamp_tz_t instant, dtime_tz_t &result,
	                     string *error_message = nullptr);
	static void AddCasts(const ICURegistration &reg);
};

} // namespace duckdb_icu
