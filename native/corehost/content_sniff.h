// RetroFusion - content sniffing for ambiguous container formats
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The work order requires that the user never picks a system by hand. Most
// files can be routed by extension alone, but .bin/.iso/.cue/.chd/.zip are
// shared by up to six systems. This sniffs the actual bytes, in the cheapest
// order possible (header first, then the cue sheet's referenced track names).

#pragma once

#include <string>

namespace rf {

enum class SniffedSystem {
	kUnknown = 0,
	kPS1,
	kSegaCD,
	kSega32X,
	kSaturn,
	kDreamcast,
	kPCFX,
	kPCE,       // TurboGrafx-CD
	k3DO,
	kGenesis,
	kArcade,    // archive that is not a known console dump
	kDOS,
	kMSX,
};

// Reads at most a few KB from `path` (follows .cue/.gdi to its first track).
// `strong` (optional) is set when the result comes from a real boot-sector
// signature rather than from a generic extension guess; only strong results are
// allowed to override an unambiguous file extension.
SniffedSystem SniffContent(const std::string &path, bool *strong = nullptr);

// True for systems identified by their own header bytes.
bool IsStrongSignature(SniffedSystem s);

// Exposed for tests.
SniffedSystem SniffFromMemory(const unsigned char *data, size_t size);

const char *SniffedSystemName(SniffedSystem s);

}  // namespace rf
