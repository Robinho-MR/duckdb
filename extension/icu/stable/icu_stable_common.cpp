#include "icu_stable_common.hpp"

#include <map>
#include <memory>
#include <utility>

namespace icu_stable {

void ThrowConversion(const std::string &message) {
	throw cxx::Exception(DUCKDB_V2_ERROR_TYPE_CONVERSION, message);
}

static std::string ReadString(const cxx::Config &config, const char *name, const std::string &fallback) {
	auto option = config.TryGetOption(name);
	if (!option || option->value.IsNull()) {
		return fallback;
	}
	return std::string(option->value.Get<cxx::varchar_t>().view());
}

Settings ReadSettings(cxx::Context &context) {
	Settings settings;
	const auto &config = context.GetConfig();
	settings.time_zone = ReadString(config, "TimeZone", settings.time_zone);
	settings.calendar = ReadString(config, "Calendar", settings.calendar);
	return settings;
}

icu_bridge::Calendar &SettingsCalendar(const Settings &settings) {
	thread_local std::unique_ptr<icu_bridge::Calendar> cached;
	thread_local Settings cached_settings;
	if (!cached || !(cached_settings == settings)) {
		cached = icu_bridge::Calendar::Create(settings.time_zone, settings.calendar);
		cached_settings = settings;
	}
	return *cached;
}

icu_bridge::Calendar *ZoneCalendar(const Settings &settings, const std::string &zone, std::string &error) {
	struct Entry {
		std::unique_ptr<icu_bridge::Calendar> calendar;
		std::string error;
	};
	thread_local std::map<std::pair<std::string, std::string>, Entry> cache;
	auto key = std::make_pair(settings.calendar, zone);
	auto entry = cache.find(key);
	if (entry == cache.end()) {
		Entry created;
		auto resolved = zone;
		if (icu_bridge::ResolveTimeZone(resolved, &created.error)) {
			created.calendar = icu_bridge::Calendar::Create(resolved, settings.calendar);
		}
		entry = cache.emplace(std::move(key), std::move(created)).first;
	}
	error = entry->second.error;
	return entry->second.calendar.get();
}

} // namespace icu_stable
