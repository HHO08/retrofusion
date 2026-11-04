// RetroFusion - tiny platform queries used by the glue layer.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

// True on the 64-bit build. Drives the ABI-aware core choice (melonDS' JIT is
// 64-bit only, so armeabi-v7a is routed to DeSmuME instead).
bool RFIsArm64();

// Directory containing the core .so files. On Android this is the app's
// nativeLibraryDir (the package manager already extracted and, if enabled,
// compressed-then-mapped them), obtained through JNI at startup and cached.
// On desktop/test builds it falls back to the current working directory.
const std::string &RFNativeLibraryDir();
void RFSetNativeLibraryDir(const std::string &dir);
