// RetroFusion - host layer test suite (runs on the build machine, no device).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Proves, with a real dlopen()'d libretro core, that:
//   1. a core loads through the host and negotiates its pixel format
//   2. video frames, input state and audio all flow through the sinks
//   3. core options actually reach the core (potato profile)
//   4. savestates round-trip and are refused across a settings change
//   5. SRAM is written and restored
//   6. crash-loop quarantine works and is per-core
//   7. extension/sniffing resolution picks the right core (incl. the NDS
//      armeabi-v7a fallback and disc-header sniffing)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "corehost/content_sniff.h"
#include "corehost/core_options.h"
#include "corehost/core_registry.h"
#include "corehost/libretro_host.h"
#include "corehost/path_util.h"

static int g_failures = 0;
static int g_checks = 0;

static void Check(bool condition, const char *what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		printf("  FAIL  %s\n", what);
	} else {
		printf("  ok    %s\n", what);
	}
}

static void TouchFile(const std::string &path, const char *content, size_t size) {
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return;
	if (size) fwrite(content, 1, size, f);
	fclose(f);
}

class TestVideoSink : public rf::VideoSink {
public:
	void OnVideoFrame(const void *data, unsigned width, unsigned height, size_t pitch) override {
		frames++;
		if (!data) { null_frames++; return; }
		last_width = width;
		last_height = height;
		const uint16_t *px = static_cast<const uint16_t *>(data);
		first_pixel = px[0];
		(void)pitch;
	}
	int frames = 0;
	int null_frames = 0;
	unsigned last_width = 0, last_height = 0;
	uint16_t first_pixel = 0;
};

class TestAudioSink : public rf::AudioSink {
public:
	void OnAudioSamples(const int16_t *interleaved, size_t frames_) override {
		total += frames_;
		(void)interleaved;
	}
	size_t total = 0;
};

class TestInput : public rf::InputSource {
public:
	int16_t State(unsigned port, unsigned device, unsigned index, unsigned id) override {
		calls++;
		if (port == 0 && device == RETRO_DEVICE_JOYPAD && index == 0 && id == RETRO_DEVICE_ID_JOYPAD_A) {
			return pressed_a;
		}
		return 0;
	}
	int16_t pressed_a = 0;
	int calls = 0;
};

int main(int argc, char **argv) {
	const std::string core_so = (argc > 1) ? argv[1] : "build/dummy_core.so";
	const std::string tmp = "/tmp/rf_test";
	std::string cmd = "rm -rf " + tmp + " && mkdir -p " + tmp + "/system " + tmp + "/save " + tmp + "/health";
	if (system(cmd.c_str()) != 0) { printf("cannot prepare tmp dir\n"); return 1; }

	printf("== 1. manifest / registry ==\n");
	Check(rf::CoreTableSize() >= 30, "core table covers the full system list");
	rf::Resolution nds64 = rf::Resolve("/games/Professor.nds", true);
	Check(nds64.core && std::string(nds64.core->core_id) == "melonds", "arm64 .nds -> melonDS");
	rf::Resolution nds32 = rf::Resolve("/games/Professor.nds", false);
	Check(nds32.core && std::string(nds32.core->core_id) == "desmume",
	      "armv7 .nds -> DeSmuME fallback (melonDS is 64-bit only)");
	rf::Resolution gba = rf::Resolve("/games/Metroid.gba", true);
	Check(gba.core && std::string(gba.core->core_id) == "mgba", ".gba -> mGBA");
	rf::Resolution psp = rf::Resolve("/games/GodOfWar.cso", true);
	Check(psp.system == rf::SystemId::kUnknown && psp.core == nullptr,
	      ".cso -> left to the untouched PPSSPP core");
	rf::Resolution over = rf::Resolve("/games/Weird.bin", true, "picodrive");
	Check(over.core && std::string(over.core->core_id) == "picodrive", "per-game override wins");

	printf("== 2. disc header sniffing ==\n");
	{
		const std::string bin = tmp + "/sega_cd.bin";
		std::vector<char> buf(4096, 0);
		memcpy(buf.data(), "SEGADISCSYSTEM", 14);
		TouchFile(bin, buf.data(), buf.size());
		Check(rf::SniffContent(bin) == rf::SniffedSystem::kSegaCD, "Sega CD header detected");
		rf::Resolution r = rf::Resolve(bin, true);
		Check(r.core && std::string(r.core->core_id) == "genesis_plus_gx", ".bin w/ Sega CD header -> Genesis Plus GX");

		const std::string psx_bin = tmp + "/psx.bin";
		std::vector<char> psx(8192, 0);
		memcpy(psx.data() + 0x100, "PLAYSTATION", 11);
		TouchFile(psx_bin, psx.data(), psx.size());
		rf::Resolution r2 = rf::Resolve(psx_bin, true);
		Check(r2.core && std::string(r2.core->core_id) == "mednafen_psx", ".bin w/ PlayStation licence -> Beetle PSX");

		const std::string sat = tmp + "/sat.chd";
		TouchFile(sat, "", 0);
		Check(rf::SniffContent(sat) == rf::SniffedSystem::kUnknown, ".chd without header stays undecided");
	}

	printf("== 3. option store (potato profile) ==\n");
	rf::CoreOptionsStore store;
	store.Load(tmp + "/cores.ini");
	const rf::CoreOptionSchema *n64schema = rf::SchemaForCore("mupen64plus_next");
	Check(n64schema != nullptr, "mupen64plus_next has a schema");
	if (n64schema) {
		bool found = false;
		for (size_t i = 0; i < n64schema->count; ++i) {
			if (std::string(n64schema->specs[i].key) == "mupen64plus-cpucore") {
				found = true;
				Check(std::string(store.Effective("mupen64plus_next", n64schema->specs[i])) == "dynamic_recompiler",
				      "fresh install already uses the dynarec");
			}
		}
		Check(found, "dynarec option present in the schema");
	}
	const rf::CoreOptionSchema *flyschema = rf::SchemaForCore("flycast");
	Check(flyschema != nullptr, "flycast has a schema");
	if (flyschema) {
		int checked = 0;
		for (size_t i = 0; i < flyschema->count; ++i) {
			const std::string key = flyschema->specs[i].key;
			const std::string val = store.Effective("flycast", flyschema->specs[i]);
			if (key == "reicast_dsp" || key == "reicast_threaded_rendering" ||
			    key == "reicast_synchronous_rendering" || key == "reicast_alpha_sorting") {
				++checked;
				printf("        %-40s = %s\n", key.c_str(), val.c_str());
			}
		}
		Check(checked == 4, "Dreamcast performance options are present");
	}
	store.Set("flycast", "reicast_dsp", "enabled");
	Check(store.IsUserSet("flycast", "reicast_dsp"), "user override recorded");
	store.ResetToPotato("flycast");
	Check(!store.IsUserSet("flycast", "reicast_dsp"), "reset restores the potato profile");
	store.Save();
	rf::CoreOptionsStore reloaded;
	reloaded.Load(tmp + "/cores.ini");
	Check(reloaded.Fingerprint("flycast") == store.Fingerprint("flycast"), "settings survive a save/load round trip");

	printf("== 4. loading a real libretro core through the host ==\n");
	TestVideoSink video;
	TestAudioSink audio;
	TestInput input;
	input.pressed_a = 1;

	rf::CoreHost host;
	host.SetSinks(&video, &audio, &input);
	host.SetDirectories(tmp + "/system", tmp + "/save");
	rf::CoreHost::SetHealthDirectory(tmp + "/health");

	// Route our dummy option through the same path the real cores use.
	std::map<std::string, std::string> overrides;
	overrides["dummy_option"] = "Xfast";
	host.SetOptionOverrides(overrides);

	const std::string game = tmp + "/test.dmy";
	TouchFile(game, "DUMMY", 5);

	std::string error;
	rf::LoadResult result = host.Load("dummy", core_so, game, &error);
	Check(result == rf::LoadResult::kOk, "core loaded");
	if (result != rf::LoadResult::kOk) {
		printf("        error: %s\n", error.c_str());
		printf("\n%d checks, %d failures\n", g_checks, g_failures);
		return 1;
	}
	Check(host.Format() == rf::PixelFormat::kRGB565, "pixel format negotiated (RGB565)");
	Check(host.GetAvInfo().base_width == 320 && host.GetAvInfo().base_height == 240, "geometry read");
	Check(std::string(host.CoreName()) == "DummyCore", "core identity read");

	for (int i = 0; i < 60; ++i) host.RunFrame();
	Check(video.frames == 60, "one video frame per retro_run");
	Check(audio.total == 735 * 60, "audio samples delivered for every frame");
	Check(input.calls > 0, "input state polled");
	// DummyCore encodes option[0] in the low byte and A-button in the high byte.
	Check((video.first_pixel & 0xFF) == 'X', "core option value reached the core");
	Check(((video.first_pixel >> 8) & 0xFF) == 1, "button state reached the core");

	printf("== 5. savestates ==\n");
	Check(host.SavestatesSupported(), "core reports savestate support");
	Check(host.SaveState(0, &error), "state saved");
	Check(host.LoadState(0, &error), "state loaded");
	// Changing an option must invalidate old states (prevents a very common
	// class of "game crashed after loading a state" bug reports).
	std::map<std::string, std::string> changed;
	changed["dummy_option"] = "Ydifferent";
	host.SetOptionOverrides(changed);
	host.SetOption("dummy_option", "Ydifferent");
	Check(!host.LoadState(0, &error), "state refused after a settings change");
	Check(host.SaveState(0, &error), "state re-saved under the new settings");

	printf("== 6. SRAM persistence ==\n");
	host.FlushSram();
	Check(rf::FileExists(tmp + "/save/test.srm"), "SRAM written next to the game's save dir");
	Check(rf::FileExists(tmp + "/save/states/test.state0"), "savestate file created");

	printf("== 7. crash-loop quarantine (per core) ==\n");
	rf::CoreHost::ClearQuarantine("dummy");
	Check(rf::CoreHost::CrashCount("dummy") == 0, "counter starts at zero");
	// Simulate two unclean exits of the same core.
	for (int i = 0; i < 2; ++i) {
		TouchFile(tmp + "/health/dummy.running", "1", 1);
		rf::CoreHost::ReconcileHealthAtStartup();
	}
	Check(rf::CoreHost::CrashCount("dummy") == 2, "two unclean exits counted");
	Check(rf::CoreHost::IsQuarantined("dummy"), "core quarantined after two crashes");
	Check(!rf::CoreHost::IsQuarantined("flycast"), "quarantine does not leak to other cores");
	rf::CoreHost::ClearQuarantine("dummy");
	Check(!rf::CoreHost::IsQuarantined("dummy"), "\"Start anyway\" clears the quarantine");

	host.Unload();
	Check(!rf::FileExists(tmp + "/health/dummy.running"), "clean unload removes the crash marker");

	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
