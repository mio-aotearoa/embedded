// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared L/R hold steering for the T-Deck games (OutRun, Voxel Flight): canvas
// touch halves, gesture swipe, trackball nudge expiry, and the top hint label.
#pragma once

#include <lvgl.h>

#include <stdint.h>

struct GameSteer
{
    lv_obj_t *canvas = nullptr;
    lv_obj_t *hint = nullptr;
    bool left = false;
    bool right = false;
    uint32_t steerUntil = 0;
    uint32_t hintUntil = 0;
    // Called whenever left/right change. Arcade: (L,R) bools; Voxel maps to -1/0/+1.
    void (*apply)(bool left, bool right, void *ctx) = nullptr;
    void *ctx = nullptr;
};

// Wire PRESS/PRESSING/RELEASED/PRESS_LOST on canvas and GESTURE on root.
void gameSteerBind(GameSteer *s, lv_obj_t *root, lv_obj_t *canvas,
                   void (*apply)(bool left, bool right, void *ctx), void *ctx);

// Trackball / external nudge: hold for holdMs then expire in gameSteerTick.
void gameSteerNudge(GameSteer *s, int dx, uint32_t holdMs = 280);

// Expire nudge when canvas not pressed; hide hint when due. Call from the frame timer.
void gameSteerTick(GameSteer *s);

// Create the shared yellow hint chip; sets s->hint and s->hintUntil.
lv_obj_t *gameSteerMakeHint(GameSteer *s, lv_obj_t *root, const char *text, uint32_t showMs);
