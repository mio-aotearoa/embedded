// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

// esp32-ai TinyStories on the T-Deck Plus.
//
// Assets (model.bin + tokenizer.json) live on the SD card and are fetched over
// Wi-Fi when missing. When both files match the pinned sizes, the page is a
// chat UI driven by the real slvDev/esp32-ai runtime (SD-backed weights).
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
    void showDownload(void);
    void showChat(void);
    void beginDownload(int fileIndex);
    void setStatus(const char *msg);
    void ensureChatPane(void);
    lv_obj_t *addChatMessage(const char *who, const char *text, bool streaming);
    void refreshAssistantRow(void);
    void startGenerate(void);

    static void rowCb(lv_event_t *e);
    static void tickCb(lv_timer_t *t);
    static void sendCb(lv_event_t *e);
    static void taCb(lv_event_t *e);
    static void genTask(void *arg);
    static void onToken(const char *utf8, int len, void *user);
    static void destroyAsync(void *p);

    static Esp32AiApp *s_active;
    static char barTitle_[32];

    lv_obj_t *root_ = nullptr;
    lv_obj_t *list_ = nullptr;       // download file rows
    lv_obj_t *chat_ = nullptr;       // wrapped message transcript
    lv_obj_t *status_ = nullptr;
    lv_obj_t *ta_ = nullptr;         // chat input
    lv_obj_t *sendBtn_ = nullptr;
    lv_obj_t *streamBody_ = nullptr; // live assistant text while generating
    lv_timer_t *timer_ = nullptr;
    int pending_ = -1;
    bool chatMode_ = false;

    // Generation worker → UI (genOut_ filled as tokens arrive; UI polls it)
    volatile bool genBusy_ = false;
    volatile bool genDone_ = false;
    volatile int genResult_ = 0;
    volatile uint32_t genOutSeq_ = 0;
    uint32_t genOutShown_ = 0;
    char genErr_[96] = {};
    char genOut_[1600] = {};
    char prompt_[256] = {};

    // Persistent worker. TCB + stack must be internal SRAM — Esp32AiApp itself
    // may live in PSRAM, and xPortCheckValidTCBMem rejects external TCB memory.
    StackType_t *genStack_ = nullptr;
    StaticTask_t *genTcb_ = nullptr;
    TaskHandle_t genHandle_ = nullptr;
    uint32_t genStackBytes_ = 0;
};
