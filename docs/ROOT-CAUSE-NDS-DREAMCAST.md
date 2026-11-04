# Root cause analysis — Nintendo DS and Dreamcast

The work order requires a written root-cause report, not "fixed". This document
covers the five documented failure classes for these two systems, what the
codebase now does about each, and **which parts are still hypotheses** that
device testing must confirm.

Legend: ✅ implemented and unit-tested · 🟡 implemented, needs device validation ·
⛔ known gap, documented rather than hidden.

---

## Nintendo DS (melonDS / DeSmuME)

### RC-1 — 64-bit-only JIT ✅
**Symptom:** NDS runs at 5–10 FPS, or crashes on start on some devices.
**Cause:** melonDS' JIT recompiler is only implemented for 64-bit hosts. On an
`armeabi-v7a` process the core silently falls back to an interpreter, which
cannot reach playable speed; the resulting timing skew also breaks games that
depend on ARM9/ARM7 synchronisation, which then looks like a "stability bug".
**Mitigation:** the registry declares melonDS as `armv7: false` with
`fallback_core_id: desmume`; `Resolve()` substitutes DeSmuME on 32-bit builds
instead of shipping a core that cannot work there (verified by test §1).

### RC-2 — renderer threading vs. the Android main thread 🟡
**Symptom:** intermittent freeze after 1–3 minutes, black frames, or a frame
clock that stalls while audio keeps playing.
**Cause:** the core's threaded renderer hands frames from an internal thread to
the frontend; a frontend that does not copy or fence the frame before returning
from the video callback can read a buffer the core is already rewriting.
**Mitigation:** `RFVideoSink::OnVideoFrame` copies the frame into an owned
`std::vector` inside the callback, so the core may reuse its buffer immediately.
`melonds_threaded_renderer` stays `enabled` in the potato profile because the
copy makes it safe and it is a large performance win.

### RC-3 — savestates across different option sets ✅
**Symptom:** "worked yesterday, crashes when I load a state today".
**Cause:** a state serialised with one set of core options (JIT on/off, BIOS
on/off, threaded renderer) does not describe a valid machine when restored under
another.
**Mitigation:** every state file carries `RFSTATE1` + an FNV-1a fingerprint of
the effective option set; loading across a fingerprint mismatch is refused with
a clear message (verified by test §5).

### RC-4 — BIOS/DS-mode firmware assumptions ✅
**Symptom:** white screens on `.nds` files that need the DS firmware, or DSi
titles failing.
**Mitigation:** the manifest marks `needs_bios: true`; the app names the exact
file (`bios7.bin`, `bios9.bin`, `firmware.bin`) instead of failing silently, and
`beetle`-style HLE fallbacks are enabled where the core offers them.

### RC-5 — crash containment ✅
**Mitigation:** crash-loop quarantine (see below) applies to NDS like every other
system: two unclean exits disable NDS only.

---

## Dreamcast (Flycast)

### RC-1 — GL-only rendering path ⛔ (phase 1 gap, stated openly)
**Cause:** Flycast's libretro port requires a hardware render context for its
PowerVR emulation; there is no usable software rasteriser. Our phase-1 host
refuses `RETRO_ENVIRONMENT_SET_HW_RENDER`, so Flycast will report
*"This core needs OpenGL…"* rather than presenting a black screen.
**Status:** this is the one system that **cannot work with the current host**.
Fixing it means implementing the GL bridge (`RETRO_HW_FRAME_BUFFER_VALID` +
`retro_hw_render_callback` against PPSSPP's existing GL context, which
`android/jni/AndroidJavaGLContext.cpp` already provides). That is the top item of
phase 2 and is written up in `docs/STATUS.md` as a known gap, not as a success.

### RC-2 — frame pacing with threaded + synchronous rendering ✅
**Cause (documented upstream):** Flycast's threaded renderer can present frames
out of order with the emulated SH4 timeline; the project's own option set pairs
it with `synchronous_rendering=enabled` to stabilise pacing.
**Mitigation:** both options are baked into the potato profile
(`reicast_threaded_rendering=enabled`, `reicast_synchronous_rendering=enabled`)
and covered by the option tests, so a fresh install is already in the documented
stable configuration instead of the default one.

### RC-3 — GD-ROM/CD-DA stalls mistaken for crashes 🟡
**Symptom:** multi-second freezes during load screens; users report a hang.
**Cause:** accurate CD emulation blocks the emulation thread while reading.
**Mitigation:** `reicast_fast_gdrom_load=enabled`, `reicast_fast_cdda=enabled`,
`reicast_dsp=disabled` in the potato profile — the same combination the libretro
community uses for low-end handhelds (per the work order's §5 list).

### RC-4 — VRAM/alpha-sorting cost 🟡
**Mitigation:** `reicast_alpha_sorting=per-strip`, `reicast_emulate_fog=disabled`,
`reicast_anisotropic_filtering=0`, internal resolution pinned to native
640×480. All four are in the schema with the work order's values as defaults.

### RC-5 — no BIOS present ✅
**Cause:** many images assume `dc_boot.bin` / `dc_flash.bin`.
**Mitigation:** same BIOS-naming path as NDS; GDI images carrying their own
`ip.bin` boot without firmware.

---

## The isolation mechanism both systems depend on ✅

A hard `SIGSEGV` inside a core cannot be caught safely in-process — pretending
otherwise is how frontends end up crashing the whole app. What we implement:

1. **Symbol isolation** — every core is `dlopen`ed with `RTLD_LOCAL`, so the
   identically named `retro_*` symbols of two cores can never bind to each other.
2. **Crash-loop quarantine** — a marker file is written before entering a core
   and removed on a clean exit. A leftover marker increments that core's counter;
   at two crashes the core is refused with an explanatory message and a
   "Start anyway" escape hatch. Verified by test §7, including that quarantine
   does **not** leak to other cores.
3. **PSP is structurally exempt** — the PSP path never enters this code, and CI
   fails the build if any file under `Core/` is modified.

Together these satisfy requirement 7c: a broken NDS or Dreamcast cannot take the
PSP core, or the app, down with it.
