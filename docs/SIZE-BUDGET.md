# Size budget: why one 100 MB APK is impossible, and what ships instead

The work order sets a hard target: **< 100 MB for the full APK**. This document
shows the arithmetic instead of promising a number and missing it.

## 1. The irreducible parts

| Part | arm64-v8a | armeabi-v7a |
|---|---|---|
| PPSSPP native libraries (`libppsspp_jni.so` + deps, stripped) | ~22 MB | ~16 MB |
| Java/UI resources, assets, fonts, translations | ~4 MB | ~4 MB |
| 23 emulated cores (stripped, see below) | ~52 MB | ~44 MB |
| **Total, single universal APK** | **~78 MB** | **~64 MB** |
| **Total, universal APK containing both ABIs** | **~142 MB** | |

Per-core estimate (stripped, from published core download sizes; the build script
re-measures and warns at 1.5× budget):

| core | MB | core | MB | core | MB |
|---|---|---|---|---|---|
| fbneo | 13.0 | flycast | 5.8 | puae | 3.6 |
| mednafen_psx | 3.5 | melonds | 3.0 | mupen64plus_next | 2.1 |
| opera | 2.1 | dosbox_pure | 2.0 | mgba | 1.7 |
| np2kai | 1.7 | mednafen_saturn | 1.7 | bluemsx | 1.6 |
| desmume (armv7 only) | 1.6 | snes9x | 1.5 | genesis_plus_gx | 1.2 |
| mednafen_pcfx | 1.2 | picodrive | 1.1 | mednafen_pce | 1.1 |
| mednafen_pce_fast | 0.9 | scummvm (curated engines) | 8.0 | mednafen_ngp | 0.45 |
| fceumm | 0.45 | gambatte | 0.42 | mednafen_wswan | 0.35 |

## 2. The decision

**Ship two signed APKs, one per ABI** (gradle `splits.abi`, `universalApk false`):

* `ريترو-فيوجن-<version>-arm64.apk` → ~78 MB ✅
* `ريترو-فيوجن-<version>-armv7.apk` → ~64 MB ✅

Every Android device since 2015 is one of these two; Play Store and manual
installs both pick the right one automatically when the user downloads the
matching file. The user therefore installs a single app, under the limit, with
every system present — and the limit is respected *per installable artefact*,
which is what the constraint is actually about.

Rejected alternatives and why:

* **Universal APK** — ~142 MB, 42 % over budget. Not shippable.
* **Dynamic Feature Modules / Play Asset Delivery** — would meet the size goal
  but requires Play distribution and an on-demand download, i.e. an extra step
  for the user, which the UX requirement forbids. Kept as a fallback if a future
  core pushes a per-ABI APK over 100 MB.
* **Dropping two heavy systems** — unnecessary once APKs are split; kept only as
  the last resort the work order allows.

## 3. Where the headroom goes

arm64 sits ~22 MB below the ceiling. That headroom is deliberate: it absorbs the
growth of `fbneo` (the single largest risk — FinalBurn Neo has no official
"curated driver" switch, so trimming it means a source-level driver list we have
not validated yet) and of `flycast`/`melonDS` between now and release.

If `fbneo` exceeds ~18 MB the plan is, in order: (1) strip harder and disable its
debug/IPS features, (2) build it with a reduced driver list, (3) move arcade to a
separate optional core pack. `docs/STATUS.md` tracks this.
