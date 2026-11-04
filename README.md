# RetroFusion (ريترو-فيوجن)

A multi-system frontend built on a **byte-identical, untouched PPSSPP v1.18.1 PSP
core**, with a libretro core host bolted on so that 32 additional systems run
inside the same application, with the same UI, the same button mapping and the
same Start → Settings menu.

> **Status: engineering scaffold complete and tested at the host layer; the
> signed APK has not been produced yet.** See [docs/STATUS.md](docs/STATUS.md)
> for exactly what is done, what is blocked and why. Nothing in this README
> claims a working APK that does not exist.

---

## 1. What is in this repository

| Path | What it is | Verified? |
|---|---|---|
| `native/corehost/` | Portable libretro **host** layer: dlopen with symbol isolation, full frontend API, SRAM/RTC, savestates, core options, crash-loop quarantine | ✅ 40 automated checks pass on the build machine (`make -C tests`) |
| `native/ppsspp/` | The **only** PPSSPP-aware code: content router, `RFEmuScreen`, per-core settings tab | ⚠️ compiles only inside the PPSSPP tree → verified by CI + device |
| `native/patches/` | 5 unified patches against pristine v1.18.1 (routing ×3 sites, settings tab, build system, gradle ABI splits + branding) | ✅ all five `git apply --check` clean against the pinned tag |
| `cores/manifest.json` | 32 systems → 24 cores, **each pinned to a real commit SHA** taken from the GitHub API on 2026-11-04 | ✅ SHAs fetched, not invented |
| `cores/core_options.json` | Per-core option schema with a *balanced* and a *weak-device (potato)* profile, straight from the work order | ✅ compiled into the option table and exercised by tests |
| `scripts/build-core.sh` | Per-core Android cross-build with a 3-step fallback ladder and a size-budget guard | ⚠️ exercises NDK + core Makefiles → CI |
| `scripts/integrate-ppsspp.sh` | Copies our sources into the PPSSPP tree, applies patches, **refuses to continue if any file under `Core/` changed** | ✅ patch-apply logic tested |
| `scripts/package-apk.sh` | Enforces signed/release/<100 MB/ABI-correct, emits Arabic-named APKs + SHA256 + build report | ⚠️ needs a real APK from CI |
| `.github/workflows/build.yml` | 24 cores × 2 ABIs in a parallel matrix → integrate → gradle → sign → verify → Release | ⚠️ not yet executed |
| `tools/` | Manifest/table generators + upstream verifier | ✅ ran, output committed |

## 2. Quick start

```bash
# host-layer tests (no NDK, no device, ~5 s)
make -C tests

# full Android build (needs NDK r26 + JDK 17 + Android SDK)
scripts/build-core.sh fceumm arm64-v8a          # one core
scripts/integrate-ppsspp.sh ../ppsspp-src       # integrate into PPSSPP
# CI does the rest: .github/workflows/build.yml
```

## 3. The five hard requirements, and where they are satisfied

| Requirement | Where |
|---|---|
| **7a** PSP core untouched | `scripts/integrate-ppsspp.sh` + CI step "Fail if the PSP core was modified" abort the build if anything under `Core/` is dirty. Our code lives entirely in new files. |
| **7b** armeabi-v7a + arm64-v8a | gradle `splits.abi`; every core built for both; melonDS (64-bit-only JIT) falls back to DeSmuME on armv7 by table, not by guesswork. |
| **7c** modular isolation | Per-core `dlopen(..., RTLD_LOCAL)` + crash-loop quarantine (`CoreHost::ReconcileHealthAtStartup`) so a crashing core disables *itself* and nothing else. See `docs/ROOT-CAUSE-NDS-DREAMCAST.md`. |
| **7d** speed > resolution | Hard-wired software rendering, native internal resolutions and the potato profile as the *default* for a fresh install. |
| **8 (UX)** indistinguishable from PPSSPP | Launches go through PPSSPP's own `MainScreen`/`GameScreen`/`NativeApp`; input is read from PPSSPP's `ControlMapper` (so user remaps apply everywhere); the pause menu is stock `GamePauseScreen`; the extra tab is built from stock `ItemHeader`/`CheckBox`/`PopupMultiChoice`. |

## 4. Systems

32 distinct systems / 24 cores (the brief's 27 lines expand to 32 machine types
once grouped lines such as *Game Gear/Genesis/Master System/Sega CD/Sega Pico*
and *PC-88/PC-98* are counted individually). Full table with extensions, BIOS
requirements and per-ABI availability: `cores/manifest.json`.

## 5. Decisions I made as the expert (not deferred to you)

1. **Per-ABI APKs instead of one universal APK.** The raw math (§ `docs/SIZE-BUDGET.md`)
   makes a single <100 MB APK physically impossible: cores alone are ~60 MB per
   ABI *before* PPSSPP's own ~22 MB native libraries and Java/UI resources.
   Two signed APKs, one per ABI, each comfortably under 100 MB, delivered
   together — same install experience, and every device downloads only what it
   runs.
2. **BIOS files are not bundled.** Shipping copyrighted firmware would make the
   APK illegal to distribute; instead the app ships an importer (system dir) and
   a clear message naming the exact file each system needs. It also keeps the
   APK small.
3. **Software rendering only, phase 1.** Forcing the software path is both the
   fastest choice on weak devices and the only one that can be validated without
   a device. Cores that *only* have a GL path fail with an explicit message
   instead of a black screen (see `docs/STATUS.md` for which ones that affects).
4. **ScummVM is built with a curated engine set** — a full ScummVM core is
   ~21 MB per ABI and would alone consume a fifth of the budget.
5. **NDS uses melonDS on arm64 and DeSmuME on armv7**, because melonDS' JIT is
   64-bit only and an interpreter-only melonDS is unplayable on the devices this
   is aimed at.
6. **Name:** *RetroFusion* / `com.retrofusion.app`, with a distinct icon
   (`assets/icon.png`) — nothing references PPSSPP branding.

## 6. License

GPL-2.0-or-later, inherited from PPSSPP and every libretro core. Full
attribution, the no-ads/no-analytics policy and the BIOS position:
[NOTICE.md](NOTICE.md).
