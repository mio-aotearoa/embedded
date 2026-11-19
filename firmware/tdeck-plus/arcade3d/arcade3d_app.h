// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>

// OutRun-style pseudo-3D racer (esp32s3-arcade-3d), as a wadamesh AppPage.
// Throttle is automatic; steer with swipe, trackball, or hold left/right half
// of the screen. Renders at the game's native 320×240 (1:1, no stretch) under
// a slim back bar so almost the whole panel is playfield.
class Arcade3dApp
{
  public:
    static void launch(void);
    static bool isOpen(void);
    static void dismiss(void);
    static void steer(int dx, int dy);  // UITask trackball → left/right hold

  private:
    ~Arcade3dApp();
    bool open(void);
    void close(void);

    static void timerCb(lv_timer_t* t);
    static void gestureCb(lv_event_t* e);
    static void touchCb(lv_event_t* e);
    static void destroyAsync(void* p);

    static Arcade3dApp* s_active;
    static char barTitle_[24];

    lv_obj_t* root_ = nullptr;
    lv_obj_t* canvas_ = nullptr;
    lv_obj_t* hint_ = nullptr;
    lv_color_t* game_buf_ = nullptr;  // native SCR_W×SCR_H RGB565
    lv_timer_t* timer_ = nullptr;
    bool left_ = false;
    bool right_ = false;
    uint32_t steerUntil_ = 0;
    uint32_t hintUntil_ = 0;
};
