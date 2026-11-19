// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>

// LocalSend app page: receive onto the card while open; discover peers and send.
class LocalSendApp
{
  public:
    static void launch(void);
    static bool isOpen(void);
    static void dismiss(void);

  private:
    ~LocalSendApp();
    bool open(void);
    void close(void);
    void showPeers(void);
    void showFiles(int peerIndex);
    void rebuild(void);
    void sendFile(const char *name);
    void setStatus(const char *msg);
    void notify(const char *msg, int ms = 2400);

    static void rowCb(lv_event_t *e);
    static void tickCb(lv_timer_t *t);
    static void destroyAsync(void *p);

    static LocalSendApp *s_active;
    static char barTitle_[32];

    lv_obj_t *root_ = nullptr;
    lv_obj_t *list_ = nullptr;
    lv_obj_t *status_ = nullptr;
    lv_timer_t *timer_ = nullptr;

    bool filesMode_ = false;
    int peerIndex_ = -1;
    int lastPeers_ = -1;
    int lastProgress_ = -1;
    char names_[24][48] = {};
    int nameCount_ = 0;
};
