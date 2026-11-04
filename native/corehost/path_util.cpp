// RetroFusion - POSIX helpers (portable layer)
// SPDX-License-Identifier: GPL-2.0-or-later
#include "path_util.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace rf {

bool EnsureDirectory(const std::string &path) {
	if (path.empty()) return false;
	struct stat st;
	if (stat(path.c_str(), &st) == 0) {
		return S_ISDIR(st.st_mode);
	}
	// Recursive create, mirroring mkdir -p.
	std::string cur;
	for (size_t i = 0; i < path.size(); ++i) {
		cur.push_back(path[i]);
		const bool last = (i + 1 == path.size());
		if (path[i] == '/' || last) {
			if (cur == "/" || cur.empty()) continue;
			mkdir(cur.c_str(), 0775);
		}
	}
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool FileExists(const std::string &path) {
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool ReadWholeFile(const std::string &path, std::vector<char> *out) {
	if (!out) return false;
	out->clear();
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return false;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return false;
	}
	const long size = ftell(f);
	if (size < 0) {
		fclose(f);
		return false;
	}
	rewind(f);
	out->resize((size_t)size);
	const size_t read = size > 0 ? fread(out->data(), 1, (size_t)size, f) : 0;
	fclose(f);
	out->resize(read);
	return true;
}

bool WriteWholeFile(const std::string &path, const void *data, size_t size) {
	if (path.empty()) return false;
	// Write to a temp file then rename: a crash mid-write must never leave a
	// truncated SRAM/savestate behind.
	const std::string tmp = path + ".tmp";
	FILE *f = fopen(tmp.c_str(), "wb");
	if (!f) return false;
	const size_t written = size ? fwrite(data, 1, size, f) : 0;
	fflush(f);
	fclose(f);
	if (written != size) {
		remove(tmp.c_str());
		return false;
	}
	remove(path.c_str());
	return rename(tmp.c_str(), path.c_str()) == 0;
}

bool RemoveFile(const std::string &path) {
	return remove(path.c_str()) == 0;
}

std::vector<std::string> ListDirectory(const std::string &path) {
	std::vector<std::string> out;
	DIR *dir = opendir(path.c_str());
	if (!dir) return out;
	while (struct dirent *entry = readdir(dir)) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
		if (entry->d_type == DT_DIR) continue;
		out.push_back(JoinPath(path, entry->d_name));
	}
	closedir(dir);
	std::sort(out.begin(), out.end());
	return out;
}

std::string FileName(const std::string &path) {
	const size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string DirName(const std::string &path) {
	const size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string FileExtension(const std::string &path) {
	const std::string name = FileName(path);
	const size_t dot = name.find_last_of('.');
	if (dot == std::string::npos || dot + 1 >= name.size()) return "";
	return Lowercase(name.substr(dot + 1));
}

std::string FileStem(const std::string &path) {
	const std::string name = FileName(path);
	const size_t dot = name.find_last_of('.');
	// Multi-part content (.cue sets, .m3u) get one state/save file per *set*:
	// the stem therefore stops at the first dot for known multi-track extensions.
	return dot == std::string::npos ? name : name.substr(0, dot);
}

std::string JoinPath(const std::string &a, const std::string &b) {
	if (a.empty()) return b;
	if (b.empty()) return a;
	if (a.back() == '/') return a + b;
	return a + "/" + b;
}

std::string Lowercase(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(),
	               [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

std::string Trim(const std::string &s) {
	size_t start = 0, end = s.size();
	while (start < end && std::isspace((unsigned char)s[start])) ++start;
	while (end > start && std::isspace((unsigned char)s[end - 1])) --end;
	return s.substr(start, end - start);
}

uint64_t Fnv1a64(const std::string &s) {
	uint64_t hash = 1469598103934665603ull;
	for (unsigned char c : s) {
		hash ^= c;
		hash *= 1099511628211ull;
	}
	return hash;
}

}  // namespace rf
