// SPDX-License-Identifier: GPL-3.0-or-later
//
// Model assets for slvDev/esp32-ai on the SD card.
//
// Upstream pins these two files and their byte sizes (scripts/fetch_model.sh);
// the sizes are repeated here so a truncated download is refused instead of
// being reported as a ready model. The URLs are the release's Hugging Face
// resolve endpoints.
#include "esp32ai_app.h"

#include "AppPage.h"
#include "http_download.h"
#include "i18n.h"

#include <Arduino.h>
#include <SD.h>
#include <WiFi.h>
#include <lvgl.h>

#include <stdio.h>
#include <string.h>

namespace
{
constexpr uint32_t kBg = 0x101418;
constexpr const char *kSdMount = "/sd";
constexpr const char *kDir = "/esp32ai/tinystories";

struct ModelFile
{
    const char *name;
    uint64_t bytes;
    const char *url;
};

// Sizes from slvDev/esp32-ai scripts/fetch_model.sh (tinystories pins).
const ModelFile kFiles[] = {
    {"model.bin", 14912348u,
     "https://huggingface.co/slvDev/esp32-ai-tinystories/resolve/main/model.bin"},
    {"tokenizer.json", 1788896u,
     "https://huggingface.co/slvDev/esp32-ai-tinystories/resolve/main/tokenizer.json"},
};
constexpr int kFileCount = (int)(sizeof(kFiles) / sizeof(kFiles[0]));

void sdPath(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s%s/%s", kSdMount, kDir, name);
}

bool fileSize(const char *path, uint64_t &size)
{
    File f = SD.open(path, FILE_READ);
    if (!f || f.isDirectory())
    {
        if (f) f.close();
        return false;
    }
    size = (uint64_t)f.size();
    f.close();
    return true;
}

void humanSize(uint64_t bytes, char *out, size_t cap)
{
    if (bytes >= 1024ull * 1024ull)
    {
        snprintf(out, cap, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    }
    else if (bytes >= 1024ull)
    {
        snprintf(out, cap, "%.0f KB", (double)bytes / 1024.0);
    }
    else
    {
        snprintf(out, cap, "%u B", (unsigned)bytes);
    }
}
}  // namespace

Esp32AiApp *Esp32AiApp::s_active = nullptr;
char Esp32AiApp::barTitle_[32];

void Esp32AiApp::launch(void)
{
    if (s_active) return;
    Esp32AiApp *app = new Esp32AiApp();
    s_active = app;
    if (!app->open())
    {
        s_active = nullptr;
        delete app;
    }
}

bool Esp32AiApp::isOpen(void) { return s_active != nullptr && s_active->root_ != nullptr; }

void Esp32AiApp::destroyAsync(void *p) { delete static_cast<Esp32AiApp *>(p); }

void Esp32AiApp::dismiss(void)
{
    if (!s_active) return;
    Esp32AiApp *app = s_active;
    s_active = nullptr;
    app->close();
    lv_async_call(destroyAsync, app);
}

Esp32AiApp::~Esp32AiApp() {}

bool Esp32AiApp::open(void)
{
    const lv_coord_t w = lv_disp_get_hor_res(nullptr);
    const lv_coord_t h = appPageContentH();

    root_ = appPageCreateRoot(kBg);
    if (!root_) return false;

    snprintf(barTitle_, sizeof barTitle_, "%s", TR("ESP-AI"));
    appPageBegin(barTitle_, &Esp32AiApp::dismiss);

    // The tall "< ESP-AI" bar overlaps the top of the root; start below it.
    const lv_coord_t bar = appPageBarCover();

    list_ = lv_list_create(root_);
    lv_obj_set_pos(list_, 0, bar + 4);
    lv_obj_set_size(list_, w, h - bar - 48);
    lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(list_, 0, LV_PART_MAIN);

    status_ = lv_label_create(root_);
    lv_obj_set_style_text_color(status_, lv_color_hex(0x9AA4AE), LV_PART_MAIN);
    lv_obj_set_width(status_, w - 8);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(status_, 4, h - 40);

    refresh();
    return true;
}

void Esp32AiApp::close(void)
{
    httpDownloadCancel();
    if (timer_)
    {
        lv_timer_del(timer_);
        timer_ = nullptr;
    }
    appPageEnd(&Esp32AiApp::dismiss);
    appPageDeleteRootAsync(root_);
    root_ = nullptr;
    list_ = nullptr;
    status_ = nullptr;
    pending_ = -1;
}

void Esp32AiApp::setStatus(const char *msg)
{
    if (status_) lv_label_set_text(status_, msg ? msg : "");
}

void Esp32AiApp::refresh(void)
{
    if (!list_) return;
    lv_obj_clean(list_);

    for (int i = 0; i < kFileCount; i++)
    {
        char path[128];
        char row[128];
        char size[24];
        sdPath(path, sizeof path, kFiles[i].name);
        uint64_t have = 0;
        if (fileSize(path, have) && have == kFiles[i].bytes)
        {
            humanSize(have, size, sizeof size);
            snprintf(row, sizeof row, "%s\xE2\x80\x94 %s", kFiles[i].name, size);
        }
        else if (fileSize(path, have))
        {
            humanSize(have, size, sizeof size);
            snprintf(row, sizeof row, TR("%s is incomplete (%s) \xE2\x80\x94 tap to re-download"),
                     kFiles[i].name, size);
        }
        else
        {
            humanSize(kFiles[i].bytes, size, sizeof size);
            snprintf(row, sizeof row, TR("%s \xE2\x80\x94 download (%s)"), kFiles[i].name, size);
        }
        lv_obj_t *btn = lv_list_add_btn(list_, nullptr, row);
        if (btn) lv_obj_add_event_cb(btn, rowCb, LV_EVENT_CLICKED, (void *)(uintptr_t)(i + 1));
    }

    char hint[256];
    snprintf(hint, sizeof hint,
             "%s %s%s",
             WiFi.status() == WL_CONNECTED ? TR("Wi-Fi connected.") : TR("Wi-Fi is not connected."),
             TR("Files live at "), kDir);
    setStatus(hint);
}

void Esp32AiApp::rowCb(lv_event_t *e)
{
    if (!s_active) return;
    const int idx = (int)(uintptr_t)lv_event_get_user_data(e) - 1;
    if (idx < 0 || idx >= kFileCount) return;

    char path[128];
    uint64_t have = 0;
    sdPath(path, sizeof path, kFiles[idx].name);
    if (fileSize(path, have) && have == kFiles[idx].bytes)
    {
        s_active->setStatus(TR("Already on the card"));
        return;
    }
    s_active->beginDownload(idx);
}

void Esp32AiApp::beginDownload(int fileIndex)
{
    if (fileIndex < 0 || fileIndex >= kFileCount) return;
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
    SD.mkdir("/esp32ai");
    SD.mkdir(kDir);

    char path[128];
    sdPath(path, sizeof path, kFiles[fileIndex].name);
    if (!httpDownloadStart(kFiles[fileIndex].url, path, kFiles[fileIndex].bytes))
    {
        setStatus(TR("Cannot start the download"));
        return;
    }

    pending_ = fileIndex;
    if (!timer_) timer_ = lv_timer_create(tickCb, 400, nullptr);

    char msg[96];
    snprintf(msg, sizeof msg, TR("Downloading %s\xE2\x80\xA6"), kFiles[fileIndex].name);
    setStatus(msg);
}

void Esp32AiApp::tickCb(lv_timer_t *t)
{
    (void)t;
    Esp32AiApp *app = s_active;
    if (!app) return;
    const HttpDownloadState &st = httpDownloadState();

    if (st.running)
    {
        char msg[128];
        if (st.total)
        {
            snprintf(msg, sizeof msg, "%s %u%% (%u / %u KB)", TR("Downloading"),
                     (unsigned)((st.bytes * 100) / st.total), (unsigned)(st.bytes / 1024),
                     (unsigned)(st.total / 1024));
        }
        else
        {
            snprintf(msg, sizeof msg, "%s %u KB", TR("Downloading"), (unsigned)(st.bytes / 1024));
        }
        app->setStatus(msg);
        return;
    }

    if (!st.done) return;

    const bool ok = st.ok;
    char err[96];
    snprintf(err, sizeof err, "%s", st.err);
    const char *name = (app->pending_ >= 0 && app->pending_ < kFileCount)
                           ? kFiles[app->pending_].name
                           : "";
    app->pending_ = -1;
    httpDownloadClearDone();

    if (app->timer_)
    {
        lv_timer_del(app->timer_);
        app->timer_ = nullptr;
    }
    app->refresh();
    if (ok)
    {
        char msg[128];
        snprintf(msg, sizeof msg, TR("Saved %s to the card"), name);
        app->setStatus(msg);
    }
    else
    {
        app->setStatus(err[0] ? err : TR("Download failed"));
    }
}
