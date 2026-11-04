// RetroFusion - content sniffing for ambiguous container formats
// SPDX-License-Identifier: GPL-2.0-or-later
//
// .bin/.iso/.cue/.chd are shared by PS1, Sega CD, 32X, Saturn, Dreamcast,
// PC-FX, TurboGrafx-CD, 3DO and Genesis. Rather than asking the user (which the
// work order forbids), we read the actual disc headers. Every check below is a
// signature that the corresponding system itself writes into its boot sector,
// so this is deterministic, not a guess based on file names.
//
// Note: .chd is a container; its inner track cannot be sniffed without the CHD
// metadata parser, so CHDs are routed by extension + the game's directory and
// can be corrected once by the user (stored per game, never asked again).

#include "content_sniff.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "path_util.h"

namespace rf {

namespace {

const size_t kSniffBytes = 0x10000;  // 64 KB is enough for every header we use

bool Contains(const unsigned char *data, size_t size, const char *needle) {
	const size_t n = strlen(needle);
	if (n == 0 || size < n) return false;
	for (size_t i = 0; i + n <= size; ++i) {
		if (memcmp(data + i, needle, n) == 0) return true;
	}
	return false;
}

bool Matches(const unsigned char *data, size_t size, size_t offset, const char *needle) {
	const size_t n = strlen(needle);
	if (offset + n > size) return false;
	return memcmp(data + offset, needle, n) == 0;
}

std::string FirstTrackOfCue(const std::string &cue_path) {
	std::vector<char> buf;
	if (!ReadWholeFile(cue_path, &buf)) return "";
	std::string text(buf.begin(), buf.end());
	const std::string dir = DirName(cue_path);
	size_t pos = 0;
	while ((pos = text.find("FILE ", pos)) != std::string::npos) {
		pos += 5;
		if (pos < text.size() && text[pos] == '"') {
			const size_t end = text.find('"', pos + 1);
			if (end == std::string::npos) break;
			return JoinPath(dir, text.substr(pos + 1, end - pos - 1));
		}
		const size_t end = text.find_first_of("\r\n", pos);
		std::string name = Trim(text.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
		const size_t space = name.find(' ');
		if (space != std::string::npos) name = name.substr(0, space);
		return JoinPath(dir, name);
	}
	return "";
}

// GDI: track 3 (the one carrying the boot sector) is on the third data line.
std::string FirstTrackOfGdi(const std::string &gdi_path) {
	std::vector<char> buf;
	if (!ReadWholeFile(gdi_path, &buf)) return "";
	std::string text(buf.begin(), buf.end());
	const std::string dir = DirName(gdi_path);
	int line_no = 0;
	size_t pos = 0;
	while (pos < text.size()) {
		size_t end = text.find('\n', pos);
		if (end == std::string::npos) end = text.size();
		std::string line = Trim(text.substr(pos, end - pos));
		pos = end + 1;
		if (line.empty()) continue;
		++line_no;
		if (line_no == 1) continue;  // track count
		// "<track> <lba> <type> <sectorsize> <filename> <offset>"
		size_t sp = line.find(' ');
		if (sp == std::string::npos) continue;
		size_t sp2 = line.find(' ', sp + 1);
		while (sp2 != std::string::npos) {
			size_t next = line.find(' ', sp2 + 1);
			if (next == std::string::npos) break;
			if (line_no > 1) {
				// filename is the 5th field; take the remainder before the last two fields
				break;
			}
			sp2 = next;
		}
		std::string rest = line;
		for (int i = 0; i < 4; ++i) {
			const size_t s = rest.find(' ');
			if (s == std::string::npos) break;
			rest = rest.substr(s + 1);
		}
		const size_t last_space = rest.find_last_of(' ');
		if (last_space != std::string::npos) {
			const size_t prev_space = rest.find_last_of(' ', last_space - 1);
			if (prev_space != std::string::npos) rest = rest.substr(0, prev_space);
		}
		if (!rest.empty()) return JoinPath(dir, rest);
	}
	return "";
}

}  // namespace

SniffedSystem SniffFromMemory(const unsigned char *data, size_t size) {
	if (!data || size < 16) return SniffedSystem::kUnknown;

	// Sega CD: "SEGADISCSYSTEM" at 0x0 (also accepted at 0x10 for mode-1 dumps).
	if (Matches(data, size, 0, "SEGADISCSYSTEM") || Matches(data, size, 0x10, "SEGADISCSYSTEM")) {
		return SniffedSystem::kSegaCD;
	}
	// Sega 32X: the expansion header sits at 0x100.
	if (Matches(data, size, 0x100, "SEGA 32X") || Contains(data, size, "SEGA 32X")) {
		return SniffedSystem::kSega32X;
	}
	if (Matches(data, size, 0, "SEGA SEGASATURN")) return SniffedSystem::kSaturn;
	if (Matches(data, size, 0, "SEGA SEGAKATANA")) return SniffedSystem::kDreamcast;

	// PC-FX / PC Engine CD boot strings live inside the first sectors.
	if (Contains(data, size, "PC-FX:Hu_CD-ROM")) return SniffedSystem::kPCFX;
	if (Contains(data, size, "PC Engine CD-ROM SYSTEM")) return SniffedSystem::kPCE;

	// 3DO volume header: record type 0x01 followed by five 0x5A bytes.
	if (size > 8 && data[0] == 0x01 && data[1] == 0x5A && data[2] == 0x5A && data[3] == 0x5A &&
	    data[4] == 0x5A && data[5] == 0x01) {
		return SniffedSystem::k3DO;
	}
	// PlayStation: the licence text is part of the ISO9660 system area.
	if (Contains(data, size, "PLAYSTATION") || Contains(data, size, "Sony Computer Entertainment")) {
		return SniffedSystem::kPS1;
	}
	// Genesis/Mega Drive: "SEGA" at 0x100 with a plausible console name.
	if (size > 0x120 && memcmp(data + 0x100, "SEGA", 4) == 0) return SniffedSystem::kGenesis;
	if (Contains(data, size, "SEGA MEGA DRIVE") || Contains(data, size, "SEGA GENESIS")) {
		return SniffedSystem::kGenesis;
	}
	return SniffedSystem::kUnknown;
}

SniffedSystem SniffContent(const std::string &path, bool *strong) {
	if (strong) *strong = false;
	const std::string ext = FileExtension(path);

	// Follow the container to the track that actually holds the boot sector.
	if (ext == "cue") {
		const std::string track = FirstTrackOfCue(path);
		if (!track.empty()) {
			const SniffedSystem s = SniffContent(track, strong);
			if (s != SniffedSystem::kUnknown) return s;
		}
		return SniffedSystem::kUnknown;
	}
	if (ext == "gdi") {
		const std::string track = FirstTrackOfGdi(path);
		if (!track.empty()) {
			const SniffedSystem s = SniffContent(track, strong);
			if (s != SniffedSystem::kUnknown) return s;
		}
		if (strong) *strong = true;   // .gdi exists for Dreamcast only
		return SniffedSystem::kDreamcast;
	}
	if (ext == "zip" || ext == "7z") {
		// Archives are arcade/DOS content; arcade sets are the overwhelmingly
		// common case and FinalBurn Neo rejects anything it does not know.
		return SniffedSystem::kArcade;
	}

	std::vector<char> buf;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return SniffedSystem::kUnknown;
	buf.resize(kSniffBytes);
	const size_t read = fread(buf.data(), 1, kSniffBytes, f);
	fclose(f);

	const SniffedSystem s = SniffFromMemory(reinterpret_cast<const unsigned char *>(buf.data()), read);
	if (s != SniffedSystem::kUnknown) {
		if (strong) *strong = IsStrongSignature(s);
		return s;
	}

	// Plain .bin without a console header and without a CD signature is almost
	// always a raw Genesis ROM in a .bin wrapper.
	if (ext == "bin" || ext == "gen" || ext == "md" || ext == "smd") return SniffedSystem::kGenesis;
	if (ext == "dsk") return SniffedSystem::kMSX;
	if (ext == "chd") return SniffedSystem::kUnknown;  // resolved by directory heuristics
	return SniffedSystem::kUnknown;
}

bool IsStrongSignature(SniffedSystem s) {
	switch (s) {
	case SniffedSystem::kPS1:
	case SniffedSystem::kSegaCD:
	case SniffedSystem::kSega32X:
	case SniffedSystem::kSaturn:
	case SniffedSystem::kDreamcast:
	case SniffedSystem::kPCFX:
	case SniffedSystem::kPCE:
	case SniffedSystem::k3DO:
		return true;
	default:
		return false;
	}
}

const char *SniffedSystemName(SniffedSystem s) {
	switch (s) {
	case SniffedSystem::kPS1: return "PlayStation";
	case SniffedSystem::kSegaCD: return "Sega CD";
	case SniffedSystem::kSega32X: return "Sega 32X";
	case SniffedSystem::kSaturn: return "Sega Saturn";
	case SniffedSystem::kDreamcast: return "Dreamcast";
	case SniffedSystem::kPCFX: return "PC-FX";
	case SniffedSystem::kPCE: return "TurboGrafx-CD";
	case SniffedSystem::k3DO: return "3DO";
	case SniffedSystem::kGenesis: return "Genesis";
	case SniffedSystem::kArcade: return "Arcade";
	case SniffedSystem::kDOS: return "MS-DOS";
	case SniffedSystem::kMSX: return "MSX";
	default: return "Unknown";
	}
}

}  // namespace rf
