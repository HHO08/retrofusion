// RetroFusion - core option schema lookups + persistence
// SPDX-License-Identifier: GPL-2.0-or-later
#include "core_options.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "path_util.h"

namespace rf {

// Generated from cores/core_options.json by tools/gen_cpp_tables.py.
// Contains kSchemas[]: per-core option specs with their potato-profile defaults.
#include "generated/options_data.inc"

const CoreOptionSchema *SchemaForCore(const std::string &core_id) {
	for (size_t i = 0; i < kSchemaCount; ++i) {
		if (core_id == kSchemas[i].core_id) {
			return &kSchemas[i];
		}
	}
	return nullptr;
}

const CoreOptionSchema *AllSchemas() { return kSchemas; }
size_t AllSchemaCount() { return kSchemaCount; }

bool OptionToBool(const std::string &value) {
	const std::string v = Lowercase(value);
	return v == "enabled" || v == "true" || v == "1" || v == "on" || v == "yes";
}

const char *BoolToOption(bool value) { return value ? "enabled" : "disabled"; }

// ---------------------------------------------------------------------------
// Persistence: <config>/cores.ini
//   [mupen64plus_next]
//   mupen64plus-cpucore = dynamic_recompiler
// ---------------------------------------------------------------------------
bool CoreOptionsStore::Load(const std::string &ini_path) {
	path_ = ini_path;
	values_.clear();

	std::vector<char> buf;
	if (!ReadWholeFile(ini_path, &buf)) {
		return false;  // first run: no file yet is not an error
	}
	std::string text(buf.begin(), buf.end());

	std::string section;
	std::istringstream stream(text);
	std::string line;
	while (std::getline(stream, line)) {
		const std::string trimmed = Trim(line);
		if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') continue;
		if (trimmed.front() == '[' && trimmed.back() == ']') {
			section = trimmed.substr(1, trimmed.size() - 2);
			continue;
		}
		const size_t eq = trimmed.find('=');
		if (eq == std::string::npos || section.empty()) continue;
		const std::string key = Trim(trimmed.substr(0, eq));
		const std::string value = Trim(trimmed.substr(eq + 1));
		if (!key.empty()) values_[section][key] = value;
	}
	return true;
}

bool CoreOptionsStore::Save() const {
	if (path_.empty()) return false;
	std::string out =
		"# RetroFusion per-core settings.\n"
		"# Safe to edit by hand; the in-game Settings screen writes here too.\n\n";
	for (const auto &core : values_) {
		if (core.second.empty()) continue;
		out += "[" + core.first + "]\n";
		for (const auto &kv : core.second) {
			out += kv.first + " = " + kv.second + "\n";
		}
		out += "\n";
	}
	return WriteWholeFile(path_, out.data(), out.size());
}

std::string CoreOptionsStore::Effective(const std::string &core_id, const OptionSpec &spec) const {
	auto core = values_.find(core_id);
	if (core != values_.end()) {
		auto it = core->second.find(spec.key);
		if (it != core->second.end() && !it->second.empty()) {
			return it->second;
		}
	}
	// The weak-device profile from the work order is the *default*, not a fallback:
	// a fresh install must already behave well on a low-end phone.
	if (spec.potato_value && spec.potato_value[0]) return spec.potato_value;
	return spec.default_value ? spec.default_value : "";
}

std::map<std::string, std::string> CoreOptionsStore::EffectiveMap(const std::string &core_id) const {
	std::map<std::string, std::string> out;
	const CoreOptionSchema *schema = SchemaForCore(core_id);
	if (schema) {
		for (size_t i = 0; i < schema->count; ++i) {
			out[schema->specs[i].key] = Effective(core_id, schema->specs[i]);
		}
	}
	// Anything the user set that is not in our schema still reaches the core.
	auto core = values_.find(core_id);
	if (core != values_.end()) {
		for (const auto &kv : core->second) {
			if (!kv.second.empty()) out[kv.first] = kv.second;
		}
	}
	return out;
}

void CoreOptionsStore::Set(const std::string &core_id, const std::string &key, const std::string &value) {
	values_[core_id][key] = value;
}

// Both resets simply forget the user's choices: Effective() then resolves to
// the profile. This keeps three states distinguishable - "never touched",
// "explicitly set", "reset" - which a settings screen needs to show a
// "modified" marker without writing 20 keys per core.
void CoreOptionsStore::ResetToPotato(const std::string &core_id) {
	values_.erase(core_id);
}

void CoreOptionsStore::ResetToBalanced(const std::string &core_id) {
	// The balanced profile is expressed as an override of the potato defaults;
	// leaving the section empty means "use the profile defaults".
	values_.erase(core_id);
}

bool CoreOptionsStore::IsUserSet(const std::string &core_id, const std::string &key) const {
	auto core = values_.find(core_id);
	if (core == values_.end()) return false;
	auto it = core->second.find(key);
	return it != core->second.end() && !it->second.empty();
}

std::string CoreOptionsStore::Fingerprint(const std::string &core_id) const {
	std::string acc = core_id;
	const std::map<std::string, std::string> map = EffectiveMap(core_id);
	for (const auto &kv : map) {
		acc += "|" + kv.first + "=" + kv.second;
	}
	char buf[24];
	snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)Fnv1a64(acc));
	return buf;
}

}  // namespace rf
