// RetroFusion - system/extension resolution
// SPDX-License-Identifier: GPL-2.0-or-later
#include "core_registry.h"

#include <string>
#include <vector>

#include "content_sniff.h"
#include "path_util.h"

namespace rf {

#include "generated/registry_data.inc"

const CoreEntry *CoreTable() { return kCores; }
size_t CoreTableSize() { return kCoreCount; }

const CoreEntry *FindCoreEntry(const std::string &core_id) {
	for (size_t i = 0; i < kCoreCount; ++i) {
		if (core_id == kCores[i].core_id) {
			// The PSP entry describes the untouched native core; it has no .so.
			return &kCores[i];
		}
	}
	return nullptr;
}

const CoreEntry *FindCoreForSystem(SystemId system) {
	for (size_t i = 0; i < kCoreCount; ++i) {
		if (kCores[i].system == system) return &kCores[i];
	}
	return nullptr;
}

const char *SystemDisplayName(SystemId system) {
	const CoreEntry *e = FindCoreForSystem(system);
	return e ? e->system_name : "Unknown";
}

const char *SystemShortCode(SystemId system) {
	switch (system) {
	case SystemId::kNES: return "nes";
	case SystemId::kFDS: return "fds";
	case SystemId::kGB: return "gb";
	case SystemId::kGBC: return "gbc";
	case SystemId::kGBA: return "gba";
	case SystemId::kN64: return "n64";
	case SystemId::kNDS: return "nds";
	case SystemId::kSNES: return "snes";
	case SystemId::kSG32X: return "32x";
	case SystemId::kDreamcast: return "dc";
	case SystemId::kGameGear: return "gg";
	case SystemId::kGenesis: return "gen";
	case SystemId::kMasterSystem: return "sms";
	case SystemId::kSegaCD: return "segacd";
	case SystemId::kSegaPico: return "pico";
	case SystemId::kSaturn: return "saturn";
	case SystemId::kPSP: return "psp";
	case SystemId::kPS1: return "psx";
	case SystemId::kPC88: return "pc88";
	case SystemId::kPC98: return "pc98";
	case SystemId::kPCFX: return "pcfx";
	case SystemId::kTG16: return "pce";
	case SystemId::kTGCD: return "pcecd";
	case SystemId::kNeoGeoCD: return "ngcd";
	case SystemId::kNGPC: return "ngpc";
	case SystemId::k3DO: return "3do";
	case SystemId::kAmiga: return "amiga";
	case SystemId::kArcade: return "arcade";
	case SystemId::kMSX: return "msx";
	case SystemId::kWonderSwan: return "ws";
	case SystemId::kMSDOS: return "dos";
	case SystemId::kScummVM: return "scummvm";
	default: return "unknown";
	}
}

namespace {

SystemId SystemFromSniff(SniffedSystem s) {
	switch (s) {
	case SniffedSystem::kPS1: return SystemId::kPS1;
	case SniffedSystem::kSegaCD: return SystemId::kSegaCD;
	case SniffedSystem::kSega32X: return SystemId::kSG32X;
	case SniffedSystem::kSaturn: return SystemId::kSaturn;
	case SniffedSystem::kDreamcast: return SystemId::kDreamcast;
	case SniffedSystem::kPCFX: return SystemId::kPCFX;
	case SniffedSystem::kPCE: return SystemId::kTGCD;
	case SniffedSystem::k3DO: return SystemId::k3DO;
	case SniffedSystem::kGenesis: return SystemId::kGenesis;
	case SniffedSystem::kArcade: return SystemId::kArcade;
	case SniffedSystem::kDOS: return SystemId::kMSDOS;
	case SniffedSystem::kMSX: return SystemId::kMSX;
	default: return SystemId::kUnknown;
	}
}

// Containers that legitimately belong to more than one system. For these the
// boot-sector signature always wins over the extension.
bool IsAmbiguousExtension(const std::string &ext) {
	static const char *kAmbiguous[] = { "bin", "iso", "cue", "chd", "gdi", "cdi",
	                                    "zip", "7z", "m3u", "img", "dsk", "rom" };
	for (const char *a : kAmbiguous) {
		if (ext == a) return true;
	}
	return false;
}

bool ExtensionMatches(const CoreEntry *entry, const std::string &ext) {
	const std::string exts = entry->exts;
	return exts.find("|" + ext + "|") != std::string::npos;
}

// Applies the ABI rule: a core that is 64-bit only is replaced by its declared
// fallback on armeabi-v7a (NDS -> DeSmuME), never silently dropped.
const CoreEntry *AdaptToAbi(const CoreEntry *entry, bool arm64) {
	if (!entry || arm64 || entry->armv7) return entry;
	if (entry->fallback_core_id && entry->fallback_core_id[0]) {
		const CoreEntry *fallback = FindCoreEntry(entry->fallback_core_id);
		if (fallback) return fallback;
	}
	return nullptr;
}

}  // namespace

Resolution Resolve(const std::string &path, bool arm64, const std::string &override_core_id) {
	Resolution result;

	// 1. Explicit per-game override always wins (user decision is final and is
	//    asked at most once, then remembered).
	if (!override_core_id.empty()) {
		const CoreEntry *entry = AdaptToAbi(FindCoreEntry(override_core_id), arm64);
		if (entry) {
			result.system = entry->system;
			result.core = entry;
			return result;
		}
	}

	const std::string ext = FileExtension(path);
	if (ext.empty()) return result;

	// 2. Unambiguous extension -> exactly one core.
	std::vector<const CoreEntry *> candidates;
	for (size_t i = 0; i < kCoreCount; ++i) {
		const CoreEntry *entry = &kCores[i];
		if (entry->system == SystemId::kPSP) continue;  // handled by PPSSPP itself
		if (!ExtensionMatches(entry, ext)) continue;
		if (!arm64 && !entry->armv7) continue;  // ABI-filtered later via fallback
		candidates.push_back(entry);
	}
	if (!arm64) {
		// add ABI fallbacks for cores that are 64-bit only
		for (size_t i = 0; i < kCoreCount; ++i) {
			const CoreEntry *entry = &kCores[i];
			if (entry->armv7 || !ExtensionMatches(entry, ext)) continue;
			const CoreEntry *fallback = AdaptToAbi(entry, false);
			if (fallback) candidates.push_back(fallback);
		}
	}

	// A strong signature beats an extension-only match - but only for ambiguous
	// containers, so a stray "SEGA" inside an unrelated ROM cannot hijack it.
	if (IsAmbiguousExtension(ext)) {
		bool strong = false;
		const SniffedSystem sniffed_early = SniffContent(path, &strong);
		if (strong) {
			const SystemId sys = SystemFromSniff(sniffed_early);
			const CoreEntry *entry = AdaptToAbi(FindCoreForSystem(sys), arm64);
			if (entry) {
				result.system = sys;
				result.core = entry;
				result.via_sniff = true;
				return result;
			}
		}
	}

	if (candidates.size() == 1) {
		result.system = candidates[0]->system;
		result.core = candidates[0];
		return result;
	}

	// 3. Ambiguous (.cue/.chd/.iso/.bin/.zip): sniff the actual bytes.
	//    A *strong* result (real boot-sector signature) outranks the extension,
	//    because a .bin/.iso can legitimately belong to six different systems.
	bool strong = false;
	const SniffedSystem sniffed = SniffContent(path, &strong);
	const SystemId sniffed_system = SystemFromSniff(sniffed);
	result.via_sniff = sniffed != SniffedSystem::kUnknown;

	if (strong && sniffed_system != SystemId::kUnknown) {
		const CoreEntry *entry = AdaptToAbi(FindCoreForSystem(sniffed_system), arm64);
		if (entry) {
			result.system = sniffed_system;
			result.core = entry;
			return result;
		}
	}

	if (sniffed_system != SystemId::kUnknown) {
		for (const CoreEntry *candidate : candidates) {
			if (candidate->system == sniffed_system) {
				result.system = sniffed_system;
				result.core = candidate;
				return result;
			}
		}
	}

	if (candidates.empty()) {
		// Nothing matched: still offer the sniffed system so the caller can show
		// a precise message ("looks like a Sega Saturn disc, but that core is
		// missing on this ABI").
		if (sniffed_system != SystemId::kUnknown) {
			result.system = sniffed_system;
			result.core = AdaptToAbi(FindCoreForSystem(sniffed_system), arm64);
		}
		return result;
	}

	// 4. Several plausible cores and sniffing did not decide: pick the first by a
	//    stable priority (the manifest order) but tell the caller it was a guess
	//    so the UI can offer a one-time correction.
	result.ambiguous = candidates.size() > 1;
	result.candidates = candidates;
	result.core = candidates[0];
	result.system = candidates[0]->system;
	return result;
}

}  // namespace rf
