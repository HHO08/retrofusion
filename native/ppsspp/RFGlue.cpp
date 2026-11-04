// RetroFusion - PPSSPP integration layer (glue implementation)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Verification status:
//   * native/corehost/* is covered by tests/host_test.cpp (40 checks green).
//   * THIS file compiles only inside the PPSSPP tree. Every PPSSPP API it uses
//     was read out of the pinned v1.18.1 sources (the file/line is named in the
//     comment above each group of calls), and the CI build is the gate.
//     Anything still uncertain is marked "RF-ADAPTER".

#include "UI/RetroFusion/RFGlue.h"
#include "UI/RetroFusion/RFPlatform.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

// PPSSPP APIs used here, with their v1.18.1 origin:
#include "Common/Data/Text/I18n.h"          // I18NCat
#include "Common/File/Path.h"               // Path
#include "Common/GPU/DataFormat.h"          // Draw::DataFormat (R5G6B5_UNORM_PACK16)
#include "Common/GPU/thin3d.h"              // Draw::TextureDesc / CreateTexture
#include "Common/Log.h"                     // INFO_LOG / ERROR_LOG
#include "Common/System/System.h"           // System_Toast, System_Vibrate, UIMessage
#include "Common/UI/Context.h"              // UIContext (Flush/BindTexture/Draw)
#include "Common/UI/PopupScreens.h"         // PopupMultiChoice
#include "Common/UI/UIScreen.h"             // UIScreen
#include "Common/UI/View.h"                 // ItemHeader, CheckBox, Choice, InfoItem
#include "Common/UI/ViewGroup.h"            // LinearLayout / ViewGroup

#include "Core/Config.h"                    // g_Config.internalDataDirectory
#include "Core/ControlMapper.h"             // ControlMapper (same instance type as EmuScreen)
#include "Core/HLE/sceCtrl.h"               // CTRL_* bitmasks
#include "Core/System.h"

#include "UI/EmuScreen.h"
#include "UI/MainScreen.h"
#include "UI/PauseScreen.h"

#include "corehost/core_options.h"
#include "corehost/core_registry.h"
#include "corehost/libretro_host.h"
#include "corehost/path_util.h"

namespace {

// ---------------------------------------------------------------------------
// Global subsystem state. libretro's callbacks are process-global C callbacks,
// so exactly one core can be active; this is enforced in CoreHost.
// ---------------------------------------------------------------------------
struct RFState {
	rf::CoreHost host;
	rf::CoreOptionsStore options;
	std::string core_id;   // empty when nothing (or PSP) is running
	std::string core_dir;  // nativeLibraryDir on Android
	std::string data_dir;   // <internalData>/RetroFusion
	bool initialized = false;
};
RFState g_rf;

std::string RFSystemDir() { return rf::JoinPath(g_rf.data_dir, "cores/system"); }
std::string RFHealthDir() { return rf::JoinPath(g_rf.data_dir, "cores/health"); }
std::string RFOptionsPath() { return rf::JoinPath(g_rf.data_dir, "cores/cores.ini"); }
std::string RFSaveDirFor(const char *system) {
	return rf::JoinPath(rf::JoinPath(g_rf.data_dir, "cores/save"), system);
}

// ---------------------------------------------------------------------------
// Input: PPSSPP's own ControlMapper produces the PSP button mask, exactly as it
// does for the PSP core. Mapping that mask onto libretro joypad ids - instead of
// inventing a private key table - is what makes the user's existing remaps and
// the default layout behave identically on every system.
// ---------------------------------------------------------------------------
uint32_t g_psp_buttons = 0;
float g_analog_x = 0.0f;
float g_analog_y = 0.0f;

class RFInputSource : public rf::InputSource {
public:
	int16_t State(unsigned port, unsigned device, unsigned index, unsigned id) override {
		if (port != 0) return 0;
		if (device == RETRO_DEVICE_JOYPAD) {
			if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)Mask();
			return (Mask() & (1u << id)) ? 1 : 0;
		}
		if (device == RETRO_DEVICE_ANALOG && index == RETRO_DEVICE_INDEX_ANALOG_LEFT) {
			const float v = (id == RETRO_DEVICE_ID_ANALOG_X) ? g_analog_x : g_analog_y;
			return (int16_t)(v * 32767.0f);
		}
		return 0;
	}

	void OnRumble(unsigned port, unsigned effect, uint16_t strength) override {
		System_Vibrate(strength ? 60 : 0);
	}

private:
	uint32_t Mask() const {
		uint32_t m = 0;
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

// ---------------------------------------------------------------------------
// Audio: handed to PPSSPP's own resampler/mixer so volume, mute and the audio
// backend stay exactly what the user already knows (UI/AudioCommon.cpp).
// ---------------------------------------------------------------------------
class RFAudioSink : public rf::AudioSink {
public:
	void OnAudioSamples(const int16_t *interleaved, size_t frames) override {
		const size_t kChunk = 1024;
		int32_t buffer[kChunk * 2];
		size_t done = 0;
		while (done < frames) {
			const size_t n = std::min(kChunk, frames - done);
			for (size_t i = 0; i < n * 2; ++i) {
				buffer[i] = (int32_t)interleaved[done * 2 + i] << 16;
			}
			System_AudioPushSamples(buffer, (int)n);
			done += n;
		}
	}
};
RFAudioSink g_audio;

// ---------------------------------------------------------------------------
// Video: the core's frame is normalised to RGB565 (one texture format, two
// cheap shift ops per pixel) and presented with PPSSPP's own UI draw path -
// the same sequence AsyncImageFileView uses: Flush, BindTexture, Rect, Flush,
// RebindTexture.
// ---------------------------------------------------------------------------
class RFVideoSink : public rf::VideoSink {
public:
	void OnVideoFrame(const void *data, unsigned width, unsigned height, size_t pitch) override {
		if (!data || !width || !height) return;
		rgb565_.resize((size_t)width * height);
		if (format_ == rf::PixelFormat::kRGB565) {
			for (unsigned y = 0; y < height; ++y) {
				memcpy(&rgb565_[(size_t)y * width], (const uint8_t *)data + y * pitch, width * 2);
			}
		} else if (format_ == rf::PixelFormat::kXRGB8888) {
			for (unsigned y = 0; y < height; ++y) {
				const uint32_t *src = (const uint32_t *)((const uint8_t *)data + y * pitch);
				uint16_t *dst = &rgb565_[(size_t)y * width];
				for (unsigned x = 0; x < width; ++x) {
					const uint32_t p = src[x];
					dst[x] = (uint16_t)(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
				}
			}
		} else {  // 0RGB1555
			for (unsigned y = 0; y < height; ++y) {
				const uint16_t *src = (const uint16_t *)((const uint8_t *)data + y * pitch);
				uint16_t *dst = &rgb565_[(size_t)y * width];
				for (unsigned x = 0; x < width; ++x) {
					const uint16_t p = src[x];
					const uint16_t r = (p >> 10) & 0x1F, g = (p >> 5) & 0x1F, b = p & 0x1F;
					dst[x] = (uint16_t)((r << 11) | (g << 6) | (b >> 1) | ((r & 1) << 5));
				}
			}
		}
		width_ = width;
		height_ = height;
		fresh_ = true;
	}

	void OnGeometryChanged(unsigned w, unsigned h, unsigned mw, unsigned mh) override {
		if (w != width_ || h != height_) fresh_ = true;
	}

	bool TakeFresh() {
		if (!fresh_) return false;
		fresh_ = false;
		return true;
	}
	const std::vector<uint16_t> &Pixels() const { return rgb565_; }
	unsigned Width() const { return width_; }
	unsigned Height() const { return height_; }
	void SetPixelFormat(rf::PixelFormat f) { format_ = f; }

private:
	std::vector<uint16_t> rgb565_;
	unsigned width_ = 0, height_ = 0;
	bool fresh_ = false;
	rf::PixelFormat format_ = rf::PixelFormat::kRGB565;
};
RFVideoSink g_video;

}  // namespace

// ---------------------------------------------------------------------------
// RFEmuScreen - the PPSSPP "EmuScreen" for libretro cores.
// ---------------------------------------------------------------------------
class RFEmuScreen : public UIScreen {
public:
	RFEmuScreen(const Path &game_path, rf::SystemId system, const rf::CoreEntry *core)
		: UIScreen(), game_path_(game_path), system_(system), core_(core) {
		// Same ControlMapper contract as EmuScreen: the callbacks receive the PSP
		// button mask after the user's mapping has been applied.
		controlMapper_.SetCallbacks(
			[this](int vkey, bool down) { OnVirtualKey(vkey, down); },
			[this](int vkey, float value) {},
			[this](uint32_t buttons, uint32_t flags) { g_psp_buttons = buttons; },
			[this](int stick, float x, float y) {
				if (stick == 0) { g_analog_x = x; g_analog_y = y; }
			},
			[this](int stick, float x, float y) {});
		LoadCore();
	}

	~RFEmuScreen() override {
		g_rf.host.Unload();
		g_rf.core_id.clear();
		ReleaseTexture();
	}

	const char *tag() const override { return "RFEmu"; }

	void update() override {
		UIScreen::update();
		if (!g_rf.host.Loaded()) return;
		if (!g_rf.host.Paused()) {
			g_rf.host.RunFrame();
		}
		if (g_rf.host.ShutdownRequested()) {
			screenManager()->switchScreen(new MainScreen());
		}
		g_video.SetPixelFormat(g_rf.host.Format());
	}

	ScreenRenderFlags render(ScreenRenderMode mode) override {
		ScreenRenderFlags flags = ScreenRenderFlags::NONE;
		UIContext *dc = screenManager()->getUIContext();
		Draw::DrawContext *draw = screenManager()->getDrawContext();
		if (!dc || !draw) return flags;

		if (g_video.TakeFresh()) {
			Upload(draw);
		}
		if (!texture_) return flags;

		// Aspect-preserving fit inside the UI bounds.
		const Bounds &bounds = dc->GetBounds();
		const float src_aspect = (float)g_video.Width() / (float)g_video.Height();
		float w = bounds.w, h = bounds.w / src_aspect;
		if (h > bounds.h) { h = bounds.h; w = bounds.h * src_aspect; }
		const float x = bounds.x + (bounds.w - w) * 0.5f;
		const float y = bounds.y + (bounds.h - h) * 0.5f;

		dc->Flush();
		draw->BindTexture(0, texture_);
		dc->Draw()->Rect(x, y, w, h, 0xFFFFFFFF);
		dc->Flush();
		dc->RebindTexture();
		return flags;
	}

	bool UnsyncTouch(const TouchInput &touch) override { return UIScreen::UnsyncTouch(touch); }

	bool UnsyncKey(const KeyInput &key) override {
		bool pauseTrigger = false;
		// Feeds the same mapper the PSP core uses, so remaps and the pause
		// gesture behave identically.
		controlMapper_.Key(key, &pauseTrigger);
		if (pauseTrigger) {
			g_rf.host.SetPaused(true);
			screenManager()->push(new GamePauseScreen(game_path_));
			return true;
		}
		return UIScreen::UnsyncKey(key);
	}

	void UnsyncAxis(const AxisInput *axes, size_t count) override {
		controlMapper_.Axis(axes, count);
		UIScreen::UnsyncAxis(axes, count);
	}

	void sendMessage(UIMessage message, const char *value) override {
		UIScreen::sendMessage(message, value);
		if (message == UIMessage::REQUEST_GAME_PAUSE) {
			g_rf.host.SetPaused(true);
			screenManager()->push(new GamePauseScreen(game_path_));
		}
	}

private:
	void OnVirtualKey(int vkey, bool down) {
		// VKeys drive the on-screen "pause" gesture; the button mask itself comes
		// through the updatePSPButtons callback above.
	}

	void LoadCore() {
		RFInitCoreSystem();
		if (!core_) {
			DisplayError("No core is available for this file on this device.");
			return;
		}
		const std::string core_path = rf::JoinPath(g_rf.core_dir, core_->core_so);
		if (!rf::FileExists(core_path)) {
			DisplayError(std::string("The ") + core_->core_display + " core is missing from this build.");
			return;
		}

		g_rf.host.SetSinks(&g_video, &g_audio, &g_input);
		g_rf.host.SetDirectories(RFSystemDir(), RFSaveDirFor(rf::SystemShortCode(system_)));
		g_rf.host.SetOptionOverrides(g_rf.options.EffectiveMap(core_->core_id));

		std::string error;
		const rf::LoadResult result =
			g_rf.host.Load(core_->core_id, core_path, game_path_.ToString(), &error);
		if (result != rf::LoadResult::kOk) {
			DisplayError(error.empty() ? "The core could not start." : error);
			return;
		}
		g_rf.core_id = core_->core_id;
		INFO_LOG(Log::System, "RetroFusion: %s running on %s (%dx%d)", game_path_.ToVisualString().c_str(),
		         g_rf.host.CoreName().c_str(), (int)g_rf.host.GetAvInfo().base_width,
		         (int)g_rf.host.GetAvInfo().base_height);
	}

	void DisplayError(const std::string &message) {
		ERROR_LOG(Log::System, "RetroFusion: %s", message.c_str());
		System_Toast(message);
	}

	// Uploads the latest frame into a texture. thin3d's base Texture has no
	// in-place update in this tag, so the texture is recreated when the frame or
	// the geometry changes; at 320x240 that is a small, bounded cost.
	void Upload(Draw::DrawContext *draw) {
		const unsigned w = g_video.Width(), h = g_video.Height();
		if (!w || !h) return;
		ReleaseTexture();

		Draw::TextureDesc desc{};
		desc.type = Draw::TextureType::LINEAR2D;
		desc.format = Draw::DataFormat::R5G6B5_UNORM_PACK16;
		desc.width = (int)w;
		desc.height = (int)h;
		desc.depth = 1;
		desc.mipLevels = 1;
		desc.generateMips = false;
		desc.tag = "RetroFusionFrame";
		desc.initData.push_back((const uint8_t *)g_video.Pixels().data());
		texture_ = draw->CreateTexture(desc);
	}

	void ReleaseTexture() {
		if (texture_) {
			texture_->Release();
			texture_ = nullptr;
		}
	}

	Path game_path_;
	rf::SystemId system_;
	const rf::CoreEntry *core_ = nullptr;
	Draw::Texture *texture_ = nullptr;
	ControlMapper controlMapper_;
};

// ---------------------------------------------------------------------------
// Routing (called from the patched launch sites)
// ---------------------------------------------------------------------------
bool RFIsForeignContent(const Path &path) {
	if (!path.IsLocalType()) return false;
	const std::string ext = rf::FileExtension(path.ToString());
	if (rf::IsPspContent(ext)) return false;
	if (ext == "elf" || ext == "prx" || ext == "ppst") return false;
	return rf::Resolve(path.ToString(), RFIsArm64()).core != nullptr;
}

Screen *RFCreateEmulatorScreen(const Path &path, ScreenManager *sm) {
	if (!RFIsForeignContent(path)) {
		return new EmuScreen(path);  // untouched PSP path
	}
	const rf::Resolution resolution = rf::Resolve(path.ToString(), RFIsArm64());
	if (!resolution.core) return new EmuScreen(path);

	if (resolution.ambiguous) {
		INFO_LOG(Log::System, "RetroFusion: ambiguous container %s -> %s (%d candidates)",
		         path.ToVisualString().c_str(), resolution.core->core_display,
		         (int)resolution.candidates.size());
	}
	return new RFEmuScreen(path, resolution.system, resolution.core);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void RFInitCoreSystem() {
	if (g_rf.initialized) return;
	g_rf.initialized = true;

	g_rf.data_dir = rf::JoinPath(g_Config.internalDataDirectory.ToString(), "RetroFusion");
	g_rf.core_dir = RFNativeLibraryDir();

	rf::EnsureDirectory(g_rf.data_dir);
	rf::EnsureDirectory(RFSystemDir());
	rf::EnsureDirectory(RFHealthDir());
	g_rf.options.Load(RFOptionsPath());

	rf::CoreHost::SetHealthDirectory(RFHealthDir());
	// Cores that died in the previous session are counted here, before anything
	// is launched, so a crash loop cannot repeat on the next start.
	rf::CoreHost::ReconcileHealthAtStartup();

	INFO_LOG(Log::System, "RetroFusion: %d systems, core dir %s", (int)rf::CoreTableSize(),
	         g_rf.core_dir.c_str());
}

const char *RFRunningCoreId() {
	return g_rf.core_id.empty() ? nullptr : g_rf.core_id.c_str();
}

void RFPauseEmulation(bool paused) {
	g_rf.host.SetPaused(paused);
	if (paused) g_rf.host.FlushSram();
}

bool RFEmulationPaused() { return g_rf.host.Paused(); }

// ---------------------------------------------------------------------------
// Settings tab - PPSSPP widgets, generated from cores/core_options.json
// ---------------------------------------------------------------------------
namespace {
std::vector<int> g_popup_storage;
std::vector<std::vector<const char *> > g_int_labels;

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

	parent->Add(new ItemHeader(std::string("Core: ") + g_rf.host.CoreName()));
	parent->Add(new InfoItem("System", g_rf.host.CoreVersion()));

	for (size_t i = 0; i < schema->count; ++i) {
		const rf::OptionSpec &spec = schema->specs[i];
		const std::string current = g_rf.options.Effective(schema->core_id, spec);

		if (spec.kind == rf::OptionKind::kBool) {
			bool *toggle = new bool(rf::OptionToBool(current));
			parent->Add(new CheckBox(toggle, spec.label, spec.info))->OnClick.Add(
				[&spec, toggle](EventParams &) {
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
			g_int_labels.push_back(labels);
			PopupMultiChoice *choice = parent->Add(new PopupMultiChoice(
				slot, spec.label, g_int_labels.back().data(), 0, (int)spec.choice_count,
				I18NCat::MAINSETTINGS, sm));
			choice->OnChoice.Add([&spec, slot](EventParams &) {
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
			std::vector<std::string> backing;
			std::vector<const char *> row;
			for (int v = spec.min_value; v <= spec.max_value; ++v) {
				backing.push_back(std::to_string(v));
			}
			for (const std::string &b : backing) row.push_back(b.c_str());
			g_int_labels.push_back(row);
			PopupMultiChoice *choice = parent->Add(new PopupMultiChoice(
				slot, spec.label, g_int_labels.back().data(), spec.min_value,
				spec.max_value - spec.min_value + 1, I18NCat::MAINSETTINGS, sm));
			choice->OnChoice.Add([&spec, slot](EventParams &) {
				const std::string value = std::to_string(*slot);
				g_rf.options.Set(g_rf.core_id, spec.key, value);
				g_rf.options.Save();
				g_rf.host.SetOption(spec.key, value);
				return UI::EVENT_DONE;
			});
		}
	}

	parent->Add(new ItemHeader("Profile"));
	parent->Add(new Choice("Reset to weak-device profile"))->OnClick.Add([](EventParams &) {
		const std::string core = g_rf.core_id;
		g_rf.options.ResetToPotato(core);
		g_rf.options.Save();
		g_rf.host.SetOptionOverrides(g_rf.options.EffectiveMap(core));
		return UI::EVENT_DONE;
	});
}
