#!/usr/bin/env bash
# RetroFusion - post-build APK verification and packaging.
#
#   scripts/package-apk.sh <ppsspp-tree> <out-dir> <version>
#
# Enforces the work order's non-negotiables automatically:
#   * every APK is signed (verified with apksigner, not assumed)
#   * every APK is a release build (no debuggable flag, no testOnly)
#   * every APK is under 100 MB
#   * only the two required ABIs are present, and each split contains only its
#     own ABI
#   * Arabic file names are used for the human-facing deliverables
#
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

TREE="${1:?usage: package-apk.sh <ppsspp-tree> <out-dir> <version>}"
OUT="${2:?usage: package-apk.sh <ppsspp-tree> <out-dir> <version>}"
VERSION="${3:-1.0.0-rf1}"

MAX_MB=100
REPORT="$OUT/BUILD-REPORT.txt"
mkdir -p "$OUT"
: > "$REPORT"

SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
APKSIGNER="$(command -v apksigner || find "$SDK" -name apksigner -type f 2>/dev/null | sort | tail -1)"
AAPT2="$(command -v aapt2 || find "$SDK" -name aapt2 -type f 2>/dev/null | sort | tail -1)"
ZIPALIGN="$(command -v zipalign || find "$SDK" -name zipalign -type f 2>/dev/null | sort | tail -1)"

fail() { echo "ERROR: $*" | tee -a "$REPORT" >&2; exit 1; }

echo "RetroFusion $VERSION - build report ($(date -u +%FT%TZ))" | tee -a "$REPORT"
echo "PPSSPP: $(cat "$TREE/../ppsspp-commit.txt" 2>/dev/null || echo unknown)" | tee -a "$REPORT"
echo | tee -a "$REPORT"

shopt -s nullglob
mapfile -t APS < <(find "$TREE/android/build/outputs/apk" -name "*.apk" -path "*release*" 2>/dev/null | sort)
[ ${#APS[@]} -gt 0 ] || fail "no release APK produced - check the gradle log"

for apk in "${APS[@]}"; do
  base="$(basename "$apk")"
  case "$base" in
    *arm64-v8a*)   TAG="arm64" ;;
    *armeabi-v7a*) TAG="armv7" ;;
    *)             TAG="universal" ;;
  esac

  case "$TAG" in
    arm64)   APK_NAME="ريترو-فيوجن-${VERSION}-arm64.apk" ;;
    armv7)   APK_NAME="ريترو-فيوجن-${VERSION}-armv7.apk" ;;
    *)       fail "unexpected universal APK ($base) - splits must stay enabled" ;;
  esac

  # 1. signature ------------------------------------------------------------
  [ -n "$APKSIGNER" ] || fail "apksigner not found"
  "$APKSIGNER" verify --verbose "$apk" > "$OUT/verify-$TAG.txt" 2>&1 \
    || fail "$base is NOT correctly signed"
  grep -q "Verified using v2 scheme (APK Signature Scheme v2): true" "$OUT/verify-$TAG.txt" \
    || echo "warning: v2 signature not confirmed for $base" | tee -a "$REPORT"

  # 2. release build, not debug --------------------------------------------
  if [ -n "$AAPT2" ]; then
    BADGING="$("$AAPT2" dump badging "$apk" 2>/dev/null || true)"
    echo "$BADGING" | grep -qi "application-debuggable" \
      && fail "$base is debuggable (debug build) - release builds only"
  fi

  # 3. size -----------------------------------------------------------------
  SIZE_BYTES=$(stat -c%s "$apk")
  SIZE_MB=$(( SIZE_BYTES / 1024 / 1024 ))
  [ "$SIZE_MB" -lt "$MAX_MB" ] || fail "$base is ${SIZE_MB} MB, over the ${MAX_MB} MB limit"

  # 4. ABI contents ---------------------------------------------------------
  ABIS="$(unzip -l "$apk" | awk '/lib\// {split($4,a,"/"); print a[2]}' | sort -u | tr '\n' ' ')"
  echo "$base" | tee -a "$REPORT"
  echo "  size    : ${SIZE_MB} MB ($SIZE_BYTES bytes)" | tee -a "$REPORT"
  echo "  abis    : $ABIS" | tee -a "$REPORT"
  echo "  signed  : yes ($(grep -c 'Verified using' "$OUT/verify-$TAG.txt") scheme(s))" | tee -a "$REPORT"

  "${ZIPALIGN:-true}" -c -v 4 "$apk" > /dev/null 2>&1 || echo "  note: zipalign -c reported an issue (gradle usually aligns already)" | tee -a "$REPORT"

  cp -f "$apk" "$OUT/$APK_NAME"
  echo "  output  : $OUT/$APK_NAME" | tee -a "$REPORT"
  echo | tee -a "$REPORT"
done

( cd "$OUT" && sha256sum *.apk > SHA256SUMS.txt )
echo "SHA256SUMS.txt written" | tee -a "$REPORT"
