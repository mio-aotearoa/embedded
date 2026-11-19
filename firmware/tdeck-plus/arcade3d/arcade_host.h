// SPDX-License-Identifier: MIT
// Host API around the vendored OutRun sources (firmware/tdeck-plus/arcade3d/game).
#pragma once

#include <stdint.h>

// Bind the main 320x240 RGB565 framebuffer (LVGL canvas). Optional: pre-sized
// buffer; if null, the host allocates PSRAM sprites itself.
bool arcadeHostBegin(uint16_t* fb320x240);
void arcadeHostEnd(void);

// Left/right steer held while true (trackball / keys / on-screen). Throttle
// stays automatic; lateral control is player-only.
void arcadeHostSetSteer(bool left, bool right);

// Advance one frame; returns false if the host was not begun.
bool arcadeHostTick(void);

uint16_t* arcadeHostFramebuffer(void);
