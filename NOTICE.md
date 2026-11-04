# NOTICE — licensing, attribution and policy

RetroFusion is a derivative work of **PPSSPP v1.18.1** (https://github.com/hrydgard/ppsspp),
licensed under the **GNU GPL v2.0 or later**. The whole application is therefore
distributed under GPL-2.0-or-later, and the complete corresponding source —
including all five patches in `native/patches/`, the new sources in `native/`,
and the pinned core revisions in `cores/manifest.json` — is published alongside
every binary. No binary is distributed without the source that produced it.

## Emulated cores

Every core is an unmodified or minimally-patched upstream libretro core, used
under its own license (all GPL-compatible; the pinned commits are listed in
`cores/manifest.json`, and `tools/verify_cores.py` records the exact date and SHA
each pin was taken from). Cores are loaded dynamically as separate shared
objects; they are not statically linked into the PSP core, and none of them is
linked against PPSSPP code.

Special mentions:

* **melonDS** — GPLv3. GPLv3 and GPLv2-or-later are compatible for this use
  (the project as a whole is distributed as GPLv3-or-later where melonDS is
  included in the same APK; the PSP core remains GPLv2-or-later as upstream).
* **FinalBurn Neo** — its own license, non-commercial. Included as a dynamically
  loaded, separately-built module; no ads, no monetisation, no distribution
  through a paid store channel without a separate license review.
* **ScummVM** — GPLv3+, built with a curated engine subset (see
  `scripts/build-core.sh`).

## Firmware / BIOS

**No BIOS or firmware file is bundled with this app.** They remain the property
of their manufacturers and shipping them would make the APK illegal to
distribute. The app instead:

* exposes a documented system directory (`<data>/RetroFusion/cores/system/`),
* names the exact missing file per system when a core needs one
  (e.g. `bios7.bin`, `scph5501.bin`, `syscard3.pce`, `dc_boot.bin`,
  `sega_101.bin`, `panafz10.bin`),
* ships no mechanism to download them.

## Privacy policy (technically enforced, not just promised)

* No advertising SDK, no analytics SDK, no crash-reporting SDK, no attribution
  library.
* The app makes **no network requests** on its own. The only networking code that
  exists is PPSSPP's own optional features (e.g. built-in updater check,
  RetroAchievements), all of which are off by default and remain off.
* No telemetry leaves the device; the crash-loop quarantine database
  (`cores/health/*.crashes`) is a local text file the user can delete.
* Verified mechanically: the CI step installs the release APK's dependency list
  from `gradle dependencies` and fails if a known ad/analytics/telemetry
  coordinate appears. (See the workflow; kept alongside `package-apk.sh` so the
  policy cannot rot.)

## Trademarks

"RetroFusion" and its icon are original to this project. No PPSSPP branding,
name, icon or trade dress is used. System names (Nintendo, Sega, Sony, NEC, SNK,
Amiga, 3DO, MSX…) are used only to describe compatibility, in the standard
nominative sense, and remain the trademarks of their owners.
