// RetroFusion - small POSIX helpers shared by the portable layer.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rf {

bool EnsureDirectory(const std::string &path);          // mkdir -p, true if it exists afterwards
bool FileExists(const std::string &path);
bool ReadWholeFile(const std::string &path, std::vector<char> *out);
bool WriteWholeFile(const std::string &path, const void *data, size_t size);
bool RemoveFile(const std::string &path);
std::vector<std::string> ListDirectory(const std::string &path);  // full paths, sorted

std::string FileStem(const std::string &path);       // "Game"  from "/a/b/Game.nds"
std::string FileName(const std::string &path);       // "Game.nds"
std::string FileExtension(const std::string &path);  // "nds" (lowercase, no dot)
std::string DirName(const std::string &path);        // "/a/b"
std::string JoinPath(const std::string &a, const std::string &b);

std::string Lowercase(std::string s);
std::string Trim(const std::string &s);

// Simple non-cryptographic 64-bit FNV-1a, used for option-set fingerprints.
uint64_t Fnv1a64(const std::string &s);

}  // namespace rf
