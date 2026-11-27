#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/scalar/strftime_format.hpp"

namespace duckdb::capiv2 {

class CV2StrpTimeFormat {
public:
	StrpTimeFormat format;
};

class CV2StrpTimeResult {
public:
	StrpTimeFormat::ParseResult result;
};

class CV2StrfTimeFormat {
public:
	StrfTimeFormat format;
};

static auto Convert(duckdb_v2_strptime_format_handle handle) -> CV2StrpTimeFormat * {
	return reinterpret_cast<CV2StrpTimeFormat *>(handle);
}
static auto Convert(CV2StrpTimeFormat *format) -> duckdb_v2_strptime_format_handle {
	return reinterpret_cast<duckdb_v2_strptime_format_handle>(format);
}
static auto Convert(duckdb_v2_strptime_result_handle handle) -> CV2StrpTimeResult * {
	return reinterpret_cast<CV2StrpTimeResult *>(handle);
}
static auto Convert(CV2StrpTimeResult *result) -> duckdb_v2_strptime_result_handle {
	return reinterpret_cast<duckdb_v2_strptime_result_handle>(result);
}
static auto Convert(duckdb_v2_strftime_format_handle handle) -> CV2StrfTimeFormat * {
	return reinterpret_cast<CV2StrfTimeFormat *>(handle);
}
static auto Convert(CV2StrfTimeFormat *format) -> duckdb_v2_strftime_format_handle {
	return reinterpret_cast<duckdb_v2_strftime_format_handle>(format);
}

//! Parses a format string into `format`, with the error message of strptime and strftime
static void ParseFormatString(const char *text, idx_t length, StrTimeFormat &format) {
	format.format_specifier = string(text, length);
	auto error = StrTimeFormat::ParseFormatSpecifier(format.format_specifier, format);
	if (!error.empty()) {
		throw InvalidInputException("Failed to parse format specifier %s: %s", format.format_specifier, error);
	}
}

//! Copies `text` into the caller's buffer as far as it fits, and reports its whole length
static void WriteToBuffer(const string &text, char *buffer, idx_t buffer_size, idx_t &length) {
	length = text.size();
	if (buffer && buffer_size > 0) {
		memcpy(buffer, text.data(), MinValue<idx_t>(buffer_size, text.size()));
	}
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

//----------------------------------------------------------------------------------------------------------------------
// strptime
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_strptime_format_create(const char *format, idx_t format_length,
                                                 duckdb_v2_strptime_format_handle *out_format,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_STR_ARG(format, format_length);
	DUCKDB_CHECK_ARG(out_format);
	*out_format = nullptr;
	return WithErrorHandler(err, [&]() {
		auto result = duckdb::make_uniq<CV2StrpTimeFormat>();
		ParseFormatString(format, format_length, result->format);
		*out_format = Convert(result.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_strptime_format_get_flags(duckdb_v2_strptime_format_handle format, uint32_t *flags,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(format);
	DUCKDB_CHECK_ARG(flags);
	return WithErrorHandler(err, [&]() {
		auto &strptime_format = Convert(format)->format;
		uint32_t result = 0;
		if (strptime_format.HasFormatSpecifier(duckdb::StrTimeSpecifier::UTC_OFFSET)) {
			result |= DUCKDB_V2_TIME_FORMAT_FLAG_UTC_OFFSET;
		}
		if (strptime_format.HasFormatSpecifier(duckdb::StrTimeSpecifier::TZ_NAME)) {
			result |= DUCKDB_V2_TIME_FORMAT_FLAG_TZ_NAME;
		}
		if (strptime_format.HasFormatSpecifier(duckdb::StrTimeSpecifier::NANOSECOND_PADDED)) {
			result |= DUCKDB_V2_TIME_FORMAT_FLAG_NANOSECONDS;
		}
		*flags = result;
	});
}

DUCKDB_V2_ERROR duckdb_v2_strptime_format_parse(duckdb_v2_strptime_format_handle format, const char *text,
                                                idx_t text_length, duckdb_v2_strptime_result_handle result,
                                                bool *matched, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(format);
	DUCKDB_CHECK_STR_ARG(text, text_length);
	DUCKDB_CHECK_ARG(result);
	DUCKDB_CHECK_ARG(matched);
	return WithErrorHandler(err, [&]() {
		auto &parse_result = Convert(result)->result;
		parse_result.tz.clear();
		*matched = Convert(format)->format.Parse(text ? text : "", text_length, parse_result);
	});
}

DUCKDB_V2_ERROR duckdb_v2_strptime_format_destroy(duckdb_v2_strptime_format_handle *format) {
	return WithErrorHandler(nullptr, [&]() {
		if (!format) {
			return;
		}
		if (*format) {
			delete Convert(*format);
			*format = nullptr;
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_strptime_result_create(duckdb_v2_strptime_result_handle *out_result,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out_result);
	*out_result = nullptr;
	return WithErrorHandler(err, [&]() { *out_result = Convert(new CV2StrpTimeResult()); });
}

DUCKDB_V2_ERROR duckdb_v2_strptime_result_get_parts(duckdb_v2_strptime_result_handle result,
                                                    duckdb_v2_strptime_parts *parts, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(result);
	DUCKDB_CHECK_ARG(parts);
	return WithErrorHandler(err, [&]() {
		auto &parse_result = Convert(result)->result;
		auto &data = parse_result.data;
		parts->parts.year = data[0];
		parts->parts.month = data[1];
		parts->parts.day = data[2];
		parts->parts.hour = data[3];
		parts->parts.minute = data[4];
		parts->parts.second = data[5];
		parts->parts.fraction = data[6];
		parts->parts.utc_offset = data[7];
		parts->is_special = parse_result.is_special;
		parts->special_days = parse_result.is_special ? parse_result.special.days : 0;
		parts->tz_name = parse_result.tz.data();
		parts->tz_name_length = parse_result.tz.size();
	});
}

DUCKDB_V2_ERROR duckdb_v2_strptime_result_get_error(duckdb_v2_strptime_result_handle result, const char *text,
                                                    idx_t text_length, const char *format_string,
                                                    idx_t format_string_length, char *buffer, idx_t buffer_size,
                                                    idx_t *length, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(result);
	DUCKDB_CHECK_STR_ARG(text, text_length);
	DUCKDB_CHECK_STR_ARG(format_string, format_string_length);
	DUCKDB_CHECK_ARG(length);
	return WithErrorHandler(err, [&]() {
		auto message = Convert(result)->result.FormatError(duckdb::string_t(text ? text : "", text_length),
		                                                   duckdb::string(format_string, format_string_length));
		WriteToBuffer(message, buffer, buffer_size, *length);
	});
}

DUCKDB_V2_ERROR duckdb_v2_strptime_result_destroy(duckdb_v2_strptime_result_handle *result) {
	return WithErrorHandler(nullptr, [&]() {
		if (!result) {
			return;
		}
		if (*result) {
			delete Convert(*result);
			*result = nullptr;
		}
	});
}

//----------------------------------------------------------------------------------------------------------------------
// strftime
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_strftime_format_create(const char *format, idx_t format_length,
                                                 duckdb_v2_strftime_format_handle *out_format,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_STR_ARG(format, format_length);
	DUCKDB_CHECK_ARG(out_format);
	*out_format = nullptr;
	return WithErrorHandler(err, [&]() {
		auto result = duckdb::make_uniq<CV2StrfTimeFormat>();
		ParseFormatString(format, format_length, result->format);
		*out_format = Convert(result.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_strftime_format_write(duckdb_v2_strftime_format_handle format,
                                                const duckdb_v2_time_parts *parts, bool nanoseconds,
                                                const char *tz_name, idx_t tz_name_length, char *buffer,
                                                idx_t buffer_size, idx_t *length, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(format);
	DUCKDB_CHECK_ARG(parts);
	DUCKDB_CHECK_STR_ARG(tz_name, tz_name_length);
	DUCKDB_CHECK_ARG(length);
	return WithErrorHandler(err, [&]() {
		auto &strftime_format = Convert(format)->format;
		int32_t data[8] = {parts->year,   parts->month,  parts->day,      parts->hour,
		                   parts->minute, parts->second, parts->fraction, parts->utc_offset};
		if (!nanoseconds) {
			data[6] *= duckdb::Interval::NANOS_PER_MICRO;
		}
		duckdb::date_t date;
		if (!duckdb::Date::TryFromDate(data[0], data[1], data[2], date)) {
			throw duckdb::InvalidInputException("Invalid date %d-%d-%d", data[0], data[1], data[2]);
		}
		// the engine reads the time zone name as a null-terminated string
		duckdb::string zone(tz_name ? tz_name : "", tz_name_length);
		auto size = strftime_format.GetLength(date, data, zone.c_str());
		*length = size;
		if (size <= buffer_size && buffer) {
			strftime_format.FormatStringNS(date, data, zone.c_str(), buffer);
		} else if (buffer && buffer_size > 0) {
			duckdb::string text(size, '\0');
			strftime_format.FormatStringNS(date, data, zone.c_str(), &text[0]);
			memcpy(buffer, text.data(), buffer_size);
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_strftime_format_destroy(duckdb_v2_strftime_format_handle *format) {
	return WithErrorHandler(nullptr, [&]() {
		if (!format) {
			return;
		}
		if (*format) {
			delete Convert(*format);
			*format = nullptr;
		}
	});
}

//----------------------------------------------------------------------------------------------------------------------
// Text of the time zone casts
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_timestamp_tz_parse_text(const char *text, idx_t text_length, bool nanoseconds,
                                                  duckdb_v2_timestamp_text *parsed,
                                                  DUCKDB_V2_TIMESTAMP_TEXT_RESULT *result,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_STR_ARG(text, text_length);
	DUCKDB_CHECK_ARG(parsed);
	DUCKDB_CHECK_ARG(result);
	return WithErrorHandler(err, [&]() {
		duckdb::timestamp_t value;
		bool has_offset = false;
		duckdb::string_t tz(nullptr, 0);
		int32_t nanos = 0;
		auto cast_result = duckdb::Timestamp::TryConvertTimestampTZ(text ? text : "", text_length, value, true,
		                                                            has_offset, tz, nanoseconds ? &nanos : nullptr);
		switch (cast_result) {
		case duckdb::TimestampCastResult::SUCCESS:
			*result = DUCKDB_V2_TIMESTAMP_TEXT_SUCCESS;
			parsed->micros = value.value;
			parsed->nanos = nanos;
			parsed->has_offset = has_offset;
			parsed->tz_name = nullptr;
			parsed->tz_name_length = 0;
			if (tz.GetSize()) {
				// a short name is inlined in the string_t: point into the text, where the name ends the timestamp
				std::string_view input(text, text_length);
				auto pos = input.rfind(std::string_view(tz.GetData(), tz.GetSize()));
				D_ASSERT(pos != std::string_view::npos);
				parsed->tz_name = text + pos;
				parsed->tz_name_length = tz.GetSize();
			}
			return;
		case duckdb::TimestampCastResult::ERROR_RANGE:
			*result = DUCKDB_V2_TIMESTAMP_TEXT_ERROR_RANGE;
			return;
		default:
			*result = DUCKDB_V2_TIMESTAMP_TEXT_ERROR_FORMAT;
			return;
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_time_tz_parse_text(const char *text, idx_t text_length, int64_t *micros, int32_t *offset,
                                             bool *has_offset, bool *success, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_STR_ARG(text, text_length);
	DUCKDB_CHECK_ARG(micros);
	DUCKDB_CHECK_ARG(offset);
	DUCKDB_CHECK_ARG(has_offset);
	DUCKDB_CHECK_ARG(success);
	return WithErrorHandler(err, [&]() {
		duckdb::dtime_tz_t value;
		bool text_has_offset = false;
		idx_t pos = 0;
		*success = duckdb::Time::TryConvertTimeTZ(text ? text : "", text_length, pos, value, text_has_offset, false);
		if (!*success) {
			return;
		}
		*micros = value.time().value;
		*offset = value.offset();
		*has_offset = text_has_offset;
	});
}
