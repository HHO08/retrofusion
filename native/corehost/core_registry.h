// RetroFusion - system/extension registry (portable layer)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Maps a content file to the core that must run it. Two-stage resolution:
//   1. unambiguous extensions (.gba, .n64, .nds, ...) -> dictionary lookup
//   2. ambiguous containers (.bin .iso .cue .chd .zip) -> content sniffing
//      (see content_sniff.cpp) plus a per-game user override, which always wins.
//
// The table is generated from cores/manifest.json at build time
// (see tools/gen_registry.py) into core_registry_data.inc so the C++ side and
// the packaging side can never disagree.

#pragma once

#include <string>
#include <vector>

namespace rf {

enum class SystemId {
	kUnknown = 0,
	// Nintendo
	kNES, kFDS, kGB, kGBC, kGBA, kN64, kNDS, kSNES,
	// Sega
	kSG32X, kDreamcast, kGameGear, kGenesis, kMasterSystem, kSegaCD, kSegaPico, kSaturn,
	// Sony
	kPSP, kPS1,
	// NEC
	kPC88, kPC98, kPCFX, kTG16, kTGCD,
	// SNK
	kNeoGeoCD, kNGPC,
	// Other
	k3DO, kAmiga, kArcade, kMSX, kWonderSwan,
	// PC
	kMSDOS, kScummVM,
	kCount
};

struct CoreEntry {
	SystemId system;
	const char *system_name;      // "Nintendo DS"
	const char *core_id;          // "melonds" (also the .so basename)
	const char *core_so;          // "melonds_libretro_android.so"
	const char *core_display;     // "melonDS (RetroFusion build)"
	const char *exts;             // "|nds|srl|"  (lowercase, pipe delimited)
	bool armv7;                   // built for armeabi-v7a too?
	bool needs_bios;              // firmware required for at least some titles
	const char *fallback_core_id; // "" when none
};

struct Resolution {
	SystemId system = SystemId::kUnknown;
	const CoreEntry *core = nullptr;
	bool via_sniff = false;
	bool ambiguous = false;          // true -> caller may ask the user, once
	std::vector<const CoreEntry *> candidates;
};

// Returns the core table (generated).
const CoreEntry *CoreTable();
size_t CoreTableSize();
const CoreEntry *FindCoreEntry(const std::string &core_id);
const CoreEntry *FindCoreForSystem(SystemId system);

// Resolve which core runs `path`. `arm64` selects the ABI-specific table.
// `override_core_id` ("" when none) comes from the per-game override store.
Resolution Resolve(const std::string &path, bool arm64, const std::string &override_core_id = "");

// "Nintendo DS" for UI strings / achievement labels.
const char *SystemDisplayName(SystemId system);
const char *SystemShortCode(SystemId system);   // "nds", used for save subdirs

// Empty string when the file is a PSP image (handled by the untouched PPSSPP core).
inline bool IsPspContent(const std::string &ext) {
	return ext == "iso" || ext == "cso" || ext == "pbp" || ext == "elf" || ext == "prx";
}

}  // namespace rf
