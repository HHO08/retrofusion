# Architecture

```
                     ┌──────────────────────────────────────────┐
  content file  ───► │ PPSSPP MainScreen / GameScreen / boot     │  (stock code, 1-line change)
                     │  RFCreateEmulatorScreen(path)             │
                     └───────────────┬──────────────────────────┘
                        PSP?         │          other systems
                     ┌───────────────┴───────────────┐
                     ▼                               ▼
             stock EmuScreen                 RFEmuScreen (new)
             (PSP core, untouched)                   │
                                                     ▼
                                     ┌───────────────────────────────┐
                                     │ native/corehost (portable)    │
                                     │  CoreHost: dlopen + libretro  │
                                     │  frontend API, SRAM, states,  │
                                     │  options, quarantine          │
                                     └───────┬───────────┬───────────┘
                                             │           │
                                   sinks ────┘           └──── core .so (lib/<abi>/<core>.so)
                                video → PPSSPP texture
                                audio → System_AudioPushSamples
                                input ← PPSSPP ControlMapper
```

## Layers and why they are split this way

**1. `native/corehost/` — portable, dependency-free, unit-tested.**
No PPSSPP headers at all. This is where every decision that can be wrong lives,
so it can be tested on the build machine in five seconds instead of on a phone.
`tests/host_test.cpp` loads a real `dlopen`'d libretro core and checks video,
audio, input, options, savestates, SRAM, quarantine and routing.

**2. `native/ppsspp/` — the only PPSSPP-aware code (~700 lines).**
It translates between PPSSPP concepts (Screen, ControlMapper, DrawContext,
GamePauseScreen) and the portable host's sinks. Marked `RF-ADAPTER` where it
touches PPSSPP APIs that are not guaranteed stable across tags.

**3. Data, not code.**
`cores/manifest.json` (systems → cores → pinned SHAs) and
`cores/core_options.json` (option schema + potato profile) are the source of
truth. `tools/gen_cpp_tables.py` compiles them into C++ tables, so the settings
UI, the runtime option delivery and the packaging step can never disagree.
Unknown option keys are inert (the core only ever asks for keys it knows), which
makes a stale key a no-op rather than a crash.

## Threading model

* One core runs at a time, on PPSSPP's emulation thread; `CoreHost::s_active` is
  a single guarded instance (libretro's environment callbacks are process-global
  C callbacks — this is enforced, not assumed).
* The video sink copies the frame inside the callback; cores that thread their
  renderer may therefore reuse the buffer immediately.
* SRAM is flushed every ~5 s of emulated time, on pause, and on unload; writes
  go through `WriteWholeFile`, which writes a temp file and renames, so a crash
  can never leave a half-written save.

## Content routing

1. Per-game user override (asked at most once, then remembered).
2. Unambiguous extension (`gba`, `nds`, `n64`, …).
3. Ambiguous container (`bin iso cue chd zip dsk rom`) → real boot-sector
   sniffing: `SEGADISCSYSTEM`, `SEGA 32X`, `SEGA SEGASATURN`, `SEGA SEGAKATANA`,
   `PC Engine CD-ROM SYSTEM`, `PC-FX:Hu_CD-ROM`, the 3DO volume header, the
   PlayStation licence string, the Genesis `SEGA` header at 0x100.
4. Ambiguity that survives sniffing: manifest order picks a default and the
   caller is told it was a guess.
5. PSP extensions (`iso cso pbp elf prx`) never enter this path.

## Isolation (requirement 7c)

* `RTLD_LOCAL` per core: no cross-core symbol binding.
* Crash-loop quarantine: marker file before entering a core, removed on clean
  exit; two leftover markers disable that core only, with a "Start anyway"
  escape hatch. Per-core counters verified by tests.
* CI fails the build if anything under `Core/` changes, so the PSP core cannot
  regress even by accident.
