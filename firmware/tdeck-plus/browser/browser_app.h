// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>

// Minimal web browser page: fetches a URL, parses HTML/CSS with the pinned
// TactileBrowser core (Lexbor) and draws the result with LVGL 8.3 widgets.
// The core is LVGL-agnostic (its RenderInterface has no lv_* calls), so the
// only platform code is the renderer and the HTTP fetch implemented here.
class BrowserApp
{
  public:
    static void launch(void);
    static bool isOpen(void);
    static void dismiss(void);

    // Called by the link handler and the URL bar.
    void navigate(const char *url);
    static void linkCb(lv_event_t *e);

  private:
    ~BrowserApp();
    bool open(void);
    void close(void);
    void renderPage(void);
    void setStatus(const char *msg);

    static void goCb(lv_event_t *e);
    static void taCb(lv_event_t *e);
    static void viewCb(lv_event_t *e);
    static void fetchTask(void *arg);
    static void tickCb(lv_timer_t *t);
    static void destroyAsync(void *p);

    static BrowserApp *s_active;
    static char barTitle_[32];

    lv_obj_t *root_ = nullptr;
    lv_obj_t *ta_ = nullptr;
    lv_obj_t *view_ = nullptr;
    lv_obj_t *status_ = nullptr;
    lv_timer_t *timer_ = nullptr;
};
