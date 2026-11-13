#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/function/built_in_functions.hpp"
#include "duckdb/function/encoding_function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"

namespace duckdb {

namespace {

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ && (defined(__GNUC__) || defined(__clang__))
#define ENCODING_FAST_PATH 1
#endif

void DecodeLatin1ToUTF8(const char *input, idx_t input_size, bool is_last, char *output, idx_t &consumed,
                        idx_t &written) {
	idx_t out = 0;
	idx_t i = 0;
	while (i < input_size) {
#ifdef ENCODING_FAST_PATH
		// Copy 8 ASCII bytes at a time
		while (i + 8 <= input_size) {
			uint64_t word;
			memcpy(&word, input + i, sizeof(word));
			const uint64_t high_bits = word & 0x8080808080808080ULL;
			// Copy the whole word, but only advance past the ASCII prefix
			memcpy(output + out, &word, sizeof(word));
			if (high_bits) {
				const idx_t ascii_prefix = static_cast<idx_t>(__builtin_ctzll(high_bits)) / 8;
				i += ascii_prefix;
				out += ascii_prefix;
				break;
			}
			i += 8;
			out += 8;
		}
		if (i >= input_size) {
			break;
		}
#endif
		const auto ch = static_cast<unsigned char>(input[i++]);
		if (ch > 0x7F && ch <= 0x9F) {
			throw InvalidInputException("File is not latin-1 encoded");
		}
		if (ch <= 0x7F) {
			output[out++] = static_cast<char>(ch);
		} else {
			output[out++] = static_cast<char>(0xc2 + (ch > 0xbf));
			output[out++] = static_cast<char>((ch & 0x3f) + 0x80);
		}
	}
	consumed = input_size;
	written = out;
}

uint16_t LoadUTF16(const char *input, idx_t pos) {
	return static_cast<uint16_t>(static_cast<unsigned char>(input[pos]) |
	                             (static_cast<unsigned char>(input[pos + 1]) << 8));
}

void EncodeUTF8ToLatin1(const char *input, idx_t input_size, char *output, idx_t &written) {
	idx_t out = 0;
	idx_t pos = 0;
	while (pos < input_size) {
		const auto lead = static_cast<unsigned char>(input[pos]);
		if (lead <= 0x7F) {
			output[out++] = static_cast<char>(lead);
			pos++;
			continue;
		}
		// a two byte sequence U+0080..U+07FF, valid UTF-8 is guaranteed by VARCHAR
		uint32_t code_point = 0;
		idx_t length = 1;
		if (lead >= 0xF0) {
			length = 4;
		} else if (lead >= 0xE0) {
			length = 3;
		} else {
			length = 2;
			code_point = ((lead & 0x1F) << 6) | (static_cast<unsigned char>(input[pos + 1]) & 0x3F);
		}
		if (length != 2 || code_point > 0xFF || code_point <= 0x9F) {
			throw ConversionException("Could not encode the character at byte %llu in latin-1", pos);
		}
		output[out++] = static_cast<char>(code_point);
		pos += length;
	}
	written = out;
}

void EncodeUTF8ToUTF16(const char *input, idx_t input_size, char *output, idx_t &written) {
	idx_t out = 0;
	idx_t pos = 0;
	auto store = [&](uint16_t unit) {
		output[out++] = static_cast<char>(unit & 0xFF);
		output[out++] = static_cast<char>(unit >> 8);
	};
	while (pos < input_size) {
		const auto lead = static_cast<unsigned char>(input[pos]);
		uint32_t code_point;
		idx_t length;
		if (lead <= 0x7F) {
			code_point = lead;
			length = 1;
		} else if (lead < 0xE0) {
			code_point = ((lead & 0x1F) << 6) | (static_cast<unsigned char>(input[pos + 1]) & 0x3F);
			length = 2;
		} else if (lead < 0xF0) {
			code_point = ((lead & 0x0F) << 12) | ((static_cast<unsigned char>(input[pos + 1]) & 0x3F) << 6) |
			             (static_cast<unsigned char>(input[pos + 2]) & 0x3F);
			length = 3;
		} else {
			code_point = ((lead & 0x07) << 18) | ((static_cast<unsigned char>(input[pos + 1]) & 0x3F) << 12) |
			             ((static_cast<unsigned char>(input[pos + 2]) & 0x3F) << 6) |
			             (static_cast<unsigned char>(input[pos + 3]) & 0x3F);
			length = 4;
		}
		if (code_point >= 0x10000) {
			code_point -= 0x10000;
			store(static_cast<uint16_t>(0xD800 + (code_point >> 10)));
			store(static_cast<uint16_t>(0xDC00 + (code_point & 0x3FF)));
		} else {
			store(static_cast<uint16_t>(code_point));
		}
		pos += length;
	}
	written = out;
}

void DecodeUTF16ToUTF8(const char *input, idx_t input_size, bool is_last, char *output, idx_t &consumed,
                       idx_t &written) {
	idx_t pos = 0;
	idx_t out = 0;
	while (pos < input_size) {
#ifdef ENCODING_FAST_PATH
		// Narrow 4 ASCII code units at a time
		while (pos + 8 <= input_size) {
			uint64_t word;
			memcpy(&word, input + pos, sizeof(word));
			const uint64_t high_bits = word & 0xFF80FF80FF80FF80ULL;
			// Narrow the whole word, but only advance past the ASCII prefix
			uint64_t narrow = word & 0x00FF00FF00FF00FFULL;
			narrow = (narrow | (narrow >> 8)) & 0x0000FFFF0000FFFFULL;
			narrow = (narrow | (narrow >> 16)) & 0x00000000FFFFFFFFULL;
			const auto narrowed = static_cast<uint32_t>(narrow);
			memcpy(output + out, &narrowed, sizeof(narrowed));
			if (high_bits) {
				const idx_t ascii_units = static_cast<idx_t>(__builtin_ctzll(high_bits)) / 16;
				pos += ascii_units * 2;
				out += ascii_units;
				break;
			}
			pos += 8;
			out += 4;
		}
		if (pos >= input_size) {
			break;
		}
#endif
		if (pos + 1 >= input_size) {
			if (is_last) {
				throw InvalidInputException("File is not utf-16 encoded");
			}
			break;
		}
		const uint16_t ch = LoadUTF16(input, pos);
		if (ch >= 0xD800 && ch <= 0xDFFF) {
			if (ch >= 0xDC00) {
				// low surrogate without a preceding high surrogate
				throw InvalidInputException("File is not utf-16 encoded");
			}
			if (pos + 3 >= input_size) {
				if (is_last) {
					throw InvalidInputException("File is not utf-16 encoded");
				}
				// the low surrogate follows in the next buffer
				break;
			}
			const uint16_t low = LoadUTF16(input, pos + 2);
			if (low < 0xDC00 || low > 0xDFFF) {
				throw InvalidInputException("File is not utf-16 encoded");
			}
			const uint32_t code_point = 0x10000 + ((static_cast<uint32_t>(ch - 0xD800)) << 10) + (low - 0xDC00);
			output[out++] = static_cast<char>(0xF0 | (code_point >> 18));
			output[out++] = static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
			output[out++] = static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
			output[out++] = static_cast<char>(0x80 | (code_point & 0x3F));
			pos += 4;
			continue;
		}
		if (ch <= 0x007F) {
			output[out++] = static_cast<char>(ch);
		} else if (ch <= 0x07FF) {
			output[out++] = static_cast<char>(0xC0 | (ch >> 6));
			output[out++] = static_cast<char>(0x80 | (ch & 0x3F));
		} else {
			output[out++] = static_cast<char>(0xE0 | (ch >> 12));
			output[out++] = static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
			output[out++] = static_cast<char>(0x80 | (ch & 0x3F));
		}
		pos += 2;
	}
	consumed = pos;
	written = out;
}

} // namespace

void BuiltinFunctions::RegisterEncodingFunctions() {
	CreateSchemaInfo schema;
	schema.SetQualifiedName(QualifiedName({Identifier(EncodingFunction::SCHEMA)}, Identifier()));
	schema.internal = true;
	schema.on_conflict = OnCreateConflict::IGNORE_ON_CONFLICT;
	catalog.CreateSchema(transaction, schema);

	for (auto &set : {EncodingFunction::Create("latin-1", DecodeLatin1ToUTF8, EncodeUTF8ToLatin1, 1, 2),
	                  EncodingFunction::Create("utf-16", DecodeUTF16ToUTF8, EncodeUTF8ToUTF16, 4, 2)}) {
		CreateScalarFunctionInfo info(set);
		EncodingFunction::SetSchema(info);
		info.internal = true;
		catalog.CreateFunction(transaction, info);
	}
}

} // namespace duckdb
