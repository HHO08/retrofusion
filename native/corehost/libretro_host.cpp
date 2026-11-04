// RetroFusion - libretro core host implementation
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Project-critical notes:
//
//  * No PSP code is touched or referenced. The PSP path inside PPSSPP stays
//    byte-identical; this is an additive, parallel subsystem.
//
//  * Isolation: cores are dlopen()'d with RTLD_LOCAL. Every core exports the
//    same symbols (retro_init, retro_run, ...). Without RTLD_LOCAL a core loaded
//    later can bind to symbols of a core loaded earlier in the same process,
//    which is the classic cause of "core B crashes after core A ran".
//
//  * Crash containment: a hard SIGSEGV inside a core cannot be caught safely
//    in-process. Instead of pretending otherwise we implement crash-loop
//    quarantine: a marker file is dropped before entering a core and removed on
//    a clean exit. A leftover marker increments that core's crash counter, and
//    after two crashes the core is disabled with a clear message instead of
//    boot-looping the whole app. Every other system (and PSP) keeps working -
//    which is what "modular isolation" actually requires.

#include "libretro_host.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <dlfcn.h>

#include "core_options.h"
#include "path_util.h"

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace rf {

CoreHost *CoreHost::s_active = nullptr;

namespace {

std::mutex g_mutex;
std::string g_health_dir;

std::string HealthPath(const std::string &core_id) {
	return JoinPath(g_health_dir, core_id + ".crashes");
}
std::string MarkerPath(const std::string &core_id) {
	return JoinPath(g_health_dir, core_id + ".running");
}

int ReadIntFile(const std::string &path) {
	std::vector<char> buf;
	if (!ReadWholeFile(path, &buf) || buf.empty()) {
		return 0;
	}
	return atoi(std::string(buf.data(), buf.size()).c_str());
}

void WriteIntFile(const std::string &path, int value) {
	std::string s = std::to_string(value);
	WriteWholeFile(path, s.data(), s.size());
}

void LogCb(enum retro_log_level level, const char *fmt, ...) {
	char buffer[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);

#if defined(__ANDROID__)
	int prio = ANDROID_LOG_INFO;
	switch (level) {
	case RETRO_LOG_DEBUG: prio = ANDROID_LOG_DEBUG; break;
	case RETRO_LOG_INFO: prio = ANDROID_LOG_INFO; break;
	case RETRO_LOG_WARN: prio = ANDROID_LOG_WARN; break;
	case RETRO_LOG_ERROR: prio = ANDROID_LOG_ERROR; break;
	default: break;
	}
	__android_log_print(prio, "RetroFusionCore", "%s", buffer);
#else
	fprintf(stderr, "[core] %s", buffer);
#endif
}

bool RumbleSetState(unsigned port, enum retro_rumble_effect effect, uint16_t strength) {
	// ActiveInput() takes the host mutex itself; do not hold it here.
	InputSource *input = CoreHost::ActiveInput();
	if (input) {
		input->OnRumble(port, (unsigned)effect, strength);
	}
	return true;
}

// --- disk control stubs (multi-disc swap is a phase-2 feature) -------------
unsigned DiskGetImageIndex(void) { return 0; }
bool DiskSetImageIndex(unsigned index) { return index == 0; }
bool DiskSetEjectState(bool ejected) { return true; }
bool DiskGetEjectState(void) { return false; }
unsigned DiskGetImageCount(void) { return 1; }
bool DiskReplaceImageIndex(unsigned index, const struct retro_game_info *info) { return index == 0; }
bool DiskAddImageIndex(void) { return false; }

template <typename T>
bool BindSymbol(void *handle, const char *name, T *out) {
	void *sym = dlsym(handle, name);
	if (!sym) {
		return false;
	}
	*out = reinterpret_cast<T>(sym);
	return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Function table
// ---------------------------------------------------------------------------
struct CoreHost::CoreApi {
	void *handle = nullptr;
	unsigned (*api_version)(void) = nullptr;
	void (*init)(void) = nullptr;
	void (*deinit)(void) = nullptr;
	void (*set_environment)(retro_environment_t) = nullptr;
	void (*set_video_refresh)(retro_video_refresh_t) = nullptr;
	void (*set_audio_sample)(retro_audio_sample_t) = nullptr;
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t) = nullptr;
	void (*set_input_poll)(retro_input_poll_t) = nullptr;
	void (*set_input_state)(retro_input_state_t) = nullptr;
	void (*get_system_info)(struct retro_system_info *) = nullptr;
	void (*get_system_av_info)(struct retro_system_av_info *) = nullptr;
	void (*set_controller_port_device)(unsigned, unsigned) = nullptr;
	bool (*load_game)(const struct retro_game_info *) = nullptr;
	void (*run)(void) = nullptr;
	void (*reset)(void) = nullptr;
	void (*unload_game)(void) = nullptr;
	size_t (*serialize_size)(void) = nullptr;
	bool (*serialize)(void *, size_t) = nullptr;
	bool (*unserialize)(const void *, size_t) = nullptr;
	void *(*get_memory_data)(unsigned) = nullptr;
	size_t (*get_memory_size)(unsigned) = nullptr;
};

// ---------------------------------------------------------------------------
// ctor / dtor / configuration
// ---------------------------------------------------------------------------
CoreHost::CoreHost() : api_(new CoreApi()) {
	memset(&log_cb_, 0, sizeof(log_cb_));
	log_cb_.log = &LogCb;

	memset(&rumble_, 0, sizeof(rumble_));
	rumble_.set_rumble_state = &RumbleSetState;

	memset(&disk_control_, 0, sizeof(disk_control_));
	disk_control_.get_image_index = &DiskGetImageIndex;
	disk_control_.set_image_index = &DiskSetImageIndex;
	disk_control_.set_eject_state = &DiskSetEjectState;
	disk_control_.get_eject_state = &DiskGetEjectState;
	disk_control_.get_num_images = &DiskGetImageCount;
	disk_control_.replace_image_index = &DiskReplaceImageIndex;
	disk_control_.add_image_index = &DiskAddImageIndex;
}

CoreHost::~CoreHost() {
	Unload();
}

void CoreHost::SetSinks(VideoSink *video, AudioSink *audio, InputSource *input) {
	video_ = video;
	audio_ = audio;
	input_ = input;
}

void CoreHost::SetDirectories(const std::string &system_dir, const std::string &save_dir) {
	system_dir_ = system_dir;
	save_dir_ = save_dir;
	EnsureDirectory(system_dir_);
	EnsureDirectory(save_dir_);
}

void CoreHost::SetOptionOverrides(const std::map<std::string, std::string> &overrides) {
	option_overrides_ = overrides;
	option_active_ = overrides;
}

const char *PixelFormatName(PixelFormat f) {
	switch (f) {
	case PixelFormat::k0RGB1555: return "0RGB1555";
	case PixelFormat::kXRGB8888: return "XRGB8888";
	case PixelFormat::kRGB565: return "RGB565";
	}
	return "?";
}

std::string CoreHost::SramPath() const {
	if (game_path_.empty()) return "";
	return JoinPath(save_dir_, FileStem(game_path_) + ".srm");
}

std::string CoreHost::RtcPath() const {
	if (game_path_.empty()) return "";
	return JoinPath(save_dir_, FileStem(game_path_) + ".rtc");
}

std::string CoreHost::StatePath(int slot) const {
	std::string states = JoinPath(save_dir_, "states");
	EnsureDirectory(states);
	return JoinPath(states, FileStem(game_path_) + ".state" + std::to_string(slot));
}

// ---------------------------------------------------------------------------
// Load / Unload
// ---------------------------------------------------------------------------
LoadResult CoreHost::Load(const std::string &core_id, const std::string &core_path,
                          const std::string &game_path, std::string *error) {
	auto fail = [&](LoadResult r, const std::string &msg) {
		last_error_ = msg;
		if (error) *error = msg;
		Unload();
		return r;
	};

	Unload();

	if (IsQuarantined(core_id)) {
		return fail(LoadResult::kQuarantined, core_id + " crashed twice on this device and was disabled.");
	}

	core_id_ = core_id;
	core_path_ = core_path;
	game_path_ = game_path;
	requested_shutdown_ = false;

	api_->handle = dlopen(core_path.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!api_->handle) {
		const char *err = dlerror();
		return fail(LoadResult::kCoreMissing, "Cannot open " + core_path + ": " + (err ? err : "?"));
	}

	bool ok = true;
	ok &= BindSymbol(api_->handle, "retro_api_version", &api_->api_version);
	ok &= BindSymbol(api_->handle, "retro_init", &api_->init);
	ok &= BindSymbol(api_->handle, "retro_deinit", &api_->deinit);
	ok &= BindSymbol(api_->handle, "retro_set_environment", &api_->set_environment);
	ok &= BindSymbol(api_->handle, "retro_set_video_refresh", &api_->set_video_refresh);
	ok &= BindSymbol(api_->handle, "retro_set_audio_sample", &api_->set_audio_sample);
	ok &= BindSymbol(api_->handle, "retro_set_audio_sample_batch", &api_->set_audio_sample_batch);
	ok &= BindSymbol(api_->handle, "retro_set_input_poll", &api_->set_input_poll);
	ok &= BindSymbol(api_->handle, "retro_set_input_state", &api_->set_input_state);
	ok &= BindSymbol(api_->handle, "retro_get_system_info", &api_->get_system_info);
	ok &= BindSymbol(api_->handle, "retro_get_system_av_info", &api_->get_system_av_info);
	ok &= BindSymbol(api_->handle, "retro_set_controller_port_device", &api_->set_controller_port_device);
	ok &= BindSymbol(api_->handle, "retro_load_game", &api_->load_game);
	ok &= BindSymbol(api_->handle, "retro_run", &api_->run);
	ok &= BindSymbol(api_->handle, "retro_reset", &api_->reset);
	ok &= BindSymbol(api_->handle, "retro_unload_game", &api_->unload_game);
	ok &= BindSymbol(api_->handle, "retro_serialize_size", &api_->serialize_size);
	ok &= BindSymbol(api_->handle, "retro_serialize", &api_->serialize);
	ok &= BindSymbol(api_->handle, "retro_unserialize", &api_->unserialize);
	ok &= BindSymbol(api_->handle, "retro_get_memory_data", &api_->get_memory_data);
	ok &= BindSymbol(api_->handle, "retro_get_memory_size", &api_->get_memory_size);

	if (!ok) {
		return fail(LoadResult::kCoreRejected, "Incomplete libretro core: " + core_path);
	}
	if (api_->api_version() != RETRO_API_VERSION) {
		char buf[128];
		snprintf(buf, sizeof(buf), "Core reports libretro API v%u, frontend requires v%u.",
		         api_->api_version(), (unsigned)RETRO_API_VERSION);
		return fail(LoadResult::kCoreRejected, buf);
	}

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		s_active = this;
	}

	api_->set_environment(&CoreHost::EnvironmentCb);
	api_->set_video_refresh(&CoreHost::VideoRefreshCb);
	api_->set_audio_sample(&CoreHost::AudioSampleCb);
	api_->set_audio_sample_batch(&CoreHost::AudioBatchCb);
	api_->set_input_poll(&CoreHost::InputPollCb);
	api_->set_input_state(&CoreHost::InputStateCb);

	struct retro_system_info sys_info;
	memset(&sys_info, 0, sizeof(sys_info));
	api_->get_system_info(&sys_info);
	core_name_ = sys_info.library_name ? sys_info.library_name : core_id;
	core_version_ = sys_info.library_version ? sys_info.library_version : "";

	api_->init();

	// --- content ---------------------------------------------------------
	struct retro_game_info info;
	memset(&info, 0, sizeof(info));
	info.path = game_path_.c_str();
	std::vector<char> content;
	if (sys_info.need_fullpath) {
		info.data = nullptr;
		info.size = 0;
	} else {
		if (!ReadWholeFile(game_path_, &content)) {
			return fail(LoadResult::kGameUnsupported, "Cannot read " + game_path_);
		}
		info.data = content.data();
		info.size = content.size();
	}

	if (!api_->load_game(&info)) {
		return fail(LoadResult::kGameUnsupported,
		            core_name_ + " rejected this file. The extension may not match the real format.");
	}

	api_->set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

	struct retro_system_av_info av;
	memset(&av, 0, sizeof(av));
	api_->get_system_av_info(&av);
	av_info_.base_width = av.geometry.base_width;
	av_info_.base_height = av.geometry.base_height;
	av_info_.max_width = av.geometry.max_width ? av.geometry.max_width : av.geometry.base_width;
	av_info_.max_height = av.geometry.max_height ? av.geometry.max_height : av.geometry.base_height;
	av_info_.fps = av.timing.fps > 0.01 ? av.timing.fps : 60.0;
	av_info_.sample_rate = av.timing.sample_rate > 0.0 ? av.timing.sample_rate : 44100.0;
	fps_ = av_info_.fps;

	if (api_->serialize_size) {
		serialize_size_ = api_->serialize_size();
	}

	// --- persistent memory ----------------------------------------------
	sram_size_ = api_->get_memory_size ? api_->get_memory_size(RETRO_MEMORY_SAVE_RAM) : 0;
	sram_ = sram_size_ ? api_->get_memory_data(RETRO_MEMORY_SAVE_RAM) : nullptr;
	rtc_size_ = api_->get_memory_size ? api_->get_memory_size(RETRO_MEMORY_RTC) : 0;
	rtc_ = rtc_size_ ? api_->get_memory_data(RETRO_MEMORY_RTC) : nullptr;

	if (sram_ && sram_size_) {
		std::vector<char> buf;
		if (ReadWholeFile(SramPath(), &buf) && buf.size() == sram_size_) {
			memcpy(sram_, buf.data(), sram_size_);
		}
	}
	if (rtc_ && rtc_size_) {
		std::vector<char> buf;
		if (ReadWholeFile(RtcPath(), &buf) && buf.size() == rtc_size_) {
			memcpy(rtc_, buf.data(), rtc_size_);
		}
	}

	loaded_ = true;
	ApplyOptionsToCore();

	// Drop the crash marker: if the process dies from here on, the next start
	// counts it against this core only.
	EnsureDirectory(g_health_dir);
	WriteIntFile(MarkerPath(core_id_), 1);
	return LoadResult::kOk;
}

void CoreHost::Unload() {
	if (api_ && api_->handle) {
		if (loaded_) {
			FlushSram();
			if (api_->unload_game) api_->unload_game();
		}
		if (api_->deinit) api_->deinit();
		dlclose(api_->handle);
	}
	if (api_) {
		*api_ = CoreApi();
	}
	if (loaded_ && !core_id_.empty()) {
		RemoveFile(MarkerPath(core_id_));
	}
	loaded_ = false;
	paused_ = false;
	sram_ = nullptr;
	sram_size_ = 0;
	rtc_ = nullptr;
	rtc_size_ = 0;
	serialize_size_ = 0;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (s_active == this) s_active = nullptr;
	}
}

bool CoreHost::RunFrame() {
	if (!loaded_ || !api_->run || requested_shutdown_) {
		return false;
	}
	if (input_) input_->BeginFrame();
	api_->run();
	if (++sram_flush_timer_ >= (int64_t)(5.0 * fps_)) {
		sram_flush_timer_ = 0;
		FlushSram();
	}
	return true;
}

void CoreHost::Reset() {
	if (loaded_ && api_->reset) api_->reset();
}

void CoreHost::FlushSram() {
	if (sram_ && sram_size_) {
		WriteWholeFile(SramPath(), sram_, sram_size_);
	}
	if (rtc_ && rtc_size_) {
		WriteWholeFile(RtcPath(), rtc_, rtc_size_);
	}
}

// ---------------------------------------------------------------------------
// Savestates
// ---------------------------------------------------------------------------
std::string CoreHost::StateFingerprint() const {
	std::string acc = core_id_ + "|" + std::to_string(serialize_size_);
	for (auto &kv : option_active_) {
		acc += "|" + kv.first + "=" + kv.second;
	}
	char buf[32];
	snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)Fnv1a64(acc));
	return buf;
}

bool CoreHost::SaveState(int slot, std::string *error) {
	if (!loaded_ || !serialize_size_ || !api_->serialize) {
		if (error) *error = "This core does not support savestates.";
		return false;
	}
	std::vector<char> data(serialize_size_);
	if (!api_->serialize(data.data(), data.size())) {
		if (error) *error = "Core failed to serialize state.";
		return false;
	}
	// Header: magic | fingerprint | size | payload. The fingerprint makes a
	// state load across incompatible option sets impossible instead of merely
	// unlikely.
	std::string fp = StateFingerprint();
	std::string out = "RFSTATE1";
	uint32_t fp_len = (uint32_t)fp.size();
	out.append((const char *)&fp_len, sizeof(fp_len));
	out.append(fp);
	uint32_t payload = (uint32_t)data.size();
	out.append((const char *)&payload, sizeof(payload));
	out.append(data.data(), data.size());
	if (!WriteWholeFile(StatePath(slot), out.data(), out.size())) {
		if (error) *error = "Cannot write state file.";
		return false;
	}
	return true;
}

bool CoreHost::LoadState(int slot, std::string *error) {
	std::vector<char> buf;
	if (!ReadWholeFile(StatePath(slot), &buf) || buf.size() < 12) {
		if (error) *error = "No savestate in this slot.";
		return false;
	}
	if (memcmp(buf.data(), "RFSTATE1", 8) != 0) {
		if (error) *error = "Savestate file is corrupt.";
		return false;
	}
	size_t off = 8;
	uint32_t fp_len = 0;
	memcpy(&fp_len, buf.data() + off, sizeof(fp_len));
	off += sizeof(fp_len);
	if (off + fp_len + sizeof(uint32_t) > buf.size()) {
		if (error) *error = "Savestate file is truncated.";
		return false;
	}
	std::string fp(buf.data() + off, fp_len);
	off += fp_len;
	if (fp != StateFingerprint()) {
		if (error) *error = "This savestate was made with different core settings.";
		return false;
	}
	uint32_t payload = 0;
	memcpy(&payload, buf.data() + off, sizeof(payload));
	off += sizeof(payload);
	if (off + payload > buf.size()) {
		if (error) *error = "Savestate payload is truncated.";
		return false;
	}
	if (!api_->unserialize(buf.data() + off, payload)) {
		if (error) *error = "Core rejected the savestate.";
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------
std::string CoreHost::GetOption(const std::string &key) const {
	auto it = option_active_.find(key);
	if (it != option_active_.end()) return it->second;
	for (const CoreOptionDefinition &def : option_defs_) {
		if (def.key == key) return def.default_value;
	}
	return "";
}

void CoreHost::SetOption(const std::string &key, const std::string &value) {
	option_active_[key] = value;
	options_dirty_ = true;
}

void CoreHost::ApplyOptionsToCore() {
	// Nothing to push: variables are pulled by the core through
	// RETRO_ENVIRONMENT_GET_VARIABLE, and the update flag makes it re-read them.
	options_dirty_ = true;
}

// ---------------------------------------------------------------------------
// Environment
// ---------------------------------------------------------------------------
bool CoreHost::EnvironmentCb(unsigned cmd, void *data) {
	CoreHost *host = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		host = s_active;
	}
	return host ? host->RunEnvironment(cmd, data) : false;
}

bool CoreHost::RunEnvironment(unsigned cmd, void *data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
		if (!data) return false;
		switch (*static_cast<const enum retro_pixel_format *>(data)) {
		case RETRO_PIXEL_FORMAT_0RGB1555: format_ = PixelFormat::k0RGB1555; break;
		case RETRO_PIXEL_FORMAT_XRGB8888: format_ = PixelFormat::kXRGB8888; break;
		case RETRO_PIXEL_FORMAT_RGB565: format_ = PixelFormat::kRGB565; break;
		default: format_ = PixelFormat::kRGB565; break;
		}
		return true;
	}
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		if (!data) return false;
		*static_cast<bool *>(data) = true;
		return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		if (!data) return false;
		*static_cast<const char **>(data) = system_dir_.c_str();
		return !system_dir_.empty();
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		if (!data) return false;
		*static_cast<const char **>(data) = save_dir_.c_str();
		return !save_dir_.empty();

	case RETRO_ENVIRONMENT_SET_VARIABLES: {
		const struct retro_variable *vars = static_cast<const struct retro_variable *>(data);
		if (!vars) return false;
		for (size_t i = 0; vars[i].key; ++i) {
			CoreOptionDefinition def;
			def.key = vars[i].key;
			std::string raw = vars[i].value ? vars[i].value : "";
			const size_t semi = raw.find(';');
			if (semi != std::string::npos) {
				def.label = Trim(raw.substr(0, semi));
				std::string rest = raw.substr(semi + 1);
				size_t start = 0;
				while (start <= rest.size()) {
					const size_t bar = rest.find('|', start);
					const std::string val = Trim(rest.substr(
						start, bar == std::string::npos ? std::string::npos : bar - start));
					if (!val.empty()) def.values.push_back(CoreOptionValue{val, val});
					if (bar == std::string::npos) break;
					start = bar + 1;
				}
			} else {
				def.label = def.key;
			}
			if (!def.values.empty()) def.default_value = def.values[0].value;
			def.hidden = true;
			option_defs_.push_back(def);
		}
		return true;
	}

	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable *var = static_cast<struct retro_variable *>(data);
		if (!var || !var->key) return false;
		auto it = option_active_.find(var->key);
		if (it != option_active_.end()) {
			var->value = it->second.c_str();
			return true;
		}
		// Fall back to the schema's potato profile, so options we did not list
		// explicitly still get a weak-device value rather than the core default.
		const CoreOptionSchema *schema = SchemaForCore(core_id_);
		if (schema) {
			for (size_t i = 0; i < schema->count; ++i) {
				if (var->key == std::string(schema->specs[i].key)) {
					var->value = schema->specs[i].potato_value;
					return var->value != nullptr;
				}
			}
		}
		var->value = nullptr;
		return false;
	}

	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		if (!data) return false;
		*static_cast<bool *>(data) = options_dirty_;
		options_dirty_ = false;
		return true;

	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
		if (!data) return false;
		*static_cast<unsigned *>(data) = 2;
		return true;

	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
		return ParseCoreOptionsV2(cmd, data);

	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
		if (data) supports_no_game_ = *static_cast<const bool *>(data);
		return true;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		if (!data) return false;
		*static_cast<struct retro_log_callback *>(data) = log_cb_;
		return true;
	case RETRO_ENVIRONMENT_GET_LANGUAGE:
		if (!data) return false;
		*static_cast<unsigned *>(data) = language_;
		return true;
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
		input_bitmasks_ = true;
		return true;
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
	case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
	case RETRO_ENVIRONMENT_SET_MESSAGE:
	case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
		return true;

	case RETRO_ENVIRONMENT_SET_GEOMETRY: {
		const struct retro_game_geometry *g = static_cast<const struct retro_game_geometry *>(data);
		if (!g) return false;
		av_info_.base_width = g->base_width;
		av_info_.base_height = g->base_height;
		av_info_.max_width = g->max_width ? g->max_width : g->base_width;
		av_info_.max_height = g->max_height ? g->max_height : g->base_height;
		if (video_) {
			video_->OnGeometryChanged(av_info_.base_width, av_info_.base_height,
			                          av_info_.max_width, av_info_.max_height);
		}
		return true;
	}
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: {
		const struct retro_system_av_info *av = static_cast<const struct retro_system_av_info *>(data);
		if (!av) return false;
		av_info_.fps = av->timing.fps > 0.01 ? av->timing.fps : av_info_.fps;
		av_info_.sample_rate = av->timing.sample_rate > 0.0 ? av->timing.sample_rate : av_info_.sample_rate;
		return true;
	}
	case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
		if (!data) return false;
		*static_cast<int *>(data) = av_enable_ ? 3 : 0;
		return true;
	case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
		if (!data) return false;
		*static_cast<struct retro_rumble_interface *>(data) = rumble_;
		return true;
	case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
		// Multi-disc swap is a phase-2 feature; handing out a valid (but
		// single-image) callback keeps multi-disc cores loading normally.
		if (!data) return false;
		*static_cast<struct retro_disk_control_callback *>(data) = disk_control_;
		return true;
	case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
	case RETRO_ENVIRONMENT_SET_HW_RENDER:
		// Phase 1 is software-only: refusing hardware render makes cores fall
		// back to their software path, which is also the fastest choice on weak
		// devices. A core that *only* has a GL path fails loudly here.
		last_error_ = "This core needs OpenGL; the software-only build cannot run it.";
		return false;
	case RETRO_ENVIRONMENT_SHUTDOWN:
		requested_shutdown_ = true;
		return true;
	case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:
		return false;
	case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
		if (!data) return false;
		*static_cast<unsigned *>(data) = 1;
		return true;
	default:
		return false;
	}
}

bool CoreHost::ParseCoreOptionsV2(unsigned cmd, void *data) {
	// v2 (with or without localisation, with or without categories)
	if (cmd == RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2 ||
	    cmd == RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL) {
		if (!data) return false;
		struct retro_core_options_v2 *intl = static_cast<struct retro_core_options_v2 *>(data);
		struct retro_core_options_v2 *opts = intl;
		if (cmd == RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL) {
			struct retro_core_options_v2_intl *wrap = static_cast<struct retro_core_options_v2_intl *>(data);
			opts = wrap->us ? wrap->us : wrap->local;
		}
		if (!opts || !opts->definitions) return false;
		for (size_t i = 0; opts->definitions[i].key; ++i) {
			const struct retro_core_option_v2_definition &src = opts->definitions[i];
			CoreOptionDefinition def;
			def.key = src.key;
			def.label = src.desc ? src.desc : src.key;
			def.info = src.info ? src.info : "";
			for (size_t v = 0; v < RETRO_NUM_CORE_OPTION_VALUES_MAX && src.values[v].value; ++v) {
				def.values.push_back(CoreOptionValue{
					src.values[v].label ? src.values[v].label : src.values[v].value,
					src.values[v].value});
			}
			def.default_value = src.default_value ? src.default_value : "";
			def.hidden = true;
			option_defs_.push_back(def);
		}
		return true;
	}
	if (cmd == RETRO_ENVIRONMENT_SET_CORE_OPTIONS) {
		const struct retro_core_option_definition *defs =
			static_cast<const struct retro_core_option_definition *>(data);
		if (!defs) return false;
		for (size_t i = 0; defs[i].key; ++i) {
			CoreOptionDefinition def;
			def.key = defs[i].key;
			def.label = defs[i].desc ? defs[i].desc : defs[i].key;
			def.info = defs[i].info ? defs[i].info : "";
			for (size_t v = 0; v < RETRO_NUM_CORE_OPTION_VALUES_MAX && defs[i].values[v].value; ++v) {
				def.values.push_back(CoreOptionValue{
					defs[i].values[v].label ? defs[i].values[v].label : defs[i].values[v].value,
					defs[i].values[v].value});
			}
			def.default_value = defs[i].default_value ? defs[i].default_value : "";
			def.hidden = true;
			option_defs_.push_back(def);
		}
		return true;
	}
	if (cmd == RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL) {
		const struct retro_core_options_intl *wrap = static_cast<const struct retro_core_options_intl *>(data);
		if (!wrap) return false;
		return ParseCoreOptionsV2(RETRO_ENVIRONMENT_SET_CORE_OPTIONS,
		                          wrap->us ? (void *)wrap->us : (void *)wrap->local);
	}
	return false;
}

// ---------------------------------------------------------------------------
// Host callbacks
// ---------------------------------------------------------------------------
void CoreHost::VideoRefreshCb(const void *data, unsigned width, unsigned height, size_t pitch) {
	if (!data) return;  // frame duplication allowed -> keep previous frame
	CoreHost *host = s_active;
	if (host && host->video_) {
		host->video_->OnVideoFrame(data, width, height, pitch);
	}
}

void CoreHost::AudioSampleCb(int16_t left, int16_t right) {
	const int16_t pair[2] = { left, right };
	CoreHost *host = s_active;
	if (host && host->audio_) host->audio_->OnAudioSamples(pair, 1);
}

size_t CoreHost::AudioBatchCb(const int16_t *data, size_t frames) {
	CoreHost *host = s_active;
	if (host && host->audio_ && data) host->audio_->OnAudioSamples(data, frames);
	return frames;
}

void CoreHost::InputPollCb(void) {
	CoreHost *host = s_active;
	if (host && host->input_) host->input_->BeginFrame();
}

int16_t CoreHost::InputStateCb(unsigned port, unsigned device, unsigned index, unsigned id) {
	CoreHost *host = s_active;
	if (host && host->input_) return host->input_->State(port, device, index, id);
	return 0;
}

// ---------------------------------------------------------------------------
// Health / quarantine
// ---------------------------------------------------------------------------
InputSource *CoreHost::ActiveInput() {
	std::lock_guard<std::mutex> lock(g_mutex);
	return s_active ? s_active->input_ : nullptr;
}

void CoreHost::SetHealthDirectory(const std::string &dir) {
	std::lock_guard<std::mutex> lock(g_mutex);
	g_health_dir = dir;
	EnsureDirectory(g_health_dir);
}

int CoreHost::CrashCount(const std::string &core_id) {
	return ReadIntFile(HealthPath(core_id));
}

bool CoreHost::IsQuarantined(const std::string &core_id) {
	return CrashCount(core_id) >= 2;
}

void CoreHost::ClearQuarantine(const std::string &core_id) {
	WriteIntFile(HealthPath(core_id), 0);
	RemoveFile(MarkerPath(core_id));
}

void CoreHost::ReconcileHealthAtStartup() {
	std::vector<std::string> files = ListDirectory(g_health_dir);
	for (const std::string &f : files) {
		const std::string name = FileName(f);
		const std::string ext = FileExtension(name);
		if (ext != "running") continue;
		const std::string core_id = name.substr(0, name.size() - strlen(".running"));
		const int count = ReadIntFile(HealthPath(core_id)) + 1;
		WriteIntFile(HealthPath(core_id), count);
		RemoveFile(f);
	}
}

}  // namespace rf
