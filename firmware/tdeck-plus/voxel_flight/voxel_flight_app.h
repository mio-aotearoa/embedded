// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>

// lewtheegg/voxel-flight: procedural flyer rendered 1:1 at the panel content
// size (no nearest-neighbour stretch).
class VoxelFlightApp
{
  public:
    static void launch(void);
    static bool isOpen(void);
    static void dismiss(void);
    static void steer(int dx, int dy);  // any nudge = "button" for future use

  private:
    ~VoxelFlightApp();
    bool open(void);
    void close(void);

    static void timerCb(lv_timer_t* t);
    static void destroyAsync(void* p);

    static VoxelFlightApp* s_active;
    static char barTitle_[24];

    lv_obj_t* root_ = nullptr;
    lv_obj_t* canvas_ = nullptr;
    lv_timer_t* timer_ = nullptr;
};
