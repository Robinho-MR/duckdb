#include "icu_common.hpp"

#include "jaro_winkler.hpp"

namespace duckdb_icu {

static bool CILessThan(const string &s1, const string &s2) {
	const idx_t length = MinValue<idx_t>(s1.size(), s2.size());
	for (idx_t i = 0; i < length; i++) {
		const auto u1 = std::toupper(static_cast<unsigned char>(s1[i]));
		const auto u2 = std::toupper(static_cast<unsigned char>(s2[i]));
		if (u1 != u2) {
			return u1 < u2;
		}
	}
	return s1.size() < s2.size();
}

vector<string> StringUtil::TopNJaroWinkler(const vector<string> &strings, const string &target, idx_t n,
                                           double threshold) {
	vector<pair<string, double>> scores;
	scores.reserve(strings.size());
	for (auto &str : strings) {
		scores.emplace_back(str,
		                    duckdb_jaro_winkler::jaro_winkler_similarity(str.data(), str.data() + str.size(),
		                                                                 target.data(), target.data() + target.size()));
	}
	if (scores.empty()) {
		return vector<string>();
	}
	std::sort(scores.begin(), scores.end(), [](const pair<string, double> &a, const pair<string, double> &b) {
		if (a.second != b.second) {
			return a.second > b.second;
		}
		return CILessThan(a.first, b.first);
	});
	vector<string> result;
	result.push_back(scores[0].first);
	for (idx_t i = 1; i < MinValue<idx_t>(scores.size(), n); i++) {
		if (scores[i].second < threshold) {
			break;
		}
		result.push_back(scores[i].first);
	}
	return result;
}

string StringUtil::CandidatesMessage(const vector<string> &candidates, const string &candidate) {
	string result_str;
	if (!candidates.empty()) {
		result_str = "\n" + candidate + ": ";
		for (idx_t i = 0; i < candidates.size(); i++) {
			if (i > 0) {
				result_str += ", ";
			}
			result_str += "\"" + candidates[i] + "\"";
		}
	}
	return result_str;
}

} // namespace duckdb_icu
