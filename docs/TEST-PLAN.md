# Test plan — 3 titles per system, on two device classes

Requirement 10 asks for at least three real games per core, with FPS on a
mid-range and a very weak device, plus written reports. This is the
machine-checkable form of that requirement: fill the table, and the results are
comparable instead of anecdotal.

## Devices

| Class | Definition | Suggested reference |
|---|---|---|
| **W** weak ("potato") | 4×A53 ≤1.8 GHz, 2–3 GB RAM, eMMC storage | e.g. Redmi 9A, Galaxy A03 |
| **M** mid-range | 4×A76 + 4×A55, 4–6 GB RAM | e.g. Pixel 6a, Redmi Note 12 |

Measure with: in-game overlay FPS (`Settings → Graphics → Show FPS`), 5 minutes
of typical gameplay after the first loading screen, thermal steady state, no
charger, airplane mode.

## Procedure per title

1. Fresh install → confirm the game appears in the PPSSPP library with its
   icon/thumbnail generated automatically (UX requirement).
2. Launch with one tap → confirm **no** manual core selection dialog appears.
3. Play 5 min → record FPS (avg / min), audio glitches, visual errors.
4. Start → Settings → **Core Settings** tab → change one option → confirm the
   change takes effect and survives an app restart.
5. Save state, load state, then toggle a core option and confirm the state is
   refused with the fingerprint message (not a crash).
6. Pause 10 s → confirm SRAM was written (`cores/save/<system>/<game>.srm`).
7. Force-stop during play → relaunch → confirm the app starts, the core is
   counted as one crash, and every *other* system still launches.

## Results table (fill one per title)

| System | Core | Title | Device | FPS avg | FPS min | Audio | Video | Notes |
|---|---|---|---|---|---|---|---|---|
| NES | fceumm | | W | | | | | |
| NES | fceumm | | M | | | | | |
| FDS | fceumm | | W | | | | | |
| GB | gambatte | | W | | | | | |
| … (one block per system in `cores/manifest.json`) | | | | | | | |

## PSP regression (must pass before any release)

The PSP core is untouched, but the surrounding app changed, so these are the
gate:

| Test | Expected |
|---|---|
| Launch 3 PSP ISOs (one CSO, one PBP, one plain ISO) | identical to stock PPSSPP v1.18.1 |
| PSP savestates made by stock PPSSPP | still load |
| PSP in-game Settings screen | pixel-identical sections; **no** Core Settings tab |
| Touch controls, custom layout, gamepad mapping | unchanged |
| `git status -- Core/` after integration | empty |

## Acceptance

A release is blocked if: any PSP regression fails, any APK is ≥100 MB, any APK
fails `apksigner verify`, any core crashes twice on first launch, or the
Core-Settings tab is missing for a system that has a schema.
