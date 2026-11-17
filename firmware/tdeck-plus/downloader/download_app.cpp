// SPDX-License-Identifier: GPL-3.0-or-later
//
// On-device downloader: type a URL, pick a destination folder, watch the bytes
// land on the SD card. The transfer itself is firmware/tdeck-plus/common/
// http_download.cpp (HTTPClient GET, TLS setInsecure, .part + rename), so this
// page only owns the URL field, the keyboard, the presets and the progress line.
#include "download_app.h"

#include "AppPage.h"
#include "http_download.h"
#include "i18n.h"
#include "ime_ja.h"

#include <Arduino.h>
#include <SD.h>
#include <WiFi.h>
#include <lvgl.h>

#include <stdio.h>
#include <string.h>

namespace
{
constexpr uint32_t kBg = 0x101418;
constexpr const char *kGeneralDir = "/downloads";
constexpr const char *kImeDir = "/ime";

struct Preset
{
    const char *label;
    const char *url;
    const char *dir;
};

// The pinyin engine's source word lists, pinned to the submodule commit. The
// binary dictionary is built from these by scripts/pinyin-dict.sh; this page
// fetches the lists straight onto the card so a device without a desktop can
// at least hold the data.
const Preset kPresets[] = {
    {"Pinyin word list", "https://raw.githubusercontent.com/VanillaNahida/lvgl_pinyin_ime/"
                         "1e2aa50840c838ebfb51e6e4756153f0084d52c3/data/dict/"
                         "rawdict_utf16_65105_freq.txt",
     kImeDir},
    {"Pinyin valid chars", "https://raw.githubusercontent.com/VanillaNahida/lvgl_pinyin_ime/"
                           "1e2aa50840c838ebfb51e6e4756153f0084d52c3/data/dict/"
                           "valid_utf16.txt",
     kImeDir},
};
constexpr int kPresetCount = (int)(sizeof(kPresets) / sizeof(kPresets[0]));

// Basename of a URL, stripped of query/fragment and made filesystem-safe.
void fileNameFor(const char *url, char *out, size_t cap)
{
    const char *start = strrchr(url, '/');
    start = start ? start + 1 : url;
    size_t o = 0;
    for (const char *p = start; *p && *p != '?' && *p != '#' && o + 1 < cap; p++)
    {
        const char c = *p;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        out[o++] = ok ? c : '_';
    }
    out[o] = 0;
    if (o == 0) snprintf(out, cap, "download.bin");
}
}  // namespace

DownloadApp *DownloadApp::s_active = nullptr;
char DownloadApp::barTitle_[32];

void DownloadApp::launch(void)
{
    if (s_active) return;
    DownloadApp *app = new DownloadApp();
    s_active = app;
    if (!app->open())
    {
        s_active = nullptr;
        delete app;
    }
}

bool DownloadApp::isOpen(void) { return s_active != nullptr && s_active->root_ != nullptr; }

void DownloadApp::destroyAsync(void *p) { delete static_cast<DownloadApp *>(p); }

void DownloadApp::dismiss(void)
{
    if (!s_active) return;
    DownloadApp *app = s_active;
    s_active = nullptr;
    app->close();
    lv_async_call(destroyAsync, app);
}

DownloadApp::~DownloadApp() {}

bool DownloadApp::open(void)
{
    const lv_coord_t w = lv_disp_get_hor_res(nullptr);
    const lv_coord_t h = appPageContentH();

    root_ = appPageCreateRoot(kBg);
    if (!root_) return false;

    snprintf(barTitle_, sizeof barTitle_, "%s", TR("Download"));
    appPageBegin(barTitle_, &DownloadApp::dismiss);

    // The tall "< Download" bar overlaps the top of the root (see AppPage.h).
    const lv_coord_t bar = appPageBarCover();

    ta_ = lv_textarea_create(root_);
    lv_obj_set_size(ta_, w - 8, 30);
    lv_obj_set_pos(ta_, 4, bar + 2);
    lv_textarea_set_one_line(ta_, true);
    lv_textarea_set_max_length(ta_, 255);
    lv_textarea_set_placeholder_text(ta_, "https://...");
    lv_obj_add_event_cb(ta_, taCb, LV_EVENT_ALL, nullptr);

    getBtn_ = lv_btn_create(root_);
    lv_obj_set_size(getBtn_, 150, 28);
    lv_obj_set_pos(getBtn_, 4, bar + 36);
    lv_obj_add_event_cb(getBtn_, downloadCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *getLabel = lv_label_create(getBtn_);
    lv_label_set_text(getLabel, TR("Download to card"));
    lv_obj_center(getLabel);

    cancelBtn_ = lv_btn_create(root_);
    lv_obj_set_size(cancelBtn_, 150, 28);
    lv_obj_set_pos(cancelBtn_, 166, bar + 36);
    lv_obj_add_event_cb(cancelBtn_, cancelCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cancelLabel = lv_label_create(cancelBtn_);
    lv_label_set_text(cancelLabel, TR("Cancel"));
    lv_obj_center(cancelLabel);
    lv_obj_add_state(cancelBtn_, LV_STATE_DISABLED);

    list_ = lv_list_create(root_);
    lv_obj_set_size(list_, w, 78);
    lv_obj_set_pos(list_, 0, bar + 68);
    lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(list_, 0, LV_PART_MAIN);
    for (int i = 0; i < kPresetCount; i++)
    {
        lv_obj_t *btn = lv_list_add_btn(list_, LV_SYMBOL_DOWNLOAD, kPresets[i].label);
        if (btn) lv_obj_add_event_cb(btn, rowCb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    status_ = lv_label_create(root_);
    lv_obj_set_width(status_, w - 8);
    lv_obj_set_pos(status_, 4, h - 40);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(status_, lv_color_hex(0x9AA4AE), LV_PART_MAIN);

    // Physical keyboard only: bind the field, no on-screen keyboard.
    appPageBindTextField(ta_);

    if (WiFi.status() == WL_CONNECTED)
    {
        setStatus(TR("Type an http:// or https:// URL, then Download"));
    }
    else
    {
        setStatus(TR("Wi-Fi is not connected"));
    }

    timer_ = lv_timer_create(tickCb, 400, nullptr);
    return true;
}

void DownloadApp::close(void)
{
    httpDownloadCancel();
    if (timer_)
    {
        lv_timer_del(timer_);
        timer_ = nullptr;
    }
    appPageBindTextField(nullptr);      // the field is about to be deleted
    appPageEnd(&DownloadApp::dismiss);
    appPageDeleteRootAsync(root_);
    root_ = nullptr;
    ta_ = nullptr;
    list_ = nullptr;
    status_ = nullptr;
    getBtn_ = nullptr;
    cancelBtn_ = nullptr;
}

void DownloadApp::setStatus(const char *msg)
{
    if (status_) lv_label_set_text(status_, msg ? msg : "");
}

void DownloadApp::setBusyUi(bool busy)
{
    if (getBtn_)
    {
        if (busy) lv_obj_add_state(getBtn_, LV_STATE_DISABLED);
        else lv_obj_clear_state(getBtn_, LV_STATE_DISABLED);
    }
    if (cancelBtn_)
    {
        if (busy) lv_obj_clear_state(cancelBtn_, LV_STATE_DISABLED);
        else lv_obj_add_state(cancelBtn_, LV_STATE_DISABLED);
    }
}

void DownloadApp::startUrl(const char *url, const char *dir, const char *fixedName)
{
    if (!url || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0))
    {
        setStatus(TR("The URL must start with http:// or https://"));
        return;
    }
    if (httpDownloadBusy())
    {
        setStatus(TR("A download is already running"));
        return;
    }
    if (WiFi.status() != WL_CONNECTED)
    {
        setStatus(TR("Connect to Wi-Fi first"));
        return;
    }

    SD.mkdir(kGeneralDir);
    SD.mkdir(kImeDir);

    char name[48];
    if (fixedName && fixedName[0]) snprintf(name, sizeof name, "%s", fixedName);
    else fileNameFor(url, name, sizeof name);

    char dest[128];
    snprintf(dest, sizeof dest, "%s/%s", dir, name);

    if (!httpDownloadStart(url, dest, 0))
    {
        setStatus(TR("Cannot start the download"));
        return;
    }
    setBusyUi(true);
    char msg[128];
    snprintf(msg, sizeof msg, TR("Downloading to %s"), dest);
    setStatus(msg);
}

void DownloadApp::downloadCb(lv_event_t *e)
{
    (void)e;
    if (!s_active || !s_active->ta_) return;
    const char *url = lv_textarea_get_text(s_active->ta_);
    s_active->startUrl(url, kGeneralDir, nullptr);
}

void DownloadApp::cancelCb(lv_event_t *e)
{
    (void)e;
    httpDownloadCancel();
    if (s_active) s_active->setStatus(TR("Cancelling..."));
}

void DownloadApp::rowCb(lv_event_t *e)
{
    if (!s_active) return;
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= kPresetCount) return;

    const Preset &p = kPresets[idx];
    char name[48];
    fileNameFor(p.url, name, sizeof name);
    s_active->startUrl(p.url, p.dir, name);
}

void DownloadApp::taCb(lv_event_t *e)
{
    if (!s_active) return;
    DownloadApp *app = s_active;
    const lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_LONG_PRESSED)
    {
        imeJaToggle();
        app->setStatus(imeJaActive() ? TR("Japanese: romaji, Space = kana")
                                     : TR("Type an http:// or https:// URL, then Download"));
        return;
    }
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED)
    {
        appPageBindTextField(app->ta_);   // physical keys type here
        return;
    }
    if (code == LV_EVENT_READY)           // Enter = press Download
    {
        if (app->getBtn_) lv_event_send(app->getBtn_, LV_EVENT_CLICKED, nullptr);
        return;
    }
    if (code == LV_EVENT_CANCEL)
    {
        appPageBindTextField(nullptr);
    }
}

void DownloadApp::tickCb(lv_timer_t *t)
{
    (void)t;
    DownloadApp *app = s_active;
    if (!app) return;

    const HttpDownloadState &st = httpDownloadState();
    if (st.running)
    {
        char msg[160];
        if (st.total)
        {
            snprintf(msg, sizeof msg, "%s %u%% (%u / %u KB)", TR("Downloading"),
                     (unsigned)((st.bytes * 100) / st.total), (unsigned)(st.bytes / 1024),
                     (unsigned)(st.total / 1024));
        }
        else
        {
            snprintf(msg, sizeof msg, "%s %u KB", TR("Downloading"),
                     (unsigned)(st.bytes / 1024));
        }
        app->setStatus(msg);
        return;
    }
    if (!st.done) return;

    const bool ok = st.ok;
    char err[96];
    snprintf(err, sizeof err, "%s", st.err);
    char path[128];
    snprintf(path, sizeof path, "%s", st.path);
    httpDownloadClearDone();
    app->setBusyUi(false);

    if (ok)
    {
        char msg[160];
        snprintf(msg, sizeof msg, TR("Saved %s"), path);
        app->setStatus(msg);
    }
    else
    {
        app->setStatus(err[0] ? err : TR("Download failed"));
    }
}
