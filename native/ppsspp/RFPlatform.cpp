// RetroFusion - platform queries for the glue layer.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "UI/RetroFusion/RFPlatform.h"

#include <string>

namespace {
std::string g_native_lib_dir = ".";
}

bool RFIsArm64() {
#if defined(__aarch64__)
	return true;
#elif defined(__arm__)
	return false;
#else
	return true;  // desktop/emulator builds default to the 64-bit core set
#endif
}

const std::string &RFNativeLibraryDir() {
	return g_native_lib_dir;
}

void RFSetNativeLibraryDir(const std::string &dir) {
	if (!dir.empty()) g_native_lib_dir = dir;
}
