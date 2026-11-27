#include "test_capi_v2.hpp"

#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// V2 datetime tests: strptime and strftime formats, the text of the TIMESTAMPTZ
// and TIMETZ casts, and the transaction start time and interrupt check of a
// context.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

std::string StrptimeError(duckdb_v2_strptime_result_handle result, std::string_view text, std::string_view format) {
	idx_t length = 0;
	REQUIRE(duckdb_v2_strptime_result_get_error(result, text.data(), text.size(), format.data(), format.size(), nullptr,
	                                            0, &length, nullptr) == DUCKDB_V2_ERROR_NONE);
	std::string message(length, '\0');
	REQUIRE(duckdb_v2_strptime_result_get_error(result, text.data(), text.size(), format.data(), format.size(),
	                                            &message[0], length, &length, nullptr) == DUCKDB_V2_ERROR_NONE);
	return message;
}

// Writes the transaction start time, and whether the interrupt check passed
void TransactionTimeExec(duckdb_v2_scalar_function_exec_info_handle info, duckdb_v2_context_handle context,
                         duckdb_v2_error_info_handle *err) {
	duckdb_v2_vector_handle out = nullptr;
	idx_t count = 0;
	void *raw = nullptr;
	int64_t micros = 0;
	if (duckdb_v2_scalar_function_exec_get_result(info, &out, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_scalar_function_exec_get_row_count(info, &count, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_data_mutable(out, &raw, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_context_interrupt_check(context, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_context_get_transaction_start_time(context, &micros, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	for (idx_t i = 0; i < count; i++) {
		static_cast<int64_t *>(raw)[i] = micros;
	}
}

int64_t QueryTransactionTime(duckdb_v2_connection_handle conn, std::string_view sql) {
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(conn, sql, &result) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(result);
	REQUIRE(chunk != nullptr);
	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
	duckdb_v2_vector_view view {};
	duckdb_v2_vector_get_view(vec, &view, nullptr);
	auto out = static_cast<const int64_t *>(view.data)[SelAt(view.sel, 0)];
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&result);
	return out;
}

// Runs a statement to completion: execution is lazy, so a result that is not stepped may not have run
void RunStatement(duckdb_v2_connection_handle conn, std::string_view sql) {
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(conn, sql, &result) == DUCKDB_V2_ERROR_NONE);
	auto status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	while (true) {
		duckdb_v2_data_chunk_handle chunk = nullptr;
		REQUIRE(duckdb_v2_result_step(result, &chunk, &status, nullptr) == DUCKDB_V2_ERROR_NONE);
		if (chunk) {
			duckdb_v2_data_chunk_destroy(&chunk);
		}
		if (status == DUCKDB_V2_RESULT_STEP_STATUS_FINISHED) {
			break;
		}
		if (status == DUCKDB_V2_RESULT_STEP_STATUS_WAITING) {
			REQUIRE(duckdb_v2_result_wait(result, nullptr) == DUCKDB_V2_ERROR_NONE);
		}
	}
	duckdb_v2_result_destroy(&result);
}

} // namespace

TEST_CASE("V2 datetime: strptime formats", "[capi_v2][datetime]") {
	duckdb_v2_strptime_format_handle format = nullptr;
	const std::string spec = "%Y-%m-%d %H:%M:%S %Z";
	REQUIRE(duckdb_v2_strptime_format_create(spec.data(), spec.size(), &format, nullptr) == DUCKDB_V2_ERROR_NONE);
	uint32_t flags = 0;
	REQUIRE(duckdb_v2_strptime_format_get_flags(format, &flags, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(flags == DUCKDB_V2_TIME_FORMAT_FLAG_TZ_NAME);

	duckdb_v2_strptime_result_handle result = nullptr;
	REQUIRE(duckdb_v2_strptime_result_create(&result, nullptr) == DUCKDB_V2_ERROR_NONE);

	const std::string text = "2026-11-27 10:30:15 America/New_York";
	bool matched = false;
	REQUIRE(duckdb_v2_strptime_format_parse(format, text.data(), text.size(), result, &matched, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(matched);
	duckdb_v2_strptime_parts parts {};
	REQUIRE(duckdb_v2_strptime_result_get_parts(result, &parts, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(!parts.is_special);
	REQUIRE(parts.parts.year == 2026);
	REQUIRE(parts.parts.month == 11);
	REQUIRE(parts.parts.day == 27);
	REQUIRE(parts.parts.hour == 10);
	REQUIRE(parts.parts.minute == 30);
	REQUIRE(parts.parts.second == 15);
	REQUIRE(std::string(parts.tz_name, parts.tz_name_length) == "America/New_York");

	// a text that does not match is not a failure of the call
	const std::string bad = "2026-13-27";
	REQUIRE(duckdb_v2_strptime_format_parse(format, bad.data(), bad.size(), result, &matched, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(!matched);
	auto message = StrptimeError(result, bad, spec);
	REQUIRE(message.find("Could not parse string \"2026-13-27\" according to format specifier \"" + spec + "\"") == 0);

	REQUIRE(duckdb_v2_strptime_result_destroy(&result) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(result == nullptr);
	REQUIRE(duckdb_v2_strptime_format_destroy(&format) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(format == nullptr);
	REQUIRE(duckdb_v2_strptime_format_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);

	// an invalid format fails with the message of strptime
	duckdb_v2_error_info_handle err = nullptr;
	REQUIRE(duckdb_v2_strptime_format_create("%Q", 2, &format, &err) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(format == nullptr);
	const char *err_text = nullptr;
	idx_t err_length = 0;
	duckdb_v2_error_info_get_text(err, &err_text, &err_length);
	REQUIRE(std::string(err_text, err_length).find("Failed to parse format specifier %Q") != std::string::npos);
	duckdb_v2_error_info_destroy(&err);
}

TEST_CASE("V2 datetime: strftime formats", "[capi_v2][datetime]") {
	duckdb_v2_strftime_format_handle format = nullptr;
	const std::string spec = "%Y-%m-%d %H:%M:%S %Z";
	REQUIRE(duckdb_v2_strftime_format_create(spec.data(), spec.size(), &format, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_time_parts parts {2026, 11, 27, 10, 30, 15, 0, 3600};
	const std::string zone = "Europe/Amsterdam";
	idx_t length = 0;
	// a buffer too small reports the length it needs, and writes what fits
	char small[4];
	REQUIRE(duckdb_v2_strftime_format_write(format, &parts, false, zone.data(), zone.size(), small, sizeof(small),
	                                        &length, nullptr) == DUCKDB_V2_ERROR_NONE);
	const std::string expected = "2026-11-27 10:30:15 Europe/Amsterdam";
	REQUIRE(length == expected.size());
	REQUIRE(std::string(small, 4) == "2026");
	std::string text(length, '\0');
	REQUIRE(duckdb_v2_strftime_format_write(format, &parts, false, zone.data(), zone.size(), &text[0], text.size(),
	                                        &length, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(text == expected);

	// an invalid date is refused
	duckdb_v2_time_parts invalid {2026, 2, 30, 0, 0, 0, 0, 0};
	REQUIRE(duckdb_v2_strftime_format_write(format, &invalid, false, nullptr, 0, &text[0], text.size(), &length,
	                                        nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_strftime_format_destroy(&format) == DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2 datetime: the text of the time zone casts", "[capi_v2][datetime]") {
	duckdb_v2_timestamp_text parsed {};
	auto result = DUCKDB_V2_TIMESTAMP_TEXT_ERROR_FORMAT;
	const std::string zoned = "2000-01-01 00:00:00 Asia/Tokyo";
	REQUIRE(duckdb_v2_timestamp_tz_parse_text(zoned.data(), zoned.size(), false, &parsed, &result, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(result == DUCKDB_V2_TIMESTAMP_TEXT_SUCCESS);
	REQUIRE(!parsed.has_offset);
	REQUIRE(parsed.micros == 946684800000000LL);
	REQUIRE(std::string(parsed.tz_name, parsed.tz_name_length) == "Asia/Tokyo");

	const std::string offset = "2000-01-01 00:00:00+09";
	REQUIRE(duckdb_v2_timestamp_tz_parse_text(offset.data(), offset.size(), true, &parsed, &result, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(result == DUCKDB_V2_TIMESTAMP_TEXT_SUCCESS);
	REQUIRE(parsed.has_offset);
	REQUIRE(parsed.micros == 946684800000000LL - 9 * 3600 * 1000000LL);

	const std::string bad = "yesterday";
	REQUIRE(duckdb_v2_timestamp_tz_parse_text(bad.data(), bad.size(), false, &parsed, &result, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(result == DUCKDB_V2_TIMESTAMP_TEXT_ERROR_FORMAT);

	int64_t micros = 0;
	int32_t tz_offset = 0;
	bool has_offset = false;
	bool success = false;
	const std::string time = "12:00:00-02:30";
	REQUIRE(duckdb_v2_time_tz_parse_text(time.data(), time.size(), &micros, &tz_offset, &has_offset, &success,
	                                     nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(success);
	REQUIRE(has_offset);
	REQUIRE(micros == 12 * 3600 * 1000000LL);
	REQUIRE(tz_offset == -(2 * 3600 + 30 * 60));
	REQUIRE(duckdb_v2_time_tz_parse_text(bad.data(), bad.size(), &micros, &tz_offset, &has_offset, &success, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(!success);
}

TEST_CASE("V2 datetime: the transaction start time of a context", "[capi_v2][datetime]") {
	EnvFixture fx;
	auto bigint = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	duckdb_v2_scalar_function_handle function = nullptr;
	REQUIRE(duckdb_v2_scalar_function_create(fx.factory, &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_name(function, "txn_start", 9, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_function_signature_handle sig = nullptr;
	REQUIRE(duckdb_v2_scalar_function_get_signature(function, &sig, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, bigint, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_exec_callback(function, TransactionTimeExec, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_property(function, DUCKDB_V2_FUNCTION_PROPERTY_STABILITY,
	                                               DUCKDB_V2_FUNCTION_PROPERTY_STABILITY_VOLATILE,
	                                               nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_scalar_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&bigint);

	RunStatement(fx.conn, "BEGIN");
	const auto first = QueryTransactionTime(fx.conn, "SELECT txn_start()");
	REQUIRE(first == QueryTransactionTime(fx.conn, "SELECT txn_start()"));
	REQUIRE(first == QueryTransactionTime(fx.conn, "SELECT epoch_us(get_current_timestamp())"));
	RunStatement(fx.conn, "COMMIT");
}

} // namespace test_capi_v2
