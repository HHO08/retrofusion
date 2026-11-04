// RetroFusion - libretro core host (portable layer)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Deliberately free of any PPSSPP dependency so it can be unit tested on the
// build machine (tests/host_test.cpp) and reused by any frontend. The
// PPSSPP-specific glue lives in native/ppsspp/.
//
// Responsibilities:
//   * dlopen() a libretro core with strict symbol isolation (RTLD_LOCAL)
//   * implement the frontend side of the libretro API
//   * own content loading, SRAM/RTC persistence, savestates, core options
//   * quarantine crash-looping cores so one broken system can never take the
//     application down twice
//
// It intentionally does NOT own rendering, audio output or input mapping: the
// caller provides sinks.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "libretro.h"

namespace rf {

class VideoSink {
public:
	virtual ~VideoSink() {}
	// Valid until the next OnVideoFrame() call. data may be null when the core
	// allows frame duplication - keep showing the previous frame then.
	virtual void OnVideoFrame(const void *data, unsigned width, unsigned height, size_t pitch_bytes) = 0;
	virtual void OnGeometryChanged(unsigned width, unsigned height, unsigned max_width, unsigned max_height) {}
};

class AudioSink {
public:
	virtual ~AudioSink() {}
	virtual void OnAudioSamples(const int16_t *interleaved, size_t frames) = 0;  // stereo s16
};

// Returns raw libretro device state ids (RETRO_DEVICE_ID_JOYPAD_*, ANALOG_*).
class InputSource {
public:
	virtual ~InputSource() {}
	virtual void BeginFrame() {}
	virtual int16_t State(unsigned port, unsigned device, unsigned index, unsigned id) = 0;
	virtual void OnRumble(unsigned port, unsigned effect, uint16_t strength) {}
};

enum class PixelFormat {
	k0RGB1555 = 0,
	kXRGB8888 = 1,
	kRGB565 = 2,
};

struct AvInfo {
	unsigned base_width = 0;
	unsigned base_height = 0;
	unsigned max_width = 0;
	unsigned max_height = 0;
	double fps = 60.0;
	double sample_rate = 44100.0;
};

struct CoreOptionValue {
	std::string label;
	std::string value;
};

struct CoreOptionDefinition {
	std::string key;
	std::string label;
	std::string info;
	std::vector<CoreOptionValue> values;
	std::string default_value;
	bool hidden = false;
};

enum class LoadResult {
	kOk = 0,
	kCoreMissing,
	kCoreRejected,
	kGameUnsupported,
	kQuarantined,
};

class CoreHost {
public:
	CoreHost();
	~CoreHost();

	CoreHost(const CoreHost &) = delete;
	CoreHost &operator=(const CoreHost &) = delete;

	void SetSinks(VideoSink *video, AudioSink *audio, InputSource *input);
	void SetDirectories(const std::string &system_dir, const std::string &save_dir);
	void SetOptionOverrides(const std::map<std::string, std::string> &overrides);
	void SetLanguage(unsigned libretro_language) { language_ = libretro_language; }

	LoadResult Load(const std::string &core_id, const std::string &core_path,
	                const std::string &game_path, std::string *error);
	void Unload();
	bool Loaded() const { return loaded_; }

	bool RunFrame();
	void Reset();
	void SetPaused(bool paused) { paused_ = paused; }
	bool Paused() const { return paused_; }
	bool ShutdownRequested() const { return requested_shutdown_; }

	PixelFormat Format() const { return format_; }
	AvInfo GetAvInfo() const { return av_info_; }
	const std::string &CoreName() const { return core_name_; }
	const std::string &CoreVersion() const { return core_version_; }
	const std::string &CoreId() const { return core_id_; }
	const std::string &LastError() const { return last_error_; }
	bool SupportsNoGame() const { return supports_no_game_; }

	bool SavestatesSupported() const { return serialize_size_ > 0; }
	size_t SerializeSize() const { return serialize_size_; }
	bool SaveState(int slot, std::string *error);
	bool LoadState(int slot, std::string *error);
	std::string StateFingerprint() const;

	std::vector<CoreOptionDefinition> Options() const { return option_defs_; }
	std::string GetOption(const std::string &key) const;
	void SetOption(const std::string &key, const std::string &value);
	bool OptionsDirty() const { return options_dirty_; }
	void ClearOptionsDirty() { options_dirty_ = false; }
	std::map<std::string, std::string> ActiveOptions() const { return option_active_; }

	void FlushSram();

	std::string SramPath() const;
	std::string RtcPath() const;
	std::string StatePath(int slot) const;

	// --- isolation / health ---------------------------------------------
	static void SetHealthDirectory(const std::string &dir);
	static int CrashCount(const std::string &core_id);
	static bool IsQuarantined(const std::string &core_id);
	static void ClearQuarantine(const std::string &core_id);
	// Called once at startup: any leftover "running" marker means the previous
	// session died inside that core.
	static void ReconcileHealthAtStartup();
	static InputSource *ActiveInput();

private:
	struct CoreApi;

	static bool EnvironmentCb(unsigned cmd, void *data);
	static void VideoRefreshCb(const void *data, unsigned width, unsigned height, size_t pitch);
	static void AudioSampleCb(int16_t left, int16_t right);
	static size_t AudioBatchCb(const int16_t *data, size_t frames);
	static void InputPollCb(void);
	static int16_t InputStateCb(unsigned port, unsigned device, unsigned index, unsigned id);

	bool RunEnvironment(unsigned cmd, void *data);
	bool ParseCoreOptionsV2(unsigned cmd, void *data);
	void ReadCoreDeclaredOptionsFromOverrides();
	void ApplyOptionsToCore();

	static CoreHost *s_active;

	std::unique_ptr<CoreApi> api_;

	std::string core_id_;
	std::string core_path_;
	std::string game_path_;
	std::string system_dir_;
	std::string save_dir_;
	std::string core_name_;
	std::string core_version_;
	std::string last_error_;

	VideoSink *video_ = nullptr;
	AudioSink *audio_ = nullptr;
	InputSource *input_ = nullptr;

	PixelFormat format_ = PixelFormat::kRGB565;
	AvInfo av_info_;
	bool loaded_ = false;
	bool paused_ = false;
	bool supports_no_game_ = true;
	bool input_bitmasks_ = false;
	bool av_enable_ = true;
	bool requested_shutdown_ = false;
	unsigned language_ = RETRO_LANGUAGE_ENGLISH;
	double fps_ = 60.0;
	size_t serialize_size_ = 0;

	std::map<std::string, std::string> option_overrides_;
	std::map<std::string, std::string> option_active_;
	std::vector<CoreOptionDefinition> option_defs_;
	bool options_dirty_ = false;

	void *sram_ = nullptr;
	size_t sram_size_ = 0;
	void *rtc_ = nullptr;
	size_t rtc_size_ = 0;
	int64_t sram_flush_timer_ = 0;

	struct retro_log_callback log_cb_;
	struct retro_rumble_interface rumble_;
	struct retro_disk_control_callback disk_control_;
};

const char *PixelFormatName(PixelFormat f);

}  // namespace rf
