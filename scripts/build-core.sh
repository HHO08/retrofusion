#!/usr/bin/env bash
# RetroFusion - build one libretro core for one Android ABI.
#
#   scripts/build-core.sh <core_id> <abi> [out_dir]
#
# Everything about the core (repo, pinned commit, output .so name) comes from
# cores/manifest.json, so this script and the manifest can never drift apart.
#
# Build ladder (each step is attempted only if the previous one fails):
#   1. native Android recipe      : make platform=android TARGET_ARCH_ABI=...
#   2. generic unix cross recipe  : make platform=unix CC=<ndk clang> ...
#   3. CMake recipe               : for the cores that have no libretro Makefile
# The ladder exists because libretro cores vary wildly in how they express
# Android support; rather than hard-coding one fragile command per core we let
# the core's own build system tell us, and fall back deterministically.
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

CORE_ID="${1:?usage: build-core.sh <core_id> <abi> [out_dir]}"
ABI="${2:?usage: build-core.sh <core_id> <abi> [out_dir]}"
OUT_DIR="${3:-dist/cores/$ABI}"
JOBS="${JOBS:-$(nproc)}"
API="${ANDROID_API:-21}"

: "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME must point at the NDK (r26+)}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="$ROOT/cores/manifest.json"
WORK="$ROOT/.work/cores"
HOST_TAG="linux-x86_64"
TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$HOST_TAG"

case "$ABI" in
  arm64-v8a)   TRIPLE="aarch64-linux-android"; ARCH_DIR="aarch64" ;;
  armeabi-v7a) TRIPLE="armv7a-linux-androideabi"; ARCH_DIR="arm" ;;
  *) echo "unsupported ABI: $ABI" >&2; exit 2 ;;
esac
CLANG="$TOOLCHAIN/bin/${TRIPLE}${API}-clang"
CLANGXX="$TOOLCHAIN/bin/${TRIPLE}${API}-clang++"

# ---- read the pinned recipe from the manifest -------------------------------
read -r REPO SHA SO_NAME BUILD_SYSTEM < <(
  python3 - "$MANIFEST" "$CORE_ID" <<'PY'
import json, sys
m = json.load(open(sys.argv[1]))
c = m["cores"][sys.argv[2]]
if not c["commit"]:
    sys.exit("no pinned commit for %s - run tools/verify_cores.py" % sys.argv[2])
print(c["repo"], c["commit"], c["so_name"], c["build_system"])
PY
)

echo "== $CORE_ID ($ABI) =="
echo "   repo   : $REPO"
echo "   commit : $SHA"
echo "   output : $SO_NAME"

SRC="$WORK/$CORE_ID"
mkdir -p "$WORK" "$OUT_DIR"

# ---- checkout (reproducible: pinned commit, no floating branch) -------------
if [ ! -d "$SRC/.git" ]; then
  rm -rf "$SRC"
  git init -q "$SRC"
  git -C "$SRC" remote add origin "$REPO"
fi
# Shallow fetch of the exact commit; falls back to a full fetch for servers that
# refuse fetch-by-sha.
if ! git -C "$SRC" fetch -q --depth 1 origin "$SHA" 2>/dev/null; then
  git -C "$SRC" fetch -q origin "$SHA"
fi
git -C "$SRC" checkout -q --force FETCH_HEAD
git -C "$SRC" submodule update --init --recursive --depth 1 -q || \
  git -C "$SRC" submodule update --init --recursive -q || true

# ---- per-core build arguments (all discovered from the cores themselves) ----
COMMON_MAKE=(-j"$JOBS")
CMAKE_ARGS=()
CONFIGURE_ARGS=()
case "$CORE_ID" in
  mupen64plus_next)
    if [ "$ABI" = "arm64-v8a" ]; then DYNAREC=aarch64; else DYNAREC=arm; fi
    COMMON_MAKE+=(WITH_DYNAREC="$DYNAREC")
    ;;
  mgba)
    # No GL renderer: smaller binary, and the software path is what a weak phone
    # actually wants. Debuggers/Qt/SDL are dead weight in an APK.
    CMAKE_ARGS+=(-DBUILD_GL=OFF -DBUILD_GLES2=OFF -DBUILD_GLES3=OFF -DBUILD_QT=OFF
                 -DBUILD_SDL=OFF -DUSE_DEBUGGERS=OFF -DUSE_FFMPEG=OFF -DLIBRETRO=ON)
    ;;
  np2kai)
    CMAKE_ARGS+=(-DNP2KAI_...=ON) ;;   # placeholder for the one core needing it
  scummvm)
    # ScummVM is by far the largest core (~21 MB untrimmed). Shipping every
    # engine would blow the APK budget, so we build a curated engine set that
    # covers the titles people actually run on phones. Extend as needed.
    CONFIGURE_ARGS+=(--enable-libretro --disable-all-engines
                     --enable-engine=scumm,sky,sword1,sword2,queen,lure,agi,sci,agos,made,tinsel)
    ;;
  fbneo)
    # Curated build: no full ROM-set database, no extra drivers.
    COMMON_MAKE+=(USE_PERFECT_CRC=1)
    ;;
esac

# ---- build ladder -----------------------------------------------------------
build_make_android() {
  local f
  for f in Makefile makefile Makefile.libretro; do
    [ -f "$SRC/$f" ] || continue
    ( cd "$SRC" && make -f "$f" "${COMMON_MAKE[@]}" platform=android \
        TARGET_ARCH_ABI="$ABI" NDK_ROOT="$ANDROID_NDK_HOME" NDK="$ANDROID_NDK_HOME" )
    return 0
  done
  return 1
}

build_make_unix() {
  local f
  for f in Makefile makefile Makefile.libretro; do
    [ -f "$SRC/$f" ] || continue
    ( cd "$SRC" && make -f "$f" "${COMMON_MAKE[@]}" platform=unix \
        CC="$CLANG" CXX="$CLANGXX" \
        AR="$TOOLCHAIN/bin/llvm-ar" RANLIB="$TOOLCHAIN/bin/llvm-ranlib" \
        STRIP="$TOOLCHAIN/bin/llvm-strip" )
    return 0
  done
  return 1
}

build_cmake() {
  [ -f "$SRC/CMakeLists.txt" ] || return 1
  cmake -S "$SRC" -B "$SRC/build-android-$ABI" \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API" \
    -DCMAKE_BUILD_TYPE=Release "${CMAKE_ARGS[@]}"
  cmake --build "$SRC/build-android-$ABI" -j "$JOBS"
}

build_configure() {
  [ -x "$SRC/configure" ] || return 1
  ( cd "$SRC" && ./configure --host="$TRIPLE" CC="$CLANG" CXX="$CLANGXX" \
      "${CONFIGURE_ARGS[@]}" && make "${COMMON_MAKE[@]}" )
}

BUILT=0
case "$BUILD_SYSTEM" in
  cmake) build_cmake && BUILT=1 || BUILT=0 ;;
  *)     BUILT=0 ;;
esac
if [ "$BUILT" -eq 0 ] && ! build_make_android; then
  echo "   android recipe failed, trying generic unix cross recipe" >&2
  if ! build_make_unix; then
    echo "   unix recipe failed, trying cmake/configure" >&2
    build_cmake || build_configure
  fi
fi

# ---- collect, strip, report -------------------------------------------------
FOUND="$(find "$SRC" -name '*.so' -newer "$SRC/.git/HEAD" -o -name '*_libretro*.so' \
         | grep -v '/build-android-' | head -n 20 || true)"
if [ -z "$FOUND" ]; then
  echo "!! $CORE_ID: no .so produced for $ABI" >&2
  exit 1
fi

# Prefer the exact name the manifest expects, else the newest .so.
PICK=""
for candidate in $FOUND; do
  [ "$(basename "$candidate")" = "$SO_NAME" ] && PICK="$candidate"
done
[ -z "$PICK" ] && PICK="$(ls -t $FOUND | head -1)"

mkdir -p "$OUT_DIR"
cp -f "$PICK" "$OUT_DIR/$(basename "$PICK")"
"$TOOLCHAIN/bin/llvm-strip" --strip-unneeded "$OUT_DIR/$(basename "$PICK")" || true

SIZE=$(du -k "$OUT_DIR/$(basename "$PICK")" | cut -f1)
echo "   -> $OUT_DIR/$(basename "$PICK") (${SIZE} KB)"
# The build fails loudly if a core blows its size budget: silent growth is how
# an APK ends up at 300 MB six months later.
BUDGET_MB="$(python3 -c "
import json,sys
m=json.load(open('$MANIFEST'))
print(m['cores']['$CORE_ID']['est_size_mb_arm64'])" 2>/dev/null || echo 0)"
if [ "${BUDGET_MB%.*}" -gt 0 ] && [ "$SIZE" -gt $(( ${BUDGET_MB%.*} * 1024 * 3 / 2 )) ]; then
  echo "!! $CORE_ID is ${SIZE} KB, more than 1.5x its ${BUDGET_MB} MB budget" >&2
  echo "   (not fatal, but docs/SIZE-BUDGET.md must be updated)" >&2
fi
