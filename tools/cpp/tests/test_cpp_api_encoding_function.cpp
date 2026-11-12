#include "catch.hpp"
#include "duckdb_cpp.hpp"
#include "duckdb_v2.h"
#include "test_cpp_api.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Stable C++ API tests: EncodingFunction. Two toy encodings are registered and read through read_csv:
//  - toy1: one byte per character. Lower case letters decode to upper case, 0xE9 decodes to the two UTF-8 bytes of
//    U+00E9, everything else passes through.
//  - toy2: a byte pair 0xFE c decodes to the three bytes "<c>"; every other byte passes through. A 0xFE that ends the
//    file decodes to "?".
// ---------------------------------------------------------------------------

namespace {

using namespace duckdb::cxx;

void Toy1Decode(EncodingFunction::DecodeInput &input) {
	const auto bytes = input.GetInput();
	auto output = input.GetOutput();
	idx_t consumed = 0;
	idx_t written = 0;
	while (consumed < bytes.size()) {
		const auto c = static_cast<uint8_t>(bytes[consumed]);
		if (c == 0xE9) {
			if (output.second - written < 2) {
				break;
			}
			output.first[written++] = static_cast<char>(0xC3);
			output.first[written++] = static_cast<char>(0xA9);
		} else {
			if (output.second - written < 1) {
				break;
			}
			output.first[written++] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : static_cast<char>(c);
		}
		consumed++;
	}
	input.SetResult(consumed, written);
}

void Toy2Decode(EncodingFunction::DecodeInput &input) {
	const auto bytes = input.GetInput();
	auto output = input.GetOutput();
	idx_t consumed = 0;
	idx_t written = 0;
	while (consumed < bytes.size()) {
		const auto c = static_cast<uint8_t>(bytes[consumed]);
		if (c == 0xFE) {
			if (consumed + 1 == bytes.size() && !input.IsLast()) {
				break;
			}
			const bool paired = consumed + 1 < bytes.size();
			const idx_t out_size = paired ? 3 : 1;
			if (output.second - written < out_size) {
				break;
			}
			if (paired) {
				output.first[written++] = '<';
				output.first[written++] = bytes[consumed + 1];
				output.first[written++] = '>';
				consumed += 2;
			} else {
				output.first[written++] = '?';
				consumed++;
			}
			continue;
		}
		if (output.second - written < 1) {
			break;
		}
		output.first[written++] = static_cast<char>(c);
		consumed++;
	}
	input.SetResult(consumed, written);
}

// Carried through SetUserData to pin that the decode callback sees it.
struct ShiftTag {
	int shift;
};

// "shifted": every byte decodes to itself plus the shift of the user data.
void ShiftedDecode(EncodingFunction::DecodeInput &input) {
	const auto shift = input.GetUserData<ShiftTag>().shift;
	const auto bytes = input.GetInput();
	auto output = input.GetOutput();
	const auto count = std::min<idx_t>(bytes.size(), output.second);
	for (idx_t i = 0; i < count; i++) {
		output.first[i] = static_cast<char>(static_cast<uint8_t>(bytes[i]) + shift);
	}
	input.SetResult(count, count);
}

void FailingDecode(EncodingFunction::DecodeInput &) {
	throw InvalidInputException("this encoding cannot decode anything");
}

void NoopDecode(EncodingFunction::DecodeInput &) {
}

void RegisterToy(Connection &conn, const std::string &name, idx_t max_input, idx_t max_output,
                 EncodingFunction::DecodeCallback callback) {
	auto function = EncodingFunction::Create(conn.GetFactory());
	function.SetName(name).SetMaxInputBytes(max_input).SetMaxOutputBytes(max_output).SetDecodeCallback(callback);
	conn.Register(function);
}

std::string WriteFile(const std::string &name, const std::string &content) {
	auto path = duckdb::TestCreatePath(name);
	std::ofstream file(path, std::ios::binary);
	file.write(content.data(), static_cast<std::streamsize>(content.size()));
	return path;
}

std::vector<std::string> CollectStrings(QueryResult result) {
	std::vector<std::string> out;
	while (auto chunk = result.FetchChunk()) {
		auto view = chunk.GetVector(0).GetView();
		for (idx_t i = 0; i < chunk.GetRowCount(); i++) {
			REQUIRE(view.IsValid(i));
			out.emplace_back(view.Data<varchar_t>()[view.SelAt(i)].view());
		}
	}
	return out;
}

std::vector<int64_t> CollectInts(QueryResult result) {
	std::vector<int64_t> out;
	while (auto chunk = result.FetchChunk()) {
		auto view = chunk.GetVector(0).GetView();
		for (idx_t i = 0; i < chunk.GetRowCount(); i++) {
			REQUIRE(view.IsValid(i));
			out.emplace_back(view.Data<int64_t>()[view.SelAt(i)]);
		}
	}
	return out;
}

// A buffer of a few dozen bytes, so that a file of a few thousand rows spans thousands of buffers.
constexpr const char *SMALL_BUFFER = ", buffer_size = 64";

std::string ReadCSV(const std::string &path, const std::string &encoding, const std::string &options = "") {
	return "SELECT a FROM read_csv('" + path + "', encoding = '" + encoding +
	       "', header = false, columns = {'a': 'VARCHAR'}" + options + ")";
}

} // namespace

TEST_CASE("Stable C++API: encoding function decodes read_csv input", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterToy(conn, "toy1", 1, 2, Toy1Decode);

	auto path = WriteFile("cpp_encoding_toy1.csv", std::string("abc\nd\xE9"
	                                                           "f\nxyz\n"));
	REQUIRE(CollectStrings(conn.Execute(ReadCSV(path, "toy1"))) == std::vector<std::string> {"ABC",
	                                                                                         "D\xC3\xA9"
	                                                                                         "F",
	                                                                                         "XYZ"});
	// The name matches case-insensitively.
	REQUIRE(CollectStrings(conn.Execute(ReadCSV(path, "TOY1"))).size() == 3);
}

TEST_CASE("Stable C++API: encoding function over many buffers", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	RegisterToy(conn, "toy1", 1, 2, Toy1Decode);
	RegisterToy(conn, "toy2", 2, 3, Toy2Decode);

	// Enough rows to span many buffers, with multi-byte output at every buffer boundary there is.
	constexpr idx_t ROWS = 3000;
	std::string toy1;
	std::string toy2;
	for (idx_t i = 0; i < ROWS; i++) {
		toy1 += "\xE9\xE9"
		        "ab\n";
		toy2 += "\xFE"
		        "x\xFE"
		        "y!\n";
	}
	toy2 += "\xFE";

	auto toy1_path = WriteFile("cpp_encoding_toy1_large.csv", toy1);
	REQUIRE(CollectInts(conn.Execute("SELECT count(*) FROM (" + ReadCSV(toy1_path, "toy1", SMALL_BUFFER) +
	                                 ") WHERE a = '\xC3\xA9\xC3\xA9"
	                                 "AB'")) == std::vector<int64_t> {static_cast<int64_t>(ROWS)});

	auto toy2_path = WriteFile("cpp_encoding_toy2_large.csv", toy2);
	REQUIRE(CollectInts(conn.Execute("SELECT count(*) FROM (" + ReadCSV(toy2_path, "toy2", SMALL_BUFFER) +
	                                 ") WHERE a = '<x><y>!'")) == std::vector<int64_t> {static_cast<int64_t>(ROWS)});
	REQUIRE(CollectStrings(conn.Execute("SELECT a FROM (" + ReadCSV(toy2_path, "toy2", SMALL_BUFFER) +
	                                    ") WHERE a = '?'")) == std::vector<std::string> {"?"});
}

TEST_CASE("Stable C++API: encoding function user data", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	// The same function object registered under two names, each with its own user data.
	auto function = EncodingFunction::Create(conn.GetFactory());
	function.SetMaxInputBytes(1).SetMaxOutputBytes(1).SetDecodeCallback(ShiftedDecode);
	function.SetName("shift_up").SetUserData<ShiftTag>(ShiftTag {1});
	conn.Register(function);
	function.SetName("shift_down").SetUserData<ShiftTag>(ShiftTag {-1});
	conn.Register(function);

	auto path = WriteFile("cpp_encoding_shifted.csv", "bcd\n");
	REQUIRE(CollectStrings(conn.Execute(ReadCSV(path, "shift_up"))) == std::vector<std::string> {"cde\v"});
	REQUIRE(CollectStrings(conn.Execute(ReadCSV(path, "shift_down"))) == std::vector<std::string> {"abc\t"});
}

// encoding_info(name) -> BIGINT: max_input_bytes * 100 + max_output_bytes of the registered encoding, or -1 for none.
void EncodingInfoExec(ScalarFunction::ExecInput &input) {
	const auto encodings = input.GetContext().ListEncodings();
	auto view = input.GetArg(0).GetView();
	auto *out = input.GetResult().GetDataMutable<int64_t>();
	for (idx_t i = 0; i < input.GetRowCount(); i++) {
		const auto name = std::string(view.Data<varchar_t>()[view.SelAt(i)].view());
		out[i] = -1;
		for (auto &encoding : encodings) {
			if (encoding.name == name) {
				out[i] = static_cast<int64_t>(encoding.max_input_bytes * 100 + encoding.max_output_bytes);
			}
		}
	}
}

TEST_CASE("Stable C++API: list encodings", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();
	auto &factory = conn.GetFactory();
	RegisterToy(conn, "toy2", 2, 3, Toy2Decode);

	auto function = ScalarFunction::Create(conn.GetFactory());
	function.SetName("encoding_info");
	function.WithSignature([&](FunctionSignature &sig) {
		sig.AddParameter("name", factory.ParseType("VARCHAR"));
		sig.SetReturnType(factory.ParseType("BIGINT"));
	});
	function.SetExecCallback(EncodingInfoExec);
	conn.Register(function);

	REQUIRE(CollectInts(conn.Execute("SELECT encoding_info('toy2')")) == std::vector<int64_t> {203});
	// The built-in encodings are listed too.
	REQUIRE(CollectInts(conn.Execute("SELECT encoding_info('utf-16')")) == std::vector<int64_t> {403});
	REQUIRE(CollectInts(conn.Execute("SELECT encoding_info('utf-8')")) == std::vector<int64_t> {101});
	REQUIRE(CollectInts(conn.Execute("SELECT encoding_info('toy3')")) == std::vector<int64_t> {-1});
}

TEST_CASE("Stable C++API: encoding function errors", "[cpp_api]") {
	auto db = Instance(":memory:");
	auto conn = db.Connect();

	// No name.
	{
		auto function = EncodingFunction::Create(conn.GetFactory());
		function.SetMaxInputBytes(1).SetMaxOutputBytes(1).SetDecodeCallback(NoopDecode);
		REQUIRE_THROWS_MATCHES(conn.Register(function), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	// No input bytes.
	{
		auto function = EncodingFunction::Create(conn.GetFactory());
		function.SetName("no_input").SetMaxOutputBytes(1).SetDecodeCallback(NoopDecode);
		REQUIRE_THROWS_MATCHES(conn.Register(function), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	// No output bytes.
	{
		auto function = EncodingFunction::Create(conn.GetFactory());
		function.SetName("no_output").SetMaxInputBytes(1).SetDecodeCallback(NoopDecode);
		REQUIRE_THROWS_MATCHES(conn.Register(function), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	// No callback.
	{
		auto function = EncodingFunction::Create(conn.GetFactory());
		function.SetName("no_callback").SetMaxInputBytes(1).SetMaxOutputBytes(1);
		REQUIRE_THROWS_MATCHES(conn.Register(function), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	// A name that is already taken, by a built-in encoding or by an earlier registration.
	{
		auto function = EncodingFunction::Create(conn.GetFactory());
		function.SetName("UTF-16").SetMaxInputBytes(1).SetMaxOutputBytes(1).SetDecodeCallback(NoopDecode);
		REQUIRE_THROWS_MATCHES(conn.Register(function), Exception, HasErrorCode(DUCKDB_V2_ERROR_INPUT_INVALID));
	}
	// Reading the user data of an encoding that has none.
	{
		RegisterToy(conn, "no_user_data", 1, 1,
		            [](EncodingFunction::DecodeInput &input) { input.GetUserData<ShiftTag>(); });
		auto path = WriteFile("cpp_encoding_no_user_data.csv", "abc\n");
		REQUIRE_THROWS_AS(conn.Execute(ReadCSV(path, "no_user_data")), Exception);
	}
	// A callback that throws aborts the query.
	{
		RegisterToy(conn, "failing", 1, 1, FailingDecode);
		auto path = WriteFile("cpp_encoding_failing.csv", "abc\n");
		REQUIRE_THROWS_AS(conn.Execute(ReadCSV(path, "failing")), Exception);
	}
	// A callback that never consumes the input is reported rather than looping or silently truncating the file.
	{
		RegisterToy(conn, "stuck", 1, 1, NoopDecode);
		auto path = WriteFile("cpp_encoding_stuck.csv", "abc\n");
		REQUIRE_THROWS_AS(conn.Execute(ReadCSV(path, "stuck")), Exception);
	}
}
