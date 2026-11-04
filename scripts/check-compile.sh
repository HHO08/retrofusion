#!/usr/bin/env bash
# RetroFusion - local syntax/API check against a real PPSSPP tree.
#
#   scripts/check-compile.sh <integrated-ppsspp-tree>
#
# This is NOT a build: it is `g++ -fsyntax-only`, which catches every API
# mismatch in our integration code in seconds on a machine with no NDK - the
# exact class of error that would otherwise cost a 40-minute CI round trip.
# The real Android build remains the authority for codegen/linking.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -uo pipefail

SRC="${1:?usage: check-compile.sh <integrated-ppsspp-tree>}"
cd "$SRC" || exit 2

INC="-I. -ICommon -ICore -IGPU -IUI -Iext -Iext/native -Iext/glslang -Iext/native/ext"
FLAGS="-std=c++17 -fsyntax-only -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable"

TARGETS=(
  "UI/RetroFusion/corehost/path_util.cpp"
  "UI/RetroFusion/corehost/content_sniff.cpp"
  "UI/RetroFusion/corehost/core_registry.cpp"
  "UI/RetroFusion/corehost/core_options.cpp"
  "UI/RetroFusion/corehost/libretro_host.cpp"
  "UI/RetroFusion/RFPlatform.cpp"
  "UI/RetroFusion/RFGlue.cpp"
  # Patched upstream files: they must still compile with our includes.
  "UI/MainScreen.cpp"
  "UI/GameScreen.cpp"
  "UI/GameSettingsScreen.cpp"
)

fail=0
for t in "${TARGETS[@]}"; do
  printf '%-48s ' "$t"
  out=$(g++ $FLAGS $INC "$t" 2>&1)
  # Upstream headers emit their own warnings; only compiler *errors* matter here.
  if ! grep -q "error:" <<< "$out"; then
    echo "OK"
  elif grep -q "fatal error: ext/" <<< "$out"; then
    echo "SKIP (needs git submodules)"
  else
    echo "FAIL"
    grep -A3 "error:" <<< "$out" | head -30
    fail=1
  fi
done
exit $fail
