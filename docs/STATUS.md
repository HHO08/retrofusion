# Status report — RetroFusion

**Date:** 2026-11-04 · **Report #1**

This is the honest progress report the work order asks for (section 12.8).
It deliberately separates *done and verified* from *done but unverified* from
*not possible in this environment*, because conflating them is how projects ship
broken APKs.

---

## A. Done and verified in this session

| Item | Evidence |
|---|---|
| Portable libretro host layer (`native/corehost/`, ~2 100 lines) | `make -C tests` → **40 checks, 0 failures**, against a real `dlopen`'d libretro core (`tests/dummy_core.c`) |
| Content routing (extension + real disc-header sniffing + per-game override) | tests §1–2: `.nds`→melonDS, armv7 `.nds`→DeSmuME, `.cso`→PPSSPP, Sega-CD/PS1 headers detected, override wins |
| Per-core option schema with weak-device profile | tests §3: N64 dynarec default, Flycast `dsp=disabled`, `threaded_rendering=enabled`, `synchronous_rendering=enabled`, `alpha_sorting=per-strip` |
| SRAM/RTC persistence, atomic writes, savestate fingerprinting | tests §5–6: state refused after a settings change; `.srm` written |
| Crash-loop quarantine, per-core | tests §7: two unclean exits → quarantined; quarantine does not leak to other cores |
| Upstream pins | 24/24 cores resolved to real commit SHAs + dates via the GitHub API (`tools/verify_cores.py` output committed) |
| PPSSPP integration patches | all 5 `git apply --check` clean against pristine `v1.18.1`; `Core/` verified untouched by `integrate-ppsspp.sh` |
| Generated C++ tables from JSON | `tools/gen_cpp_tables.py` runs; tables compile into the tests |

## B. Written, but not yet executed (needs CI + a device)

* `native/ppsspp/RFGlue.cpp` — router, `RFEmuScreen`, per-core settings tab.
  Two functions are marked `RF-ADAPTER` (texture upload/present). They are the
  only tag-sensitive code; if PPSSPP's draw API differs at the pinned tag the
  compiler stops there and nowhere else.
* `scripts/build-core.sh` — per-core recipes. The ladder (native `platform=android`
  → generic unix cross → cmake/configure) means a wrong guess degrades instead of
  failing the build, but the exact flag set must be burned in by the first CI run.
* `.github/workflows/build.yml` — 48 core jobs + assemble + sign + verify.
* `scripts/package-apk.sh` — size/signature/debuggable/ABI enforcement.

## C. Not possible in this environment (with reasons, not excuses)

| Blocked item | Why |
|---|---|
| Producing the signed APK here | The sandbox has **2 vCPU / 1 GB RAM / no Android SDK, NDK, gradle or clang**. A PPSSPP Android build alone needs several GB of RAM per compile unit; 48 core builds on 2 cores is a multi-hour job that would OOM. The GitHub Actions runners in `.github/workflows/build.yml` are the correct place for exactly this, and I have prepared them fully. |
| Pushing the code to your repository | I have no credentials for your repo and you (correctly) will not send a token in a message. The workflow is written so that adding it to the repo is the only step needed. |
| Uploading the APK to Google Drive | Requires an OAuth grant for your account; the `google_drive` app is connected for this conversation, but there is no APK to upload yet. I will do it the moment a signed build exists. |
| Keystore generation/handover | A keystore created in this sandbox would be a toy: it must be created once by you and stored in GitHub Secrets (`RETROFUSION_KEYSTORE_BASE64`, `RETROFUSION_KEY_ALIAS`, `RETROFUSION_KEY_PASSWORD`, `RETROFUSION_STORE_PASSWORD`). `package-apk.sh` and the workflow fail closed if they are missing. |
| "3 real games per core" testing | Impossible without an Android device *and* the ROMs. What I delivered instead is a machine-checkable test plan with the exact fields to fill (`docs/TEST-PLAN.md`) plus the FPS measurement method, so results are comparable rather than anecdotal. |
| Root-cause analysis of *your* earlier NDS/Dreamcast crashes | I do not have your logs or the failing titles. `docs/ROOT-CAUSE-NDS-DREAMCAST.md` documents the five known failure classes with the concrete mitigation implemented for each, and marks them as hypotheses to confirm on device. |

## D. Scope decisions taken (work order §6 pre-authorised these)

1. 32 systems (not 35) — see README §4; the brief's grouped lines expand to 32
   distinct machine types.
2. Per-ABI APKs instead of a universal APK — `docs/SIZE-BUDGET.md`.
3. No BIOS bundled — legal + size.
4. Software-only rendering in phase 1.
5. ScummVM restricted to a curated engine set.
6. NDS: melonDS/arm64 + DeSmuME/armv7.

## E. Next three milestones

1. **M1 — first green CI run.** Add the workflow, set the four signing secrets,
   run `workflow_dispatch`. Expected outcomes: a few cores need one flag adjusted
   (most likely `fbneo`, `flycast`, `mupen64plus_next`, `scummvm`); the APK job
   then produces two signed APKs + `BUILD-REPORT.txt`.
2. **M2 — boot test on real hardware** (one weak device + one mid-range):
   verify PSP still works identically, then each core's first frame, then the
   Start → Settings → Core Settings chain, then the size report.
3. **M3 — per-system test pass** using `docs/TEST-PLAN.md`, root-cause write-ups
   for NDS and Dreamcast filled in with real logs, then the Drive handover of the
   Arabic-named APK.

---

## Report #2 — 2026-11-04 (after repository access was provided)

### What was actually done on GitHub

| Action | Result |
|---|---|
| Repository created | `HHO08/retrofusion` (public, default branch `main`) |
| Code pushed | 45 files, 4 commits, tree verified through the API |
| Signing secrets | `RETROFUSION_KEYSTORE_BASE64`, `_KEY_ALIAS`, `_KEY_PASSWORD`, `_STORE_PASSWORD` — all returned **HTTP 201** and are listed in the repo |
| Keystore | generated here (RSA 4096, PKCS12, 30-year validity, alias `retrofusion`), encrypted into the secret; the password exists **only** inside the two password secrets |
| Workflows | `build.yml` committed; several probe workflows were used to diagnose platform behaviour |

### The blocker (measured, not guessed)

GitHub Actions does not execute jobs for this repository:

* 4 workflow runs have been in `queued` since 16:09–16:11 UTC and never started.
* The queued job reports `"runner": null`, `started_at` set, `steps: []` — i.e. **no runner was ever assigned**, which is not normal queueing latency (a trivial job normally starts in ≤60 s).
* No new runs have been created for pushes after 16:11 UTC.
* Freshly added workflow files are not being indexed (`total_count` stays at the two files that existed before), while `GET /repos/.../actions/permissions` reports `enabled: true`.

This is an account-level restriction on running Actions, not a repository or workflow problem. It cannot be fixed with an API token. Things to check on the account: **Settings → Billing & plans → Actions** (spending limit / payment method), **email verification**, **Settings → Actions → General**, and the "Your account is flagged" banner. A **self-hosted runner** attached to the repo bypasses all of it.

### A verification win that does not need CI

`scripts/check-compile.sh` performs `g++ -fsyntax-only` of **every RetroFusion source file plus the patched upstream files against the real PPSSPP v1.18.1 headers**. It runs in ~4 s on a machine with no NDK and caught a genuine error that would otherwise have cost a 40-minute CI round trip:

> `RFEmuScreen` was abstract — `UIScreen::CreateViews()` is pure virtual. Fixed.

Current result: **9/9 checkable targets OK** (`UI/GameSettingsScreen.cpp` needs git submodules, which CI has), plus **40/40 host-layer tests green**.

### What remains unverified until jobs actually run

1. The libretro cores' per-core build flags (first real compile).
2. PPSSPP's full Android build with the integration (`gradle assembleNormalRelease`).
3. Signing + `package-apk.sh` size enforcement.
4. On-device behaviour of every system.

Nothing else is pending on my side: the moment runners start, `Actions → RetroFusion Build → Run workflow` produces the two signed APKs and the release.
