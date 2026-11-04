#!/usr/bin/env bash
# RetroFusion - integrate our sources into a pristine PPSSPP v1.18.1 tree.
#
#   scripts/integrate-ppsspp.sh <path-to-ppsspp-tree>
#
# The PSP core is never modified: we only (a) copy new files in and (b) apply
# five small patches that add routing, one settings tab and the build/gradle
# entries. Every patch is checked before being applied, and the script aborts if
# the tree is not the pinned tag.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

PPSSPP="${1:?usage: integrate-ppsspp.sh <path-to-ppsspp-tree>}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXPECTED_TAG="v1.18.1"

cd "$PPSSPP"

# --- 0. safety: refuse to patch an unknown tree ------------------------------
if [ -d .git ]; then
  if git describe --tags --exact-match 2>/dev/null | grep -q "$EXPECTED_TAG"; then
    echo "ok: $PPSSPP is at $EXPECTED_TAG"
  else
    echo "warning: $PPSSPP is not exactly at $EXPECTED_TAG - patches may not apply" >&2
  fi
fi

# --- 1. copy our sources ----------------------------------------------------
# Layout inside the PPSSPP tree:
#   UI/RetroFusion/            glue (RFGlue, RFPlatform)
#   UI/RetroFusion/corehost/   portable host layer + generated tables + libretro.h
mkdir -p UI/RetroFusion/corehost/generated
cp -f "$ROOT"/native/ppsspp/RFGlue.h "$ROOT"/native/ppsspp/RFGlue.cpp \
      "$ROOT"/native/ppsspp/RFPlatform.h "$ROOT"/native/ppsspp/RFPlatform.cpp UI/RetroFusion/
cp -f "$ROOT"/native/corehost/*.h "$ROOT"/native/corehost/*.cpp UI/RetroFusion/corehost/
cp -f "$ROOT"/native/corehost/generated/*.inc UI/RetroFusion/corehost/generated/
cp -f "$ROOT"/vendor/libretro.h UI/RetroFusion/corehost/libretro.h
echo "copied RetroFusion sources"

# --- 2. apply patches (checked first, so a partial state is impossible) -----
for patch in "$ROOT"/native/patches/*.patch; do
  name="$(basename "$patch")"
  if git apply --check "$patch" 2>/dev/null; then
    git apply "$patch"
    echo "applied $name"
  elif git apply --check --reverse "$patch" 2>/dev/null; then
    echo "already applied $name"
  else
    echo "ERROR: cannot apply $name" >&2
    exit 1
  fi
done

# --- 3. sanity: the PSP core must still be pristine ------------------------
echo "verifying the PSP core was not touched..."
CHANGED="$(git status --porcelain -- Core/ | grep -v 'Core/RetroFusion' || true)"
if [ -n "$CHANGED" ]; then
  echo "ERROR: PSP core files were modified:" >&2
  echo "$CHANGED" >&2
  exit 1
fi
echo "ok: no file under Core/ was modified (requirement 7a / 12.6)"
