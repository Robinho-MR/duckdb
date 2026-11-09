// Reading JSON files: read_json and friends (functions/read_json.cpp), the replacement scan that turns
// FROM 'x.json' into read_json_auto, and COPY ... FROM (functions/copy_json_from.cpp).

#include "json_geojson.hpp"
#include "json_scan.hpp"

namespace duckdb {
namespace json {

//===--------------------------------------------------------------------===//
// Replacement scan
//===--------------------------------------------------------------------===//
static bool CanReplace(const string &table_name) {
	auto lower_name = StringUtil::Lower(table_name);
	const auto ends_with = [](const string &str, const string &suffix) {
		return str.size() >= suffix.size() && str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
	};
	if (ends_with(lower_name, ".gz")) {
		lower_name = lower_name.substr(0, lower_name.size() - 3);
	} else if (ends_with(lower_name, ".zst")) {
		lower_name = lower_name.substr(0, lower_name.size() - 4);
	}
	for (auto extension : {"json", "jsonl", "ndjson", "geojson", "geojsonl"}) {
		const auto suffix = string(".") + extension;
		if (ends_with(lower_name, suffix) || lower_name.find(suffix + "?") != string::npos) {
			return true;
		}
	}
	return false;
}

static bool HasGlob(const string &str) {
	return str.find_first_of("*?[") != string::npos;
}

static void ReadJSONReplacement(cxx::ReplacementScan::Input &input) {
	auto name = input.GetName();
	// the components of the name, joined: a path with dots in it is not a qualified name
	string table_name;
	for (idx_t i = 0; i < name.GetPartCount(); i++) {
		auto part = name.GetPart(i);
		if (part.empty()) {
			continue;
		}
		table_name += (!table_name.empty() ? "." : "") + string(part);
	}
	if (!CanReplace(table_name)) {
		return;
	}
	auto &factory = input.GetContext().GetFactory();
	input.SetFunctionName("read_json_auto");
	input.AddArgument(Value::Create(factory, varchar_t(table_name)));
	if (!HasGlob(table_name)) {
		// the alias is the name of the file up to its first dot
		auto slash = table_name.find_last_of('/');
		auto base_name = slash == string::npos ? table_name : table_name.substr(slash + 1);
		base_name = base_name.substr(0, base_name.find('.'));
		input.SetAlias(base_name);
	}
}

void RegisterReader(Extension &extension, Context &context) {
	// GeoJSON geometries become GEOMETRY leaves when the schema of a GeoJSON file is detected
	JSONScan::SetGeoJSONGeometryDetector(IsGeoJSONGeometry);
	RegisterReadJSON(extension, context);

	auto scan = cxx::ReplacementScan::Create(context.GetFactory());
	scan.SetCallback(ReadJSONReplacement);
	extension.Register(scan);
}

} // namespace json
} // namespace duckdb
