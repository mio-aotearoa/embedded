// SPDX-License-Identifier: GPL-3.0-or-later
//
// Model assets for slvDev/esp32-ai on the SD card, plus a chat UI that runs the
// real TinyStories runtime once both files are present at the pinned sizes.
#include "esp32ai_app.h"

#include "AppPage.h"
#include "app_toast.h"
#include "cpu_boost.h"
#include "http_download.h"
#include "i18n.h"
#include "llm_port.h"
#include "task_spawn.h"

#include <Arduino.h>
#include <SD.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include <stdio.h>
#include <string.h>

namespace
{
constexpr uint32_t kBg = 0x101418;
// SD.open/mkdir paths are relative to the card root (same as Download/Kiwix).
constexpr const char *kDir = "/esp32ai/tinystories";

struct ModelFile
{
    const char *name;
    uint64_t bytes;
    const char *url;
};

const ModelFile kFiles[] = {
    {"model.bin", 14912348u,
     "https://huggingface.co/slvDev/esp32-ai-tinystories/resolve/main/model.bin"},
    {"tokenizer.json", 1788896u,
     "https://huggingface.co/slvDev/esp32-ai-tinystories/resolve/main/tokenizer.json"},
};
constexpr int kFileCount = (int)(sizeof(kFiles) / sizeof(kFiles[0]));

void sdPath(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s", kDir, name);
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
        snprintf(out, cap, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024ull)
        snprintf(out, cap, "%.0f KB", (double)bytes / 1024.0);
    else
        snprintf(out, cap, "%u B", (unsigned)bytes);
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
    cpuBoostAcquire();  // TinyStories matmul; restore on close/dismiss

    const lv_coord_t w = lv_disp_get_hor_res(nullptr);
    const lv_coord_t h = appPageContentH();

    root_ = appPageCreateRoot(kBg);
    if (!root_)
    {
        cpuBoostRelease();
        return false;
    }

    snprintf(barTitle_, sizeof barTitle_, "%s", TR("ESP-AI"));
    appPageBegin(barTitle_, &Esp32AiApp::dismiss);

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
    (void)httpDownloadWarmup();
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
    appPageBindTextField(nullptr);
    if (genHandle_)
    {
        vTaskDelete(genHandle_);
        genHandle_ = nullptr;
        // Idle task must reclaim the static TCB before the buffers are freed.
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    esp32aiUnload();
    if (genMem_.stack)
    {
        heap_caps_free(genMem_.stack);
        genMem_.stack = nullptr;
        genMem_.stackBytes = 0;
    }
    if (genMem_.tcb)
    {
        heap_caps_free(genMem_.tcb);
        genMem_.tcb = nullptr;
    }
    appPageEnd(&Esp32AiApp::dismiss);
    appPageDeleteRootAsync(root_);
    root_ = nullptr;
    list_ = nullptr;
    chat_ = nullptr;
    status_ = nullptr;
    ta_ = nullptr;
    sendBtn_ = nullptr;
    streamBody_ = nullptr;
    pending_ = -1;
    chatMode_ = false;
    cpuBoostRelease();
}

void Esp32AiApp::setStatus(const char *msg)
{
    if (status_) lv_label_set_text(status_, msg ? msg : "");
}

void Esp32AiApp::notify(const char *msg, int ms)
{
    setStatus(msg);
    appToast(msg, ms);
}

void Esp32AiApp::ensureChatPane(void)
{
    if (list_) lv_obj_add_flag(list_, LV_OBJ_FLAG_HIDDEN);
    if (chat_) return;

    const lv_coord_t w = lv_disp_get_hor_res(nullptr);
    const lv_coord_t h = appPageContentH();
    const lv_coord_t bar = appPageBarCover();
    chat_ = lv_obj_create(root_);
    lv_obj_set_pos(chat_, 0, bar + 4);
    lv_obj_set_size(chat_, w, h - bar - 78);
    lv_obj_set_style_bg_opa(chat_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(chat_, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chat_, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(chat_, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(chat_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(chat_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(chat_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(chat_, LV_SCROLLBAR_MODE_AUTO);
}

lv_obj_t *Esp32AiApp::addChatMessage(const char *who, const char *text, bool streaming)
{
    ensureChatPane();
    if (!chat_) return nullptr;

    const lv_coord_t inner = lv_obj_get_content_width(chat_);
    lv_obj_t *block = lv_obj_create(chat_);
    lv_obj_set_width(block, inner > 0 ? inner : (lv_disp_get_hor_res(nullptr) - 12));
    lv_obj_set_height(block, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(block, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(block, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(block, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(block, 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(block, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *whoLab = lv_label_create(block);
    lv_label_set_text(whoLab, who && who[0] ? who : "?");
    lv_obj_set_style_text_color(whoLab, lv_color_hex(0x7A8794), LV_PART_MAIN);

    lv_obj_t *body = lv_label_create(block);
    lv_obj_set_width(body, lv_pct(100));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(body, lv_color_hex(0xE8EEF4), LV_PART_MAIN);
    if (streaming && (!text || !text[0]))
        lv_label_set_text(body, "\xE2\x80\xA6");
    else
        lv_label_set_text(body, text ? text : "");

    lv_obj_scroll_to_view(block, LV_ANIM_OFF);
    return body;
}

void Esp32AiApp::refreshAssistantRow(void)
{
    if (!streamBody_) return;
    lv_label_set_text(streamBody_, genOut_[0] ? genOut_ : "\xE2\x80\xA6");
    lv_obj_scroll_to_view(lv_obj_get_parent(streamBody_), LV_ANIM_OFF);
}

void Esp32AiApp::showDownload(void)
{
    chatMode_ = false;
    streamBody_ = nullptr;
    if (chat_)
    {
        lv_obj_add_flag(chat_, LV_OBJ_FLAG_HIDDEN);
    }
    if (ta_)
    {
        appPageBindTextField(nullptr);
        lv_obj_del(ta_);
        ta_ = nullptr;
    }
    if (sendBtn_)
    {
        lv_obj_del(sendBtn_);
        sendBtn_ = nullptr;
    }

    const lv_coord_t w = lv_disp_get_hor_res(nullptr);
    const lv_coord_t h = appPageContentH();
    const lv_coord_t bar = appPageBarCover();
    if (list_)
    {
        lv_obj_clear_flag(list_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(list_, 0, bar + 4);
        lv_obj_set_size(list_, w, h - bar - 48);
        lv_obj_clean(list_);
    }

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
    snprintf(hint, sizeof hint, "%s %s%s",
             WiFi.status() == WL_CONNECTED ? TR("Wi-Fi connected.") : TR("Wi-Fi is not connected."),
             TR("Files live at "), kDir);
    setStatus(hint);
}

void Esp32AiApp::showChat(void)
{
    chatMode_ = true;
    const lv_coord_t w = lv_disp_get_hor_res(nullptr);
    const lv_coord_t h = appPageContentH();

    ensureChatPane();
    if (chat_)
    {
        lv_obj_clear_flag(chat_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clean(chat_);
    }
    streamBody_ = nullptr;

    if (!ta_)
    {
        ta_ = lv_textarea_create(root_);
        lv_obj_set_size(ta_, w - 88, 28);
        lv_obj_set_pos(ta_, 4, h - 72);
        lv_textarea_set_one_line(ta_, true);
        lv_textarea_set_max_length(ta_, 200);
        lv_textarea_set_placeholder_text(ta_, TR("Type a message"));
        lv_obj_add_event_cb(ta_, taCb, LV_EVENT_ALL, nullptr);
    }
    if (!sendBtn_)
    {
        sendBtn_ = lv_btn_create(root_);
        lv_obj_set_size(sendBtn_, 72, 28);
        lv_obj_set_pos(sendBtn_, w - 80, h - 72);
        lv_obj_add_event_cb(sendBtn_, sendCb, LV_EVENT_CLICKED, nullptr);
        lv_obj_t *lab = lv_label_create(sendBtn_);
        lv_label_set_text(lab, TR("Send"));
        lv_obj_center(lab);
    }

    appPageBindTextField(ta_);
    addChatMessage("ESP-AI", TR("Chat ready. Type a message (ASCII)."), false);
    setStatus(TR("On-device TinyStories \xE2\x80\x94 replies stream live"));

    // Reserve + spawn before EnsureLoaded eats internal SRAM for activations.
    if (!genHandle_)
    {
        static const uint32_t kSizes[] = {12 * 1024, 10 * 1024, 8 * 1024};
        if (!staticTaskSpawn(&genHandle_, &genMem_, genTask, "esp32ai-gen", this, 2, 0, kSizes,
                             sizeof kSizes / sizeof kSizes[0], "esp32ai"))
        {
            notify(TR("No RAM for generation task"));
            return;
        }
    }

    char err[96];
    if (!esp32aiEnsureLoaded(err, sizeof err))
    {
        notify(err[0] ? err : TR("Failed to load the model"));
    }
}

void Esp32AiApp::refresh(void)
{
    if (!list_) return;
    if (esp32aiModelReady()) showChat();
    else showDownload();
}

void Esp32AiApp::rowCb(lv_event_t *e)
{
    if (!s_active || s_active->chatMode_) return;
    const int idx = (int)(uintptr_t)lv_event_get_user_data(e) - 1;
    if (idx < 0 || idx >= kFileCount) return;

    char path[128];
    uint64_t have = 0;
    sdPath(path, sizeof path, kFiles[idx].name);
    if (fileSize(path, have) && have == kFiles[idx].bytes)
    {
        s_active->notify(TR("Already on the card"), 1600);
        return;
    }
    s_active->beginDownload(idx);
}

void Esp32AiApp::beginDownload(int fileIndex)
{
    if (fileIndex < 0 || fileIndex >= kFileCount) return;
    if (httpDownloadBusy())
    {
        notify(TR("A download is already running"));
        return;
    }
    if (WiFi.status() != WL_CONNECTED)
    {
        notify(TR("Connect to Wi-Fi first"));
        return;
    }
    SD.mkdir("/esp32ai");
    SD.mkdir(kDir);

    char path[192];
    sdPath(path, sizeof path, kFiles[fileIndex].name);
    if (!httpDownloadStart(kFiles[fileIndex].url, path, kFiles[fileIndex].bytes))
    {
        const HttpDownloadState &st = httpDownloadState();
        notify(st.err[0] ? st.err : TR("Cannot start the download"));
        return;
    }

    pending_ = fileIndex;
    if (!timer_) timer_ = lv_timer_create(tickCb, 400, nullptr);

    char msg[96];
    snprintf(msg, sizeof msg, TR("Downloading %s\xE2\x80\xA6"), kFiles[fileIndex].name);
    notify(msg, 1600);
}

void Esp32AiApp::taCb(lv_event_t *e)
{
    if (!s_active || !s_active->chatMode_) return;
    if (lv_event_get_code(e) == LV_EVENT_READY) s_active->startGenerate();
}

void Esp32AiApp::sendCb(lv_event_t *e)
{
    (void)e;
    if (s_active) s_active->startGenerate();
}

void Esp32AiApp::startGenerate(void)
{
    if (!chatMode_ || !ta_ || genBusy_) return;
    const char *text = lv_textarea_get_text(ta_);
    if (!text || !text[0])
    {
        setStatus(TR("Type a prompt first"));
        return;
    }
    snprintf(prompt_, sizeof prompt_, "%s", text);
    lv_textarea_set_text(ta_, "");
    addChatMessage("You", prompt_, false);
    setStatus(TR("Replying\xE2\x80\xA6"));
    genBusy_ = true;
    genDone_ = false;
    genResult_ = 0;
    genErr_[0] = 0;
    genOut_[0] = 0;
    genOutSeq_ = 0;
    genOutShown_ = 0;
    streamBody_ = addChatMessage("ESP-AI", "", true);
    if (sendBtn_) lv_obj_add_state(sendBtn_, LV_STATE_DISABLED);
    // Poll often so streamed tokens paint while the worker runs.
    if (!timer_) timer_ = lv_timer_create(tickCb, 100, nullptr);
    else lv_timer_set_period(timer_, 100);
    if (!genHandle_)
    {
        genBusy_ = false;
        setStatus(TR("Cannot start generation"));
        if (sendBtn_) lv_obj_clear_state(sendBtn_, LV_STATE_DISABLED);
        return;
    }
    xTaskNotifyGive(genHandle_);
}

void Esp32AiApp::onToken(const char *utf8, int len, void *user)
{
    (void)utf8;
    (void)len;
    Esp32AiApp *app = static_cast<Esp32AiApp *>(user);
    if (app) app->genOutSeq_++;
}

void Esp32AiApp::genTask(void *arg)
{
    Esp32AiApp *app = static_cast<Esp32AiApp *>(arg);
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        char err[96];
        // 200 tokens matches upstream N_GENERATE; chat UI streams via onToken.
        const int n = esp32aiGenerate(app->prompt_, app->genOut_, sizeof app->genOut_, 200,
                                      onToken, app, err, sizeof err);
        app->genResult_ = n;
        if (n < 0) snprintf(app->genErr_, sizeof app->genErr_, "%s", err);
        else app->genErr_[0] = 0;
        app->genDone_ = true;
        app->genBusy_ = false;
    }
}

void Esp32AiApp::tickCb(lv_timer_t *t)
{
    (void)t;
    Esp32AiApp *app = s_active;
    if (!app) return;

    if (app->chatMode_)
    {
        if (app->genBusy_ && app->genOutSeq_ != app->genOutShown_)
        {
            app->genOutShown_ = app->genOutSeq_;
            app->refreshAssistantRow();
        }
        if (app->genDone_)
        {
            app->genDone_ = false;
            if (app->sendBtn_) lv_obj_clear_state(app->sendBtn_, LV_STATE_DISABLED);
            if (app->genResult_ < 0)
            {
                app->setStatus(app->genErr_[0] ? app->genErr_ : TR("Generation failed"));
                if (app->streamBody_) lv_label_set_text(app->streamBody_, "(failed)");
            }
            else
            {
                app->refreshAssistantRow();
                if (!app->genOut_[0] && app->streamBody_)
                    lv_label_set_text(app->streamBody_, "(empty)");
                app->setStatus(TR("Done"));
            }
            app->streamBody_ = nullptr;
            if (!httpDownloadBusy() && app->timer_ && !app->genBusy_)
            {
                lv_timer_del(app->timer_);
                app->timer_ = nullptr;
            }
        }
        return;
    }

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
        if (!app->chatMode_)
        {
            char msg[128];
            snprintf(msg, sizeof msg, TR("Saved %s to the card"), name);
            app->notify(msg);
        }
    }
    else
    {
        app->notify(err[0] ? err : TR("Download failed"), 3200);
    }
}
