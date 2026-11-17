// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>

// esp32-ai model manager for the T-Deck Plus.
//
// The upstream model (slvDev/esp32-ai, TinyStories, 28.9M parameters at 4 bits)
// is 14.9 MB, which does not fit this board's flash layout, so it lives on the
// SD card and is fetched over Wi-Fi when it is missing. Text generation itself
// needs the upstream runtime, which reads the weights through a flash mapping;
// that port is not wired here, so this page manages the assets and says so.
class Esp32AiApp
{
  public:
    static void launch(void);
    static bool isOpen(void);
    static void dismiss(void);

  private:
    ~Esp32AiApp();
    bool open(void);
    void close(void);
    void refresh(void);
    void beginDownload(int fileIndex);
    void setStatus(const char *msg);

    static void rowCb(lv_event_t *e);
    static void tickCb(lv_timer_t *t);
    static void destroyAsync(void *p);

    static Esp32AiApp *s_active;
    static char barTitle_[32];

    lv_obj_t *root_ = nullptr;
    lv_obj_t *list_ = nullptr;
    lv_obj_t *status_ = nullptr;
    lv_timer_t *timer_ = nullptr;
    int pending_ = -1;
};
