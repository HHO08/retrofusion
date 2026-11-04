/* RetroFusion - minimal libretro core used to test the host layer on the build
 * machine (x86_64), no NDK or device required.
 *
 * It deliberately exercises every host path we rely on:
 *   - SET_PIXEL_FORMAT negotiation
 *   - GET_VARIABLE (core option) delivery, encoded into the frame buffer so the
 *     test can prove the potato profile actually reached the core
 *   - video refresh (RGB565), audio batch, input state polling
 *   - serialize / unserialize
 *   - SAVE_RAM block
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dummy_core.h"

static retro_environment_t env_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_cb;
static retro_input_poll_t poll_cb;
static retro_input_state_t state_cb;

static uint16_t frame[320 * 240];
static int16_t audio[2 * 735];
static uint8_t sram[0x2000];
static uint32_t run_count;
static uint32_t checksum;

void retro_init(void) {}
void retro_deinit(void) {}
unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_set_environment(retro_environment_t cb) { env_cb = cb; }
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }  /* host uses the batch path */
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { state_cb = cb; }

void retro_get_system_info(struct retro_system_info *info) {
	memset(info, 0, sizeof(*info));
	info->library_name = "DummyCore";
	info->library_version = "1.0";
	info->valid_extensions = "dmy";
	info->need_fullpath = false;
	info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info) {
	memset(info, 0, sizeof(*info));
	info->geometry.base_width = 320;
	info->geometry.base_height = 240;
	info->geometry.max_width = 320;
	info->geometry.max_height = 240;
	info->geometry.aspect_ratio = 4.0f / 3.0f;
	info->timing.fps = 59.94;
	info->timing.sample_rate = 44100.0;
}

void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }

bool retro_load_game(const struct retro_game_info *game) {
	enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
	if (!env_cb || !env_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
		return false;
	}
	if (!game) return false;
	return true;
}

void retro_unload_game(void) {}
void retro_reset(void) { run_count = 0; }

void retro_run(void) {
	struct retro_variable var;
	uint16_t option_code = 0;

	/* Prove the option profile reaches the core. */
	memset(&var, 0, sizeof(var));
	var.key = "dummy_option";
	var.value = NULL;
	if (env_cb && env_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
		option_code = (uint16_t)(var.value[0]);
	}

	/* Prove input state passthrough. */
	uint16_t pressed = 0x0000;
	if (poll_cb) poll_cb();
	if (state_cb) {
		pressed = (uint16_t)state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
	}

	for (int i = 0; i < 320 * 240; ++i) {
		frame[i] = (uint16_t)((option_code & 0xFF) | ((pressed & 0xFF) << 8));
	}
	for (int i = 0; i < 735; ++i) {
		audio[i * 2 + 0] = (int16_t)(run_count & 0x7FFF);
		audio[i * 2 + 1] = (int16_t)(run_count & 0x7FFF);
	}

	++run_count;
	checksum += option_code + pressed;

	if (video_cb) video_cb(frame, 320, 240, 320 * sizeof(uint16_t));
	if (audio_cb) audio_cb(audio, 735);
}

size_t retro_serialize_size(void) { return 16; }

bool retro_serialize(void *data, size_t size) {
	if (size < 16) return false;
	memcpy(data, &run_count, 4);
	memcpy((char *)data + 4, &checksum, 4);
	memcpy((char *)data + 8, sram, 8);
	return true;
}

bool retro_unserialize(const void *data, size_t size) {
	if (size < 16) return false;
	memcpy(&run_count, data, 4);
	memcpy(&checksum, (const char *)data + 4, 4);
	return true;
}

uint32_t retro_dummy_run_count(void) { return run_count; }

void *retro_get_memory_data(unsigned id) {
	return id == RETRO_MEMORY_SAVE_RAM ? sram : NULL;
}

size_t retro_get_memory_size(unsigned id) {
	return id == RETRO_MEMORY_SAVE_RAM ? sizeof(sram) : 0;
}
