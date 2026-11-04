// RetroFusion - PPSSPP integration layer (glue)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// This is the ONLY place where RetroFusion touches PPSSPP. Everything else in
// native/corehost/ is portable and already covered by tests/.
//
// Three jobs:
//   1. routing   - decide whether a launch is PSP (untouched EmuScreen) or a
//                  libretro core (RFEmuScreen)
//   2. hosting   - one RFEmuScreen that runs a core frame, presents it and
//                  feeds input through PPSSPP's own ControlMapper
//   3. settings  - build the per-core tab *inside* PPSSPP's existing
//                  GameSettingsScreen, so Start -> Settings looks and behaves
//                  exactly like stock PPSSPP
//
// The patch set in native/patches/ wires these five entry points into the
// upstream tree; nothing here replaces or reimplements PSP behaviour.

#pragma once

#include <string>

class Path;
class Screen;
class ScreenManager;
namespace UI { class ViewGroup; }

// --- 1. routing -------------------------------------------------------------

// True when `path` is handled by a libretro core (i.e. not a PSP image).
bool RFIsForeignContent(const Path &path);

// Returns a newly allocated screen: the stock EmuScreen for PSP content, or an
// RFEmuScreen for everything else. Returns nullptr if the content could not be
// routed at all (caller shows the stock error dialog).
Screen *RFCreateEmulatorScreen(const Path &path, ScreenManager *screen_manager);

// --- 2. lifecycle -----------------------------------------------------------

// Initialises the core subsystem exactly once (option store, health dir,
// core library directory). Safe to call multiple times.
void RFInitCoreSystem();

// Core id currently running, or nullptr when the PSP core (or nothing) is
// active. Used by the settings patch to decide whether to add the core tab.
const char *RFRunningCoreId();

// --- 3. settings tab --------------------------------------------------------

// Fills `parent` with the PPSSPP-styled core settings section for the running
// core. No-op for PSP content. Called from GameSettingsScreen::CreateTabs().
void RFBuildCoreSettingsTab(UI::ViewGroup *parent, ScreenManager *screen_manager);

// Emulation pause/resume from the pause screen.
void RFPauseEmulation(bool paused);
bool RFEmulationPaused();
