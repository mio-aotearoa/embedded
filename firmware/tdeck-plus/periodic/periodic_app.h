// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>

// Periodic table app for the T-Deck Plus, adapted from NeoCalculator's
// PeriodicTableApp (El-EnderJ, GPL-3.0-or-later). The grid is custom-drawn in
// one LVGL object instead of 118 buttons; tapping a cell shows the element.
class PeriodicApp
{
  public:
    static void launch(void);
    static bool isOpen(void);
    static void dismiss(void);

  private:
    ~PeriodicApp();
    bool open(void);
    void close(void);
    void select(int z);
    void updateDetail(void);

    static void drawCb(lv_event_t *e);
    static void pressedCb(lv_event_t *e);
    static void destroyAsync(void *p);

    static PeriodicApp *s_active;
    static char barTitle_[32];

    lv_obj_t *root_ = nullptr;
    lv_obj_t *grid_ = nullptr;
    lv_obj_t *detail_ = nullptr;
    int selected_ = 1;
    // Row height actually used, fitted to the space below the status bar.
    lv_coord_t cellH_ = 19;
};
