#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/execution/operator/csv_scanner/encode/csv_encoder.hpp"
#include "duckdb/function/encoding_function.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb::capiv2 {

class CV2EncodingInfo final : public EncodingFunctionInfo {
public:
	CV2EncodingInfo(duckdb_v2_encoding_function_decode_callback_fn decode_cb, shared_ptr<CV2UserData> user_data)
	    : decode_cb(decode_cb), user_data(std::move(user_data)) {
	}

public:
	duckdb_v2_encoding_function_decode_callback_fn decode_cb;
	shared_ptr<CV2UserData> user_data;
};

//! Calls the callback and checks what it reports.
static void CV2EncodingCall(const CV2EncodingInfo &info, const char *input, idx_t input_length, bool is_last,
                            char *output, idx_t output_capacity, idx_t &consumed, idx_t &written) {
	consumed = 0;
	written = 0;
	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	info.decode_cb(info.user_data ? info.user_data->GetData() : nullptr, reinterpret_cast<const uint8_t *>(input),
	               input_length, is_last, reinterpret_cast<uint8_t *>(output), output_capacity, &consumed, &written,
	               &err_ptr);
	if (err.HasError()) {
		err.ThrowAsException();
	}
	if (consumed > input_length || written > output_capacity) {
		throw InvalidInputException("The decode callback of an encoding reported more bytes than it was given");
	}
}

static void CV2EncodingDecode(CSVEncoderBuffer &encoded_buffer, char *target_buffer,
                              idx_t &target_buffer_current_position, const idx_t target_buffer_size,
                              char *remaining_bytes_buffer, idx_t &remaining_bytes_size,
                              EncodingFunction *encoding_function) {
	const auto &info = encoding_function->function_info->Cast<CV2EncodingInfo>();
	const auto is_last = encoded_buffer.last_buffer;
	const auto encoded_size = encoded_buffer.actual_encoded_buffer_size;
	idx_t consumed;
	idx_t written;

	idx_t available = encoded_size - encoded_buffer.cur_pos;
	idx_t free_space = target_buffer_size - target_buffer_current_position;
	if (available == 0 || free_space == 0) {
		return;
	}
	CV2EncodingCall(info, encoded_buffer.Ptr() + encoded_buffer.cur_pos, available, is_last,
	                target_buffer + target_buffer_current_position, free_space, consumed, written);
	encoded_buffer.cur_pos += consumed;
	target_buffer_current_position += written;

	available = encoded_size - encoded_buffer.cur_pos;
	free_space = target_buffer_size - target_buffer_current_position;
	const auto max_output_bytes = encoding_function->GetBytesPerIteration();
	const auto has_lookup = is_last || available >= encoding_function->GetLookupBytes();
	if (available > 0 && consumed == 0 && has_lookup && free_space >= max_output_bytes) {
		throw InvalidInputException("The decode callback of encoding \"%s\" did not consume any of its input",
		                            encoding_function->GetName());
	}
	if (available == 0 || free_space == 0 || free_space >= max_output_bytes) {
		return;
	}
	// The next character did not fit: decode it on the side, and carry the part that does not fit to the next chunk
	vector<char> scratch(max_output_bytes);
	CV2EncodingCall(info, encoded_buffer.Ptr() + encoded_buffer.cur_pos, available, is_last, scratch.data(),
	                scratch.size(), consumed, written);
	if (consumed == 0) {
		if (is_last) {
			throw InvalidInputException("The decode callback of encoding \"%s\" did not consume the end of the input",
			                            encoding_function->GetName());
		}
		return;
	}
	encoded_buffer.cur_pos += consumed;
	const auto fits = MinValue<idx_t>(written, free_space);
	memcpy(target_buffer + target_buffer_current_position, scratch.data(), fits);
	target_buffer_current_position += fits;
	remaining_bytes_size = written - fits;
	memcpy(remaining_bytes_buffer, scratch.data() + fits, remaining_bytes_size);
}

class CV2EncodingFunction {
public:
	explicit CV2EncodingFunction(DatabaseInstance &db) : db(db) {
	}

	//! Validates the configuration and builds the encoding to register on `target`.
	EncodingFunction Build(DatabaseInstance &target) const {
		CheckRegistrationTarget(db, target, "encoding function");
		if (name.empty()) {
			throw InvalidInputException("Name must be set for the function.");
		}
		if (max_input_bytes == 0) {
			throw InvalidInputException("Maximum input bytes must be set to at least 1 for the function.");
		}
		if (max_output_bytes == 0) {
			throw InvalidInputException("Maximum output bytes must be set to at least 1 for the function.");
		}
		if (!decode_cb) {
			throw InvalidInputException("Decode callback must be set for the function.");
		}
		EncodingFunction result(name, CV2EncodingDecode, max_output_bytes, max_input_bytes);
		result.function_info = make_shared_ptr<CV2EncodingInfo>(decode_cb, user_data);
		return result;
	}

public:
	//! The database it was created for: the only one it can be registered on.
	DatabaseInstance &db;
	string name;
	idx_t max_input_bytes = 0;
	idx_t max_output_bytes = 0;
	duckdb_v2_encoding_function_decode_callback_fn decode_cb = nullptr;
	shared_ptr<CV2UserData> user_data = nullptr;
};

static auto Convert(duckdb_v2_encoding_function_handle func) -> CV2EncodingFunction * {
	return reinterpret_cast<CV2EncodingFunction *>(func);
}
static auto Convert(CV2EncodingFunction *func) -> duckdb_v2_encoding_function_handle {
	return reinterpret_cast<duckdb_v2_encoding_function_handle>(func);
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_encoding_function_create(duckdb_v2_factory_handle factory,
                                                   duckdb_v2_encoding_function_handle *function,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(function);
	*function = nullptr;
	return WithErrorHandler(err, [&]() {
		auto result = duckdb::make_uniq<CV2EncodingFunction>(Convert(factory)->GetDatabase());
		*function = Convert(result.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_encoding_function_set_name(duckdb_v2_encoding_function_handle function, const char *name,
                                                     idx_t name_length, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_STR_ARG(name, name_length);
	return WithErrorHandler(err,
	                        [&]() { Convert(function)->name = std::string(ConvertIdentifierName(name, name_length)); });
}

DUCKDB_V2_ERROR duckdb_v2_encoding_function_set_max_input_bytes(duckdb_v2_encoding_function_handle function,
                                                                idx_t max_input_bytes,
                                                                duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->max_input_bytes = max_input_bytes; });
}

DUCKDB_V2_ERROR duckdb_v2_encoding_function_set_max_output_bytes(duckdb_v2_encoding_function_handle function,
                                                                 idx_t max_output_bytes,
                                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->max_output_bytes = max_output_bytes; });
}

DUCKDB_V2_ERROR duckdb_v2_encoding_function_set_user_data(duckdb_v2_encoding_function_handle function,
                                                          duckdb_v2_opaque *user_data,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(user_data);
	return WithErrorHandler(err, [&]() {
		Convert(function)->user_data =
		    duckdb::make_shared_ptr<CV2UserData>(user_data->ptr, user_data->destroy, user_data->equals);
	});
}

DUCKDB_V2_ERROR duckdb_v2_encoding_function_set_decode_callback(duckdb_v2_encoding_function_handle function,
                                                                duckdb_v2_encoding_function_decode_callback_fn callback,
                                                                duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->decode_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_connection_register_encoding_function(duckdb_v2_connection_handle conn,
                                                                duckdb_v2_encoding_function_handle function,
                                                                duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		auto &db = *Convert(conn)->context->db;
		duckdb::DBConfig::GetConfig(db).RegisterEncodeFunction(Convert(function)->Build(db));
	});
}

DUCKDB_V2_ERROR duckdb_v2_extension_register_encoding_function(duckdb_v2_extension_handle extension,
                                                               duckdb_v2_encoding_function_handle function,
                                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(extension);
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		auto &db = GetExtensionLoader(extension).GetDatabaseInstance();
		duckdb::DBConfig::GetConfig(db).RegisterEncodeFunction(Convert(function)->Build(db));
	});
}

DUCKDB_V2_ERROR duckdb_v2_encoding_function_destroy(duckdb_v2_encoding_function_handle *function) {
	return WithErrorHandler(nullptr, [&]() {
		if (!function) {
			return;
		}
		if (*function) {
			delete Convert(*function);
			*function = nullptr;
		}
	});
}
