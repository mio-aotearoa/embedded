// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

// Render at the panel content size (1:1 pixels, no stretch). w/h are clamped
// to the engine max (320×240).
bool voxelHostBegin(int w, int h);
void voxelHostEnd(void);
bool voxelHostTick(void);
void voxelHostSetButton(bool pressed);

uint16_t* voxelHostFramebuffer(void);
int voxelHostWidth(void);
int voxelHostHeight(void);
