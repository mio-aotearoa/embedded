// SPDX-License-Identifier: GPL-3.0-or-later
//
// LocalSend app page. Opens an HTTP receiver so peers can push files to the
// card, and discovers peers for sending. Progress is polled from an lv_timer
// so the UI task never blocks on the network or the card.
#include "localsend_app.h"

#include "AppPage.h"
#include "app_toast.h"
#include "i18n.h"
#include "localsend_client.h"

#include <Arduino.h>
#include <SD.h>
#include <WiFi.h>
#include <lvgl.h>

#include <stdio.h>
#include <string.h>

namespace
{
constexpr uint32_t kBg = 0x101418;
constexpr int kMaxFiles = 24;

// Row user-data: -1 = rescan, -2 = back to devices, 0..15 = peer, 100 + i = file i.
int rowTag(lv_event_t *e) { return (int)(intptr_t)lv_event_get_user_data(e); }
}  // namespace

LocalSendApp *LocalSendApp::s_active = nullptr;
char LocalSendApp::barTitle_[32];

void LocalSendApp::launch(void)
{
    if (s_active) return;
    LocalSendApp *app = new LocalSendApp();
    s_active = app;
    if (!app->open())
    {
        s_active = nullptr;
        delete app;
    }
}

bool LocalSendApp::isOpen(void) { return s_active != nullptr && s_active->root_ != nullptr; }

void LocalSendApp::destroyAsync(void *p) { delete static_cast<LocalSendApp *>(p); }

void LocalSendApp::dismiss(void)
{
    if (!s_active) return;
    LocalSendApp *app = s_active;
    s_active = nullptr;
    app->close();
    lv_async_call(destroyAsync, app);
}

LocalSendApp::~LocalSendApp() {}

bool LocalSendApp::open(void)
{
    const lv_coord_t w = lv_disp_get_hor_res(nullptr);
    const lv_coord_t h = appPageContentH();

    root_ = appPageCreateRoot(kBg);
    if (!root_) return false;

    snprintf(barTitle_, sizeof barTitle_, "%s", TR("LocalSend"));
    appPageBegin(barTitle_, &LocalSendApp::dismiss);

    const lv_coord_t bar = appPageBarCover();

    list_ = lv_list_create(root_);
    lv_obj_set_pos(list_, 0, bar + 2);
    lv_obj_set_size(list_, w, h - bar - 44);
    lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(list_, 0, LV_PART_MAIN);

    status_ = lv_label_create(root_);
    lv_obj_set_style_text_color(status_, lv_color_hex(0x9AA4AE), LV_PART_MAIN);
    lv_obj_set_width(status_, w - 8);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(status_, 4, h - 38);

    showPeers();
    if (WiFi.status() == WL_CONNECTED)
    {
        LocalSendClient::startReceiver();
        LocalSendClient::startScan();
        setStatus(TR("Ready to receive · searching..."));
    }
    else
    {
        notify(TR("Connect to Wi-Fi first"));
    }

    timer_ = lv_timer_create(tickCb, 500, nullptr);
    return true;
}

void LocalSendApp::close(void)
{
    LocalSendClient::cancelScan();
    LocalSendClient::cancelSend();
    LocalSendClient::stopReceiver();
    if (timer_)
    {
        lv_timer_del(timer_);
        timer_ = nullptr;
    }
    appPageEnd(&LocalSendApp::dismiss);
    appPageDeleteRootAsync(root_);
    root_ = nullptr;
    list_ = nullptr;
    status_ = nullptr;
}

void LocalSendApp::showPeers(void)
{
    filesMode_ = false;
    peerIndex_ = -1;
    lastPeers_ = -1;
    rebuild();
}

void LocalSendApp::showFiles(int peerIndex)
{
    peerIndex_ = peerIndex;
    filesMode_ = true;
    nameCount_ = 0;

    File dir = SD.open("/");
    if (dir)
    {
        while (nameCount_ < kMaxFiles)
        {
            File entry = dir.openNextFile();
            if (!entry) break;
            if (!entry.isDirectory())
            {
                const char *name = entry.name();
                if (name)
                {
                    while (*name == '/') name++;
                    snprintf(names_[nameCount_], sizeof names_[0], "%s", name);
                    if (names_[nameCount_][0]) nameCount_++;
                }
            }
            entry.close();
        }
        dir.close();
    }
    rebuild();
    if (nameCount_ == 0) setStatus(TR("No files on the card"));
}

void LocalSendApp::rebuild(void)
{
    if (!list_) return;
    lv_obj_clean(list_);

    if (!filesMode_)
    {
        LsPeer p;
        const int n = LocalSendClient::peerCount();
        for (int i = 0; i < n; i++)
        {
            if (!LocalSendClient::peer(i, p)) continue;
            char row[64];
            snprintf(row, sizeof row, "%s  \xE2\x80\x94  %s", p.alias, p.ip);
            lv_obj_t *btn = lv_list_add_btn(list_, nullptr, row);
            if (btn) lv_obj_add_event_cb(btn, rowCb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
        lv_obj_t *again = lv_list_add_btn(list_, LV_SYMBOL_REFRESH, TR("Search again"));
        if (again) lv_obj_add_event_cb(again, rowCb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
        lastPeers_ = n;
        return;
    }

    lv_obj_t *back = lv_list_add_btn(list_, LV_SYMBOL_LEFT, TR("Devices"));
    if (back) lv_obj_add_event_cb(back, rowCb, LV_EVENT_CLICKED, (void *)(intptr_t)-2);

    for (int i = 0; i < nameCount_; i++)
    {
        lv_obj_t *btn = lv_list_add_btn(list_, LV_SYMBOL_FILE, names_[i]);
        if (btn) lv_obj_add_event_cb(btn, rowCb, LV_EVENT_CLICKED, (void *)(intptr_t)(100 + i));
    }
}

void LocalSendApp::setStatus(const char *msg)
{
    if (status_) lv_label_set_text(status_, msg ? msg : "");
}

void LocalSendApp::notify(const char *msg, int ms)
{
    setStatus(msg);
    appToast(msg, ms);
}

void LocalSendApp::sendFile(const char *name)
{
    if (!name || !name[0]) return;
    char path[96];
    snprintf(path, sizeof path, "/%s", name);

    if (!LocalSendClient::beginSend(path, peerIndex_))
    {
        notify(TR("Cannot start the transfer"));
        return;
    }
    char msg[96];
    snprintf(msg, sizeof msg, TR("Sending %s..."), name);
    setStatus(msg);
}

void LocalSendApp::rowCb(lv_event_t *e)
{
    if (!s_active) return;
    const int tag = rowTag(e);

    if (tag == -2)
    {
        s_active->showPeers();
        return;
    }
    if (tag == -1)
    {
        if (WiFi.status() != WL_CONNECTED)
        {
            s_active->setStatus(TR("Connect to Wi-Fi first"));
            return;
        }
        LocalSendClient::startScan();
        s_active->setStatus(TR("Searching the network..."));
        return;
    }
    if (tag >= 100)
    {
        const int i = tag - 100;
        if (i >= 0 && i < s_active->nameCount_) s_active->sendFile(s_active->names_[i]);
        return;
    }
    s_active->showFiles(tag);
}

void LocalSendApp::tickCb(lv_timer_t *t)
{
    (void)t;
    LocalSendApp *app = s_active;
    if (!app) return;

    const LocalSendClient::RecvState &rx = LocalSendClient::recvState();
    if (rx.busy)
    {
        char msg[128];
        snprintf(msg, sizeof msg, "%s %s %u / %u KB", TR("Receiving"),
                 rx.fileName[0] ? rx.fileName : "…",
                 (unsigned)(rx.received / 1024), (unsigned)(rx.total / 1024));
        app->setStatus(msg);
        return;
    }
    if (rx.done)
    {
        if (rx.ok)
        {
            char msg[128];
            snprintf(msg, sizeof msg, "%s %s (%u KB) → /localsend/", TR("Received"),
                     rx.fileName[0] ? rx.fileName : "file",
                     (unsigned)(rx.total / 1024));
            app->notify(msg);
        }
        else
        {
            app->notify(rx.err[0] ? rx.err : TR("Receive failed"), 3200);
        }
        LocalSendClient::clearRecvDone();
        return;
    }

    if (!app->filesMode_)
    {
        const int n = LocalSendClient::peerCount();
        if (n != app->lastPeers_) app->rebuild();

        if (LocalSendClient::scanning())
        {
            char msg[96];
            const int done = LocalSendClient::scanProgress();
            if (done < 255)
            {
                snprintf(msg, sizeof msg, TR("Ready to receive · searching... %d/254"), done);
            }
            else
            {
                snprintf(msg, sizeof msg, "%s", TR("Ready to receive · finishing..."));
            }
            app->setStatus(msg);
        }
        else if (app->lastProgress_ != 255)
        {
            app->lastProgress_ = 255;
            char msg[128];
            if (n > 0)
            {
                snprintf(msg, sizeof msg, TR("Ready to receive · %d device(s) - tap to send"), n);
            }
            else
            {
                snprintf(msg, sizeof msg, "%s",
                         TR("Ready to receive. No send peers yet (HTTP LocalSend only)."));
            }
            app->setStatus(msg);
        }
        return;
    }

    const LocalSendClient::State &st = LocalSendClient::state();
    if (st.running)
    {
        char msg[128];
        snprintf(msg, sizeof msg, "%s %u / %u KB", TR("Sending"),
                 (unsigned)(st.sent / 1024), (unsigned)(st.total / 1024));
        app->setStatus(msg);
        return;
    }
    if (!st.done) return;

    if (st.ok)
    {
        char msg[96];
        snprintf(msg, sizeof msg, "%s %u KB", TR("Sent"), (unsigned)(st.total / 1024));
        app->notify(msg);
    }
    else
    {
        app->notify(st.err[0] ? st.err : TR("Transfer failed"), 3200);
    }
    LocalSendClient::clearDone();
}
