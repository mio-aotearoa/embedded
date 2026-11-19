// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>

// lewtheegg/voxel-flight: procedural flyer rendered 1:1 at the panel content
// size. Upstream is autopilot-only; here hold left/right to bank, release for
// the built-in wander/city autopilot.
class VoxelFlightApp
{
  public:
    static void launch(void);
    static bool isOpen(void);
    static void dismiss(void);
    static void steer(int dx, int dy);  // trackball: dx banks L/R

  private:
    ~VoxelFlightApp();
    bool open(void);
    void close(void);

    static void timerCb(lv_timer_t* t);
    static void gestureCb(lv_event_t* e);
    static void touchCb(lv_event_t* e);
    static void destroyAsync(void* p);

    static VoxelFlightApp* s_active;
    static char barTitle_[24];

    lv_obj_t* root_ = nullptr;
    lv_obj_t* canvas_ = nullptr;
    lv_obj_t* hint_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    bool left_ = false;
    bool right_ = false;
    uint32_t steerUntil_ = 0;
    uint32_t hintUntil_ = 0;
};
