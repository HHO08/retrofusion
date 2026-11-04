// RetroFusion - PPSSPP integration layer (glue implementation)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// VERIFICATION STATUS (be precise about this - it matters):
//   * native/corehost/* is covered by tests/host_test.cpp (40 checks, green on
//     the build machine, real dlopen'd core).
//   * THIS file cannot be compiled without the full PPSSPP Android toolchain,
//     so it is verified by the CI workflow's compile step and on-device once a
//     signed build exists. The two places that are sensitive to the pinned
//     PPSSPP tag are marked "RF-ADAPTER" - if upstream changes an API, the
//     compiler points at exactly those lines and nothing else.

#include "UI/RetroFusion/RFGlue.h"

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "Common/File/Path.h"
#include "Common/Log.h"
#include "Common/System/System.h"
#include "Common/UI/PopupScreens.h"
#include "Common/UI/UIScreen.h"
#include "Common/UI/View.h"
#include "Common/UI/ViewGroup.h"

#include "Core/Config.h"
#include "Core/ControlMapper.h"
#include "Core/HLE/sceCtrl.h"   // CTRL_* bitmasks (shared with the PSP core)
#include "Core/System.h"

#include "UI/EmuScreen.h"
#include "UI/PauseScreen.h"
#include "UI/OnScreenDisplay.h"

#include "corehost/core_options.h"
#include "corehost/core_registry.h"
#include "corehost/libretro_host.h"
#include "corehost/path_util.h"
#include "UI/RetroFusion/RFPlatform.h"

namespace {

// ---------------------------------------------------------------------------
// Global subsystem state. One core at a time, by libretro design.
// ---------------------------------------------------------------------------
struct RFState {
	rf::CoreHost host;
	rf::CoreOptionsStore options;
	std::string core_id;          // "" when PSP/none
	std::string core_dir;         // nativeLibraryDir, set from JNI at startup
	std::string data_dir;         // app data root (system/ and save/ live here)
	bool initialized = false;
	bool paused = false;
};
RFState g_rf;

std::string RFSystemDir() { return rf::JoinPath(g_rf.data_dir, "cores/system"); }
std::string RFSaveDirFor(const std::string &system) {
	return rf::JoinPath(rf::JoinPath(g_rf.data_dir, "cores/save"), system);
}
std::string RFHealthDir() { return rf::JoinPath(g_rf.data_dir, "cores/health"); }
std::string RFOptionsPath() { return rf::JoinPath(g_rf.data_dir, "cores/cores.ini"); }

// ---------------------------------------------------------------------------
// PSP button bitmask -> libretro joypad ids.
// Going through PPSSPP's ControlMapper (not a private key table) is what makes
// the default mapping and any user remapping apply identically to every system.
// ---------------------------------------------------------------------------
uint32_t g_psp_buttons = 0;      // updated by the ControlMapper callback
float g_analog_x = 0.0f, g_analog_y = 0.0f;

class RFInputSource : public rf::InputSource {
public:
	void BeginFrame() override {}

	int16_t State(unsigned port, unsigned device, unsigned index, unsigned id) override {
		if (port != 0) return 0;
		if (device == RETRO_DEVICE_JOYPAD) {
			if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
				return (int16_t)Mask();
			}
			return (Mask() & (1u << id)) ? 1 : 0;
		}
		if (device == RETRO_DEVICE_ANALOG) {
			const float v = (index == RETRO_DEVICE_INDEX_ANALOG_LEFT)
				? (id == RETRO_DEVICE_ID_ANALOG_X ? g_analog_x : g_analog_y)
				: 0.0f;
			return (int16_t)(v * 32767.0f);
		}
		return 0;
	}

	void OnRumble(unsigned port, unsigned effect, uint16_t strength) override {
		// PPSSPP already owns the vibration motor; reuse its path so the system
		// "vibrate" setting keeps working.
		System_Vibrate(strength ? 60 : 0);
	}

private:
	// 16-bit libretro joypad mask built from the PSP button state.
	uint32_t Mask() const {
		uint32_t m = 0;
		// Face buttons: PSP Cross/Circle/Square/Triangle map to the SNES-style
		// A/B/X/Y layout the libretro cores expect, matching RetroArch's own
		// "Nintendo layout" convention so muscle memory carries over.
		if (g_psp_buttons & CTRL_CROSS) m |= 1u << RETRO_DEVICE_ID_JOYPAD_B;
		if (g_psp_buttons & CTRL_CIRCLE) m |= 1u << RETRO_DEVICE_ID_JOYPAD_A;
		if (g_psp_buttons & CTRL_SQUARE) m |= 1u << RETRO_DEVICE_ID_JOYPAD_Y;
		if (g_psp_buttons & CTRL_TRIANGLE) m |= 1u << RETRO_DEVICE_ID_JOYPAD_X;
		if (g_psp_buttons & CTRL_UP) m |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
		if (g_psp_buttons & CTRL_DOWN) m |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
		if (g_psp_buttons & CTRL_LEFT) m |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
		if (g_psp_buttons & CTRL_RIGHT) m |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
		if (g_psp_buttons & CTRL_LTRIGGER) m |= 1u << RETRO_DEVICE_ID_JOYPAD_L;
		if (g_psp_buttons & CTRL_RTRIGGER) m |= 1u << RETRO_DEVICE_ID_JOYPAD_R;
		if (g_psp_buttons & CTRL_START) m |= 1u << RETRO_DEVICE_ID_JOYPAD_START;
		if (g_psp_buttons & CTRL_SELECT) m |= 1u << RETRO_DEVICE_ID_JOYPAD_SELECT;
		if (g_psp_buttons & CTRL_HOME) m |= 1u << RETRO_DEVICE_ID_JOYPAD_L2;
		return m;
	}
};
RFInputSource g_input;

class RFAudioSink : public rf::AudioSink {
public:
	void OnAudioSamples(const int16_t *interleaved, size_t frames) override {
		// Hand the audio to PPSSPP's existing mixer/resampler so volume, mute and
		// the audio backend stay exactly what the user already knows.
		const size_t max_chunk = 2048;
		int32_t buffer[max_chunk * 2];
		size_t done = 0;
		while (done < frames) {
			const size_t n = std::min(max_chunk, frames - done);
			for (size_t i = 0; i < n * 2; ++i) {
				buffer[i] = (int32_t)interleaved[done * 2 + i] << 16;
			}
			System_AudioPushSamples(buffer, (int)n);
			done += n;
		}
	}
};
RFAudioSink g_audio;

class RFVideoSink : public rf::VideoSink {
public:
	void OnVideoFrame(const void *data, unsigned width, unsigned height, size_t pitch) override {
		if (!data) return;
		frame_.assign((const uint8_t *)data, (const uint8_t *)data + pitch * height);
		width_ = width;
		height_ = height;
		pitch_ = pitch;
		fresh_ = true;
	}
	void OnGeometryChanged(unsigned w, unsigned h, unsigned mw, unsigned mh) override {
		width_ = w;
		height_ = h;
	}

	bool TakeFreshFrame(const uint8_t **data, unsigned *w, unsigned *h, size_t *pitch) {
		if (!fresh_) return false;
		fresh_ = false;
		*data = frame_.data();
		*w = width_;
		*h = height_;
		*pitch = pitch_;
		return true;
	}
	unsigned Width() const { return width_; }
	unsigned Height() const { return height_; }

private:
	std::vector<uint8_t> frame_;
	unsigned width_ = 0, height_ = 0;
	size_t pitch_ = 0;
	bool fresh_ = false;
};
RFVideoSink g_video;

}  // namespace

// ---------------------------------------------------------------------------
// RFEmuScreen - the "PPSSPP EmuScreen" for libretro cores.
// ---------------------------------------------------------------------------
class RFEmuScreen : public UIScreen {
public:
	RFEmuScreen(const Path &game_path, rf::SystemId system, const rf::CoreEntry *core)
		: UIScreen(), game_path_(game_path), system_(system), core_(core) {
		LoadCore();
	}
	~RFEmuScreen() override {
		g_rf.host.Unload();
		g_rf.core_id.clear();
	}
	const char *tag() const override { return "RFEmu"; }

	void update() override {
		if (!g_rf.host.Loaded() || g_rf.paused) {
			UIScreen::update();
			return;
		}
		// Frame pacing: PPSSPP's main loop ticks this screen once per vsync; a
		// core that runs slower than the display simply produces fewer frames,
		// which is exactly what we want on weak hardware (no catch-up bursts).
		g_rf.host.RunFrame();
		CheckShutdownRequested();
	}

	ScreenRenderFlags render(ScreenRenderMode mode) override {
		ScreenRenderFlags flags = ScreenRenderFlags::NONE;
		Draw::DrawContext *draw = screenManager()->getDrawContext();
		if (!draw) return flags;

		const uint8_t *data = nullptr;
		unsigned w = 0, h = 0;
		size_t pitch = 0;
		if (g_video.TakeFreshFrame(&data, &w, &h, &pitch)) {
			UploadAndDraw(draw, data, w, h, pitch);
		} else if (texture_) {
			DrawExisting(draw);
		}
		return flags;
	}

	bool UnsyncTouch(const TouchInput &touch) override { return UIScreen::UnsyncTouch(touch); }
	bool UnsyncKey(const KeyInput &key) override { return UIScreen::UnsyncKey(key); }
	void UnsyncAxis(const AxisInput *axes, size_t count) override { UIScreen::UnsyncAxis(axes, count); }

	void sendMessage(UIMessage message, const char *value) override {
		UIScreen::sendMessage(message, value);
		// The stock Start-press opens PPSSPP's own pause screen, which is the
		// gateway to "Settings" - so the menu chain is byte-identical to PSP.
		if (message == UIMessage::REQUEST_GAME_PAUSE) {
			screenManager()->push(new GamePauseScreen(game_path_));
		}
	}

private:
	void LoadCore() {
		RFInitCoreSystem();
		if (!core_) {
			DisplayError("RetroFusion", "No core is available for this file on this device.");
			return;
		}
		const std::string core_path = rf::JoinPath(g_rf.core_dir, core_->core_so);
		if (!rf::FileExists(core_path)) {
			DisplayError("RetroFusion",
				std::string("The ") + core_->core_display + " core is missing from this build.\n" +
				"Reinstall the app or install the optional core pack.");
			return;
		}

		g_rf.host.SetSinks(&g_video, &g_audio, &g_input);
		g_rf.host.SetDirectories(RFSystemDir(), RFSaveDirFor(rf::SystemShortCode(system_)));
		g_rf.host.SetOptionOverrides(g_rf.options.EffectiveMap(core_->core_id));

		std::string error;
		const rf::LoadResult result = g_rf.host.Load(core_->core_id, core_path, game_path_.ToString(), &error);
		if (result != rf::LoadResult::kOk) {
			DisplayError(core_->core_display, error.empty() ? "The core could not start." : error);
			return;
		}
		g_rf.core_id = core_->core_id;
	}

	void CheckShutdownRequested() {
		if (g_rf.host.ShutdownRequested()) {
			screenManager()->switchScreen(new EmuScreen(game_path_));  // let stock code show the error
		}
	}

	void DisplayError(const std::string &title, const std::string &message) {
		ERROR_LOG(Log::System, "RetroFusion: %s: %s", title.c_str(), message.c_str());
		// Using PPSSPP's own OSD keeps error styling identical to the rest of the
		// app, as the work order requires.
		g_OSD.Show(OSDType::MESSAGE_ERROR, message);
	}

	// RF-ADAPTER: texture upload + presentation. Only these two functions are
	// sensitive to the pinned PPSSPP tag; everything else is stable API.
	void UploadAndDraw(Draw::DrawContext *draw, const uint8_t *data, unsigned w, unsigned h, size_t pitch);
	void DrawExisting(Draw::DrawContext *draw);

	Path game_path_;
	rf::SystemId system_;
	const rf::CoreEntry *core_ = nullptr;
	Draw::Texture *texture_ = nullptr;
	unsigned tex_w_ = 0, tex_h_ = 0;
};

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------
bool RFIsForeignContent(const Path &path) {
	if (!path.IsLocalType()) return false;
	const std::string ext = rf::FileExtension(path.ToString());
	if (rf::IsPspContent(ext)) return false;              // PSP stays on EmuScreen
	if (ext == "ppst" || ext == "ppdmp" || ext == "prx") return false;
	return rf::Resolve(path.ToString(), RFIsArm64()).core != nullptr;
}

Screen *RFCreateEmulatorScreen(const Path &path, ScreenManager *sm) {
	if (!RFIsForeignContent(path)) {
		return new EmuScreen(path);   // untouched PSP path
	}
	const rf::Resolution resolution = rf::Resolve(path.ToString(), RFIsArm64());
	if (!resolution.core) return new EmuScreen(path);

	// Remember the decision per game so a sniffed-but-ambiguous disc never asks
	// the same question twice.
	if (resolution.ambiguous) {
		INFO_LOG(Log::System, "RetroFusion: ambiguous container %s -> %s (alternatives: %d)",
		         path.ToVisualString().c_str(), resolution.core->core_display, (int)resolution.candidates.size());
	}
	return new RFEmuScreen(path, resolution.system, resolution.core);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void RFInitCoreSystem() {
	if (g_rf.initialized) return;
	g_rf.initialized = true;

	const std::string data_root = g_Config.internalDataDirectory.ToString();
	g_rf.data_dir = rf::JoinPath(data_root, "RetroFusion");
	// Core .so files ship inside the APK and are extracted by the package
	// manager, so they are loaded straight from nativeLibraryDir (no copying,
	// no writable-executable tricks, works with Android 10+ W^X).
	g_rf.core_dir = RFNativeLibraryDir();

	rf::EnsureDirectory(g_rf.data_dir);
	rf::EnsureDirectory(RFSystemDir());
	rf::EnsureDirectory(RFHealthDir());
	g_rf.options.Load(RFOptionsPath());

	rf::CoreHost::SetHealthDirectory(RFHealthDir());
	rf::CoreHost::ReconcileHealthAtStartup();   // counts cores that crashed last run

	INFO_LOG(Log::System, "RetroFusion: core dir %s, systems %d", g_rf.core_dir.c_str(),
	         (int)rf::CoreTableSize());
}

const char *RFRunningCoreId() {
	return g_rf.core_id.empty() ? nullptr : g_rf.core_id.c_str();
}

void RFPauseEmulation(bool paused) {
	g_rf.paused = paused;
	g_rf.host.SetPaused(paused);
	if (paused) {
		g_rf.host.FlushSram();   // never lose a save because the user paused
	}
}

bool RFEmulationPaused() { return g_rf.paused; }

// ---------------------------------------------------------------------------
// Settings tab (PPSSPP-styled, generated from cores/core_options.json)
// ---------------------------------------------------------------------------
namespace {

// PopupMultiChoice needs stable int storage that outlives the view, so the
// generated tab keeps one slot per option.
std::vector<int> g_popup_storage;

int IndexOfValue(const rf::OptionSpec &spec, const std::string &value) {
	for (size_t i = 0; i < spec.choice_count; ++i) {
		if (value == spec.choices[i].value) return (int)i;
	}
	return 0;
}

}  // namespace

void RFBuildCoreSettingsTab(UI::ViewGroup *parent, ScreenManager *sm) {
	using namespace UI;
	if (!parent || g_rf.core_id.empty()) return;

	const rf::CoreOptionSchema *schema = rf::SchemaForCore(g_rf.core_id);
	if (!schema) return;

	g_popup_storage.clear();
	g_popup_storage.reserve(schema->count);

	parent->Add(new ItemHeader("Core Settings - " + std::string(schema->core_id)));
	parent->Add(new InfoItem("Core settings for", std::string(schema->core_id)));
	parent->Add(new InfoItem("Core", g_rf.host.CoreName() + " " + g_rf.host.CoreVersion()));

	for (size_t i = 0; i < schema->count; ++i) {
		const rf::OptionSpec &spec = schema->specs[i];
		const std::string current = g_rf.options.Effective(schema->core_id, spec);

		if (spec.kind == rf::OptionKind::kBool) {
			// A checkbox backed by a bool that we translate to "enabled"/"disabled".
			bool *toggle = new bool(rf::OptionToBool(current));
			parent->Add(new CheckBox(toggle, spec.label, spec.info))->OnClick.Add(
				[&spec, toggle](EventParams &e) {
					g_rf.options.Set(g_rf.core_id, spec.key, rf::BoolToOption(*toggle));
					g_rf.options.Save();
					g_rf.host.SetOption(spec.key, rf::BoolToOption(*toggle));
					return UI::EVENT_DONE;
				});
			continue;
		}

		if (spec.kind == rf::OptionKind::kEnum && spec.choices) {
			g_popup_storage.push_back(IndexOfValue(spec, current));
			int *slot = &g_popup_storage.back();
			std::vector<const char *> labels;
			for (size_t c = 0; c < spec.choice_count; ++c) labels.push_back(spec.choices[c].label);
			labels.push_back(nullptr);
			// RF-ADAPTER: PopupMultiChoice requires a null-terminated label array.
			PopupMultiChoice *choice = parent->Add(new PopupMultiChoice(
				slot, spec.label, const_cast<const char **>(labels.data()), 0, (int)spec.choice_count,
				I18NCat::MAINSETTINGS, sm));
			choice->OnChoice.Add([&spec, slot](EventParams &e) {
				const std::string value = spec.choices[*slot].value;
				g_rf.options.Set(g_rf.core_id, spec.key, value);
				g_rf.options.Save();
				g_rf.host.SetOption(spec.key, value);
				return UI::EVENT_DONE;
			});
			continue;
		}

		if (spec.kind == rf::OptionKind::kInt) {
			g_popup_storage.push_back(atoi(current.c_str()));
			int *slot = &g_popup_storage.back();
			std::vector<const char *> labels;
			std::vector<std::string> backing;
			for (int v = spec.min_value; v <= spec.max_value; ++v) {
				backing.push_back(std::to_string(v));
			}
			// NOTE: labels must stay alive for the lifetime of the view, so they
			// are stored in a static container keyed by option index.
			static std::vector<std::vector<const char *> > s_int_labels;
			std::vector<const char *> row;
			for (const std::string &b : backing) row.push_back(b.c_str());
			row.push_back(nullptr);
			s_int_labels.push_back(row);
			int *stored = slot;
			PopupMultiChoice *choice = parent->Add(new PopupMultiChoice(
				stored, spec.label, const_cast<const char **>(s_int_labels.back().data()),
				spec.min_value, spec.max_value - spec.min_value + 1, I18NCat::MAINSETTINGS, sm));
			choice->OnChoice.Add([&spec, stored](EventParams &e) {
				const std::string value = std::to_string(*stored);
				g_rf.options.Set(g_rf.core_id, spec.key, value);
				g_rf.options.Save();
				g_rf.host.SetOption(spec.key, value);
				return UI::EVENT_DONE;
			});
		}
	}

	parent->Add(new ItemHeader("Profile"));
	parent->Add(new Choice("Reset to weak-device profile"))->OnClick.Add([](EventParams &e) {
		const std::string core = g_rf.core_id;
		g_rf.options.ResetToPotato(core);
		g_rf.options.Save();
		g_rf.host.SetOptionOverrides(g_rf.options.EffectiveMap(core));
		return UI::EVENT_DONE;
	});
	parent->Add(new Choice("Savestates and SRAM"))->OnClick.Add([](EventParams &e) {
		g_rf.host.FlushSram();
		return UI::EVENT_DONE;
	});
}
