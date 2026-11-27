#include "icu-datefunc.hpp"
#include "icu-vector.hpp"

namespace duckdb_icu {

struct ICUTimeBucket : public ICUDateFunc {
	// Use 2000-01-03 00:00:00 (Monday) as origin when bucket_width is days, hours, ... for TimescaleDB compatibility
	// There are 10959 days between 1970-01-01 and 2000-01-03
	constexpr static const int64_t DEFAULT_ORIGIN_MICROS_1 = 10959 * Interval::MICROS_PER_DAY;
	// Use 2000-01-01 as origin when bucket_width is months, years, ... for TimescaleDB compatibility
	// There are 10957 days between 1970-01-01 and 2000-01-01
	constexpr static const int64_t DEFAULT_ORIGIN_MICROS_2 = 10957 * Interval::MICROS_PER_DAY;

	enum struct BucketWidthType { CONVERTIBLE_TO_MICROS, CONVERTIBLE_TO_DAYS, CONVERTIBLE_TO_MONTHS, UNCLASSIFIED };

	static inline BucketWidthType ClassifyBucketWidth(const interval_t bucket_width) {
		if (bucket_width.months == 0 && bucket_width.days == 0 && bucket_width.micros > 0) {
			return BucketWidthType::CONVERTIBLE_TO_MICROS;
		} else if (bucket_width.months == 0 && bucket_width.days >= 0 && bucket_width.micros == 0) {
			return BucketWidthType::CONVERTIBLE_TO_DAYS;
		} else if (bucket_width.months > 0 && bucket_width.days == 0 && bucket_width.micros == 0) {
			return BucketWidthType::CONVERTIBLE_TO_MONTHS;
		} else {
			return BucketWidthType::UNCLASSIFIED;
		}
	}

	static inline BucketWidthType ClassifyBucketWidthErrorThrow(const interval_t bucket_width) {
		if (bucket_width.months == 0 && bucket_width.days == 0) {
			if (bucket_width.micros <= 0) {
				throw NotImplementedException("Period must be greater than 0");
			}
			return BucketWidthType::CONVERTIBLE_TO_MICROS;
		} else if (bucket_width.months == 0 && bucket_width.micros == 0) {
			if (bucket_width.days <= 0) {
				throw NotImplementedException("Period must be greater than 0");
			}
			return BucketWidthType::CONVERTIBLE_TO_DAYS;
		} else if (bucket_width.days == 0 && bucket_width.micros == 0) {
			if (bucket_width.months <= 0) {
				throw NotImplementedException("Period must be greater than 0");
			}
			return BucketWidthType::CONVERTIBLE_TO_MONTHS;
		} else if (bucket_width.months == 0) {
			throw NotImplementedException("Day intervals cannot have time component");
		} else {
			throw NotImplementedException("Month intervals cannot have day or time component");
		}
	}

	static inline timestamp_tz_t WidthConvertibleToMicrosCommon(int64_t bucket_width_micros, const timestamp_tz_t ts,
	                                                            const timestamp_tz_t origin, TZCalendar &calendar) {
		if (!bucket_width_micros) {
			throw OutOfRangeException("Can't bucket using zero microseconds");
		}
		int64_t ts_micros = SubtractOverflowCheck(ts.micros, origin.micros);
		int64_t result_micros = (ts_micros / bucket_width_micros) * bucket_width_micros;
		if (ts_micros < 0 && ts_micros % bucket_width_micros != 0) {
			result_micros = SubtractOverflowCheck(result_micros, bucket_width_micros);
		}

		return Add(calendar, origin, Interval::Make(0, 0, result_micros));
	}

	//! Add resolves a local time that occurs twice (e.g. when the clocks are turned back) to its latter occurrence.
	//! If that is after ts, ts lies between the two occurrences, and its bucket starts at the former occurrence.
	static timestamp_tz_t FormerOccurrence(Calendar *calendar, const timestamp_tz_t bucket, const timestamp_tz_t ts) {
		auto get_offset = [&](const timestamp_tz_t instant) {
			SetTime(calendar, instant);
			return int64_t(ExtractField(calendar, CAL_ZONE_OFFSET)) + ExtractField(calendar, CAL_DST_OFFSET);
		};
		const auto ts_offset = get_offset(ts);
		const auto bucket_offset = get_offset(bucket);
		if (ts_offset <= bucket_offset) {
			return bucket;
		}
		const timestamp_tz_t former {bucket.micros - (ts_offset - bucket_offset) * Interval::MICROS_PER_MSEC};
		if (ts.micros < former.micros || get_offset(former) != ts_offset) {
			return bucket;
		}
		return former;
	}

	static inline timestamp_tz_t WidthConvertibleToDaysCommon(int32_t bucket_width_days, const timestamp_tz_t ts,
	                                                          const timestamp_tz_t origin, TZCalendar &calendar) {
		if (!bucket_width_days) {
			throw OutOfRangeException("Can't bucket using zero days");
		}
		const auto sub_days = SubtractFactory(DatePartSpecifier::DAY);

		int64_t ts_days = sub_days(calendar.GetCalendar(), origin, ts);
		int64_t result_days = (ts_days / bucket_width_days) * bucket_width_days;
		if (result_days < NumericLimits<int32_t>::Minimum() || result_days > NumericLimits<int32_t>::Maximum()) {
			throw OutOfRangeException("Timestamp out of range");
		}
		auto bucket = Add(calendar, origin, Interval::Make(0, static_cast<int32_t>(result_days), 0));
		if (ts.micros < bucket.micros) {
			bucket = FormerOccurrence(calendar.GetCalendar(), bucket, ts);
		}
		while (ts.micros < bucket.micros) {
			bucket = Add(calendar, bucket, Interval::Make(0, -bucket_width_days, 0));
		}

		return bucket;
	}

	static inline timestamp_tz_t WidthConvertibleToMonthsCommon(int32_t bucket_width_months, const timestamp_tz_t ts,
	                                                            const timestamp_tz_t origin, TZCalendar &calendar_p) {
		if (!bucket_width_months) {
			throw OutOfRangeException("Can't bucket using zero months");
		}
		const auto trunc_months = TruncationFactory(DatePartSpecifier::MONTH);
		const auto sub_months = SubtractFactory(DatePartSpecifier::MONTH);

		auto calendar = calendar_p.GetCalendar();
		uint64_t tmp_micros = SetTime(calendar, ts);
		trunc_months(calendar, tmp_micros);
		auto truncated_ts = GetTimeUnsafe(calendar, tmp_micros);

		tmp_micros = SetTime(calendar, origin);
		trunc_months(calendar, tmp_micros);
		auto truncated_origin = GetTimeUnsafe(calendar, tmp_micros);

		auto ts_months = int32_t(sub_months(calendar, truncated_origin, truncated_ts));
		auto result_months = (ts_months / bucket_width_months) * bucket_width_months;
		if (ts_months < 0 && ts_months % bucket_width_months != 0) {
			result_months = SubtractOverflowCheck(result_months, bucket_width_months);
		}

		auto bucket = Add(calendar_p, truncated_origin, Interval::Make(result_months, 0, 0));
		if (ts.micros < bucket.micros) {
			bucket = FormerOccurrence(calendar, bucket, ts);
		}
		while (ts.micros < bucket.micros) {
			bucket = Add(calendar_p, bucket, Interval::Make(-bucket_width_months, 0, 0));
		}
		return bucket;
	}

	//! The bucket of `ts` for a width of a type, from `origin`
	static timestamp_tz_t Bucket(BucketWidthType type, interval_t bucket_width, timestamp_tz_t ts,
	                             timestamp_tz_t origin, TZCalendar &calendar) {
		switch (type) {
		case BucketWidthType::CONVERTIBLE_TO_MICROS:
			return WidthConvertibleToMicrosCommon(bucket_width.micros, ts, origin, calendar);
		case BucketWidthType::CONVERTIBLE_TO_DAYS:
			return WidthConvertibleToDaysCommon(bucket_width.days, ts, origin, calendar);
		case BucketWidthType::CONVERTIBLE_TO_MONTHS:
			return WidthConvertibleToMonthsCommon(bucket_width.months, ts, origin, calendar);
		default:
			throw NotImplementedException("Bucket type not implemented for ICU TIME_BUCKET");
		}
	}

	//! The default origin of a width: a Monday for widths of days or less, the start of a year for months
	static int64_t DefaultOrigin(BucketWidthType type) {
		return type == BucketWidthType::CONVERTIBLE_TO_MONTHS ? DEFAULT_ORIGIN_MICROS_2 : DEFAULT_ORIGIN_MICROS_1;
	}

	//! The type of a width that is constant for all rows, when it is one the rows need not classify themselves. A row
	//! classifies its width with the errors for widths of no type.
	struct WidthTypes {
		WidthTypes(cxx::ScalarFunction::ExecInput &input, bool other_constant) {
			auto width_arg = input.GetArg(0);
			if (!other_constant || !IsConstant(width_arg)) {
				return;
			}
			ArgReader<interval_t> width(width_arg);
			if (width.IsValid(0)) {
				const auto type = ClassifyBucketWidth(width.Get(0));
				if (type != BucketWidthType::UNCLASSIFIED) {
					constant_type = type;
				}
			}
		}

		BucketWidthType Get(interval_t bucket_width) const {
			return constant_type ? *constant_type : ClassifyBucketWidthErrorThrow(bucket_width);
		}

		std::optional<BucketWidthType> constant_type;
	};

	//! The calendar of the settings, in UTC, as the functions without a time zone argument bucket in
	static TZCalendar UTCCalendar(cxx::ScalarFunction::ExecInput &input) {
		auto &info = input.GetBindData<BindData>();
		auto &cache = input.GetInitData<CalendarCacheState>();
		TZCalendar calendar(*info.calendar, info.cal_setting);
		cache.SetTimeZone(calendar.GetCalendar(), "UTC");
		return calendar;
	}

	template <typename TB>
	static void ICUTimeBucketFunction(cxx::ScalarFunction::ExecInput &input) {
		auto calendar = UTCCalendar(input);
		WidthTypes types(input, true);
		ExecuteRows<timestamp_tz_t, interval_t, TB>(
		    input, [&](interval_t bucket_width, TB tb) -> std::optional<timestamp_tz_t> {
			    const auto ts = TruncCast<TB, timestamp_tz_t>(tb);
			    const auto type = types.Get(bucket_width);
			    if (!IsFinite(ts)) {
				    return ts;
			    }
			    return Bucket(type, bucket_width, ts, timestamp_tz_t {DefaultOrigin(type)}, calendar);
		    });
	}

	template <typename TB>
	static void ICUTimeBucketOffsetFunction(cxx::ScalarFunction::ExecInput &input) {
		auto calendar = UTCCalendar(input);
		WidthTypes types(input, true);
		ExecuteRows<timestamp_tz_t, interval_t, TB, interval_t>(
		    input, [&](interval_t bucket_width, TB tb, interval_t offset) -> std::optional<timestamp_tz_t> {
			    const auto ts = TruncCast<TB, timestamp_tz_t>(tb);
			    const auto type = types.Get(bucket_width);
			    if (!IsFinite(ts)) {
				    return ts;
			    }
			    const timestamp_tz_t origin {DefaultOrigin(type)};
			    return Add(calendar, Bucket(type, bucket_width, Sub(calendar, ts, offset), origin, calendar), offset);
		    });
	}

	template <typename TB>
	static void ICUTimeBucketOriginFunction(cxx::ScalarFunction::ExecInput &input) {
		auto calendar = UTCCalendar(input);
		WidthTypes types(input, IsConstant(input.GetArg(2)));
		ExecuteRows<timestamp_tz_t, interval_t, TB, timestamp_tz_t>(
		    input, [&](interval_t bucket_width, TB tb, timestamp_tz_t origin) -> std::optional<timestamp_tz_t> {
			    if (!IsFinite(origin)) {
				    return std::nullopt;
			    }
			    const auto type = types.Get(bucket_width);
			    const auto ts = TruncCast<TB, timestamp_tz_t>(tb);
			    if (!IsFinite(ts)) {
				    return ts;
			    }
			    return Bucket(type, bucket_width, ts, origin, calendar);
		    });
	}

	template <typename TB>
	static void ICUTimeBucketTimeZoneFunction(cxx::ScalarFunction::ExecInput &input) {
		auto &info = input.GetBindData<BindData>();
		auto &cache = input.GetInitData<CalendarCacheState>();
		TZCalendar calendar(*info.calendar, info.cal_setting);
		WidthTypes types(input, IsConstant(input.GetArg(2)));
		ExecuteRows<timestamp_tz_t, interval_t, TB, std::string_view>(
		    input, [&](interval_t bucket_width, TB tb, std::string_view tz) -> std::optional<timestamp_tz_t> {
			    const auto ts = TruncCast<TB, timestamp_tz_t>(tb);
			    cache.SetTimeZone(calendar.GetCalendar(), tz);
			    const auto type = types.Get(bucket_width);
			    const auto origin = FromNaive(calendar.GetCalendar(), timestamp_t {DefaultOrigin(type)});
			    if (!IsFinite(ts)) {
				    return ts;
			    }
			    return Bucket(type, bucket_width, ts, origin, calendar);
		    });
	}

	template <typename TB>
	static void AddFunctions(const ICURegistration &reg, const string &type) {
		auto add = [&](vector<pair<string, string>> parameters, cxx::ScalarFunction::ExecCallback exec) {
			auto function = reg.Scalar("time_bucket", parameters, "TIMESTAMPTZ");
			function.SetBindCallback(Bind).SetInitCallback(InitCalendarCache).SetExecCallback(exec);
			function.SetStability(cxx::FunctionStability::CONSISTENT_WITHIN_QUERY);
			function.SetArgumentMonotonicity(1, cxx::FunctionMonotonicity::NON_DECREASING);
			reg.extension.Register(function);
		};
		add({{"bucket_width", "INTERVAL"}, {"timestamp", type}}, ICUTimeBucketFunction<TB>);
		add({{"bucket_width", "INTERVAL"}, {"timestamp", type}, {"offset", "INTERVAL"}},
		    ICUTimeBucketOffsetFunction<TB>);
		add({{"bucket_width", "INTERVAL"}, {"timestamp", type}, {"origin", "TIMESTAMPTZ"}},
		    ICUTimeBucketOriginFunction<TB>);
		add({{"bucket_width", "INTERVAL"}, {"timestamp", type}, {"timezone", "VARCHAR"}},
		    ICUTimeBucketTimeZoneFunction<TB>);
	}
};

void RegisterICUTimeBucketFunctions(const ICURegistration &reg) {
	ICUTimeBucket::AddFunctions<timestamp_tz_t>(reg, "TIMESTAMPTZ");
	ICUTimeBucket::AddFunctions<timestamp_tz_ns_t>(reg, "TIMESTAMPTZ_NS");
}

} // namespace duckdb_icu
