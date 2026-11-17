// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>

// Downloads a URL straight to the SD card over HTTP or HTTPS, using the same
// http_download worker as the Kiwix/ESP-AI pages (one transfer at a time,
// "<dest>.part" renamed only after the last byte).
class DownloadApp
{
  public:
    static void launch(void);
    static bool isOpen(void);
    static void dismiss(void);

  private:
    ~DownloadApp();
    bool open(void);
    void close(void);
    void startUrl(const char *url, const char *dir, const char *fixedName);
    void setStatus(const char *msg);
    void setBusyUi(bool busy);

    static void rowCb(lv_event_t *e);
    static void downloadCb(lv_event_t *e);
    static void cancelCb(lv_event_t *e);
    static void taCb(lv_event_t *e);
    static void tickCb(lv_timer_t *t);
    static void destroyAsync(void *p);

    static DownloadApp *s_active;
    static char barTitle_[32];

    lv_obj_t *root_ = nullptr;
    lv_obj_t *ta_ = nullptr;
    lv_obj_t *list_ = nullptr;
    lv_obj_t *status_ = nullptr;
    lv_obj_t *getBtn_ = nullptr;
    lv_obj_t *cancelBtn_ = nullptr;
    lv_timer_t *timer_ = nullptr;
};
