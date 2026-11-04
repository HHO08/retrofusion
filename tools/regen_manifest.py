#!/usr/bin/env python3
"""Build cores/manifest.json from the curated table below + the *verified*
upstream commit SHAs in work/cores_verified.json (see tools/verify_cores.py).

Run from the repo root:  python3 tools/regen_manifest.py
Keeping the SHAs pinned is what makes the Android build reproducible and stops
an upstream force-push from silently changing what we ship.
"""
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
VERIFIED = os.environ.get("RF_VERIFIED", os.path.join(ROOT, "..", "work", "cores_verified.json"))

# core_id -> (repo, so_name, build_system, tier, est_size_mb_arm64, notes)
CORES = {
 "fceumm":              ("libretro/libretro-fceumm",           "fceumm_libretro_android.so",              "make",  "base",  0.45, "lightest reliable NES/FDS core"),
 "gambatte":            ("libretro/gambatte-libretro",         "gambatte_libretro_android.so",            "make",  "base",  0.42, "GB/GBC accuracy + low CPU"),
 "mgba":                ("libretro/mgba",                      "mgba_libretro_android.so",                "cmake", "base",  1.70, "GBA; build without OpenGL renderer"),
 "mupen64plus_next":    ("libretro/mupen64plus-libretro-nx",   "mupen64plus_next_gles3_libretro_android.so","make","heavy", 2.10, "N64, GLES3 dynarec build"),
 "melonds":             ("libretro/melonDS",                   "melonds_libretro_android.so",             "make",  "heavy", 3.00, "NDS; JIT requires 64-bit"),
 "desmume":             ("libretro/desmume",                   "desmume_libretro_android.so",             "make",  "fallback",1.60,"NDS fallback for armeabi-v7a only"),
 "snes9x":              ("libretro/snes9x",                    "snes9x_libretro_android.so",              "make",  "base",  1.50, "SNES (not bsnes)"),
 "picodrive":           ("libretro/picodrive",                 "picodrive_libretro_android.so",           "make",  "base",  1.10, "32X"),
 "flycast":             ("libretro/flycast",                   "flycast_libretro_android.so",             "make",  "heavy", 5.80, "Dreamcast; biggest non-ScummVM core"),
 "genesis_plus_gx":     ("libretro/Genesis-Plus-GX",           "genesis_plus_gx_libretro_android.so",     "make",  "base",  1.20, "covers 5 Sega systems"),
 "mednafen_saturn":     ("libretro/beetle-saturn-libretro",    "mednafen_saturn_libretro_android.so",     "make",  "heavy", 1.70, "Saturn"),
 "opera":               ("libretro/opera-libretro",            "opera_libretro_android.so",               "make",  "heavy", 2.10, "3DO"),
 "puae":                ("libretro/libretro-uae",              "puae_libretro_android.so",                "make",  "heavy", 3.60, "Amiga"),
 "fbneo":               ("libretro/FBNeo",                     "fbneo_libretro_android.so",               "make",  "heavy",13.00, "Arcade + Neo Geo CD; size risk"),
 "bluemsx":             ("libretro/blueMSX-libretro",          "bluemsx_libretro_android.so",             "make",  "base",  1.60, "MSX"),
 "mednafen_wswan":      ("libretro/beetle-wswan-libretro",     "mednafen_wswan_libretro_android.so",      "make",  "base",  0.35, "WonderSwan (Beetle Cygne successor repo)"),
 "mednafen_psx":        ("libretro/beetle-psx-libretro",       "mednafen_psx_libretro_android.so",        "make",  "heavy", 3.50, "PS1 software renderer (no HW build)"),
 "np2kai":              ("libretro/NP2kai",                    "np2kai_libretro_android.so",              "cmake", "base",  1.70, "PC-88 + PC-98"),
 "mednafen_pcfx":       ("libretro/beetle-pcfx-libretro",      "mednafen_pcfx_libretro_android.so",       "make",  "base",  1.20, "PC-FX"),
 "mednafen_pce_fast":   ("libretro/beetle-pce-fast-libretro",  "mednafen_pce_fast_libretro_android.so",   "make",  "base",  0.90, "TurboGrafx-16"),
 "mednafen_pce":        ("libretro/beetle-pce-libretro",       "mednafen_pce_libretro_android.so",        "make",  "base",  1.10, "TurboGrafx-CD"),
 "dosbox_pure":         ("libretro/dosbox-pure",               "dosbox_pure_libretro_android.so",         "make",  "heavy", 2.00, "MS-DOS"),
 "scummvm":             ("libretro/scummvm",                   "scummvm_libretro_android.so",             "make",  "heavy", 8.00, "engine subset build (see build-core.sh)"),
 "mednafen_ngp":        ("libretro/beetle-ngp-libretro",       "mednafen_ngp_libretro_android.so",        "make",  "base",  0.45, "Neo Geo Pocket Color"),
}

# system_key -> (display name, core_id, extensions, needs_bios, armv7_ok)
SYSTEMS = {
 "nes":      ("Nintendo Entertainment System", "fceumm",            ["nes","unf","unif"],              False, True),
 "fds":      ("Famicom Disk System",           "fceumm",            ["fds"],                           True,  True),
 "gb":       ("Game Boy",                      "gambatte",          ["gb"],                            False, True),
 "gbc":      ("Game Boy Color",                "gambatte",          ["gbc"],                           False, True),
 "gba":      ("Game Boy Advance",              "mgba",              ["gba"],                           True,  True),
 "n64":      ("Nintendo 64",                   "mupen64plus_next",  ["n64","z64","v64"],               False, True),
 "nds":      ("Nintendo DS",                   "melonds",           ["nds","srl"],                     True,  False),
 # Hidden entry: never matched by extension, only reached through melonds'
 # fallback_core_id on armeabi-v7a (melonDS' JIT is 64-bit only).
 "nds32":    ("Nintendo DS (32-bit fallback)", "desmume",           [],                                True,  True),
 "snes":     ("Super Nintendo",                "snes9x",            ["smc","sfc","fig","swc","bs"],    False, True),
 "32x":      ("Sega 32X",                      "picodrive",         ["32x"],                           False, True),
 "dreamcast":("Dreamcast",                     "flycast",           ["gdi","cdi"],                     False, True),
 "gamegear": ("Game Gear",                     "genesis_plus_gx",   ["gg"],                            False, True),
 "genesis":  ("Genesis / Mega Drive",          "genesis_plus_gx",   ["md","gen","smd","68k"],          False, True),
 "mastersystem":("Master System",              "genesis_plus_gx",   ["sms"],                           False, True),
 "segacd":   ("Sega CD",                       "genesis_plus_gx",   ["chd","cue","iso"],               True,  True),
 "segapico": ("Sega Pico",                     "genesis_plus_gx",   ["bin"],                           False, True),
 "saturn":   ("Sega Saturn",                   "mednafen_saturn",   ["chd","m3u"],                     True,  True),
 "psp":      ("PlayStation Portable",          "ppsspp_native",     ["iso","cso","pbp","elf","prx"],   False, True),
 "ps1":      ("PlayStation",                   "mednafen_psx",      ["pbp","m3u","chd","cue","iso"],   True,  True),
 "pc88":     ("PC-8801",                       "np2kai",            ["d88","88d"],                     True,  True),
 "pc98":     ("PC-9801",                       "np2kai",            ["hdi","fdi","hdm","d98","zip"],   True,  True),
 "pcfx":     ("PC-FX",                         "mednafen_pcfx",     ["chd","cue"],                     True,  True),
 "tg16":     ("TurboGrafx-16",                 "mednafen_pce_fast", ["pce"],                           False, True),
 "tgcd":     ("TurboGrafx-CD",                 "mednafen_pce",      ["chd","cue"],                     True,  True),
 "ngcd":     ("Neo Geo CD",                    "fbneo",             ["chd","cue"],                     True,  True),
 "ngpc":     ("Neo Geo Pocket Color",          "mednafen_ngp",      ["ngc","ngpc"],                    False, True),
 "3do":      ("3DO",                           "opera",             ["chd","cue","iso"],               True,  True),
 "amiga":    ("Amiga",                         "puae",              ["adf","ipf","dsk","hdf","lha"],   True,  True),
 "arcade":   ("Arcade (FinalBurn Neo)",        "fbneo",             ["zip"],                           True,  True),
 "msx":      ("MSX",                           "bluemsx",           ["rom","mx1","mx2","cas","dsk"],   True,  True),
 "wonderswan":("WonderSwan",                   "mednafen_wswan",    ["ws","wsc"],                      False, True),
 "msdos":    ("MS-DOS",                        "dosbox_pure",       ["dosz","exe","com","bat","conf","zip"], True, True),
 "scummvm":  ("ScummVM",                       "scummvm",           ["scummvm","svm"],                 False, True),
}


def main():
    verified = {}
    if os.path.exists(VERIFIED):
        verified = json.load(open(VERIFIED))
    else:
        print("warning: %s missing, SHAs will be empty (run tools/verify_cores.py)" % VERIFIED,
              file=sys.stderr)

    cores = {}
    for cid, (repo, so, bs, tier, size, note) in CORES.items():
        v = verified.get(cid, {})
        cores[cid] = {
            "repo": "https://github.com/%s" % repo,
            "commit": v.get("sha", ""),
            "commit_date": v.get("date", ""),
            "so_name": so,
            "build_system": bs,
            "tier": tier,
            "est_size_mb_arm64": size,
            "notes": note,
        }

    systems = {}
    for key, (name, core, exts, bios, armv7) in SYSTEMS.items():
        systems[key] = {
            "hidden": len(exts) == 0,
            "display_name": name,
            "core": core,
            "extensions": exts,
            "needs_bios": bios,
            "armv7": armv7,
        }

    manifest = {
        "_comment": "Source of truth for systems, cores and ABI coverage. "
                    "Consumed by tools/gen_cpp_tables.py, scripts/build-core.sh and the CI workflow.",
        "frontend": {
            "app_name": "RetroFusion",
            "package": "com.retrofusion.app",
            "upstream": "https://github.com/hrydgard/ppsspp",
            "upstream_tag": "v1.18.1",
            "abis": ["arm64-v8a", "armeabi-v7a"],
            "max_apk_mb": 100,
        },
        "cores": cores,
        "systems": systems,
    }

    out = os.path.join(ROOT, "cores", "manifest.json")
    with open(out, "w") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
        f.write("\n")
    total = sum(c["est_size_mb_arm64"] for c in cores.values())
    print("wrote %s: %d systems, %d cores, %.1f MB estimated arm64 payload"
          % (out, len(systems), len(cores), total))


if __name__ == "__main__":
    main()
