// SPDX-License-Identifier: GPL-3.0-or-later
//
// Web browser page for the T-Deck Plus.
//
// HTML/CSS come from the pinned TactileBrowser core (third_party/TactileBrowser,
// Lexbor inside). That core talks to the platform only through RenderInterface
// function pointers, so it needs no LVGL 9 port: this file supplies an LVGL 8.3
// renderer, an HTTP(S) fetcher and the page chrome. JavaScript is not wired yet
// (the core has no JS engine; Duktape would be the next step).
#include "browser_app.h"

#include "AppPage.h"
#include "i18n.h"
#include "tactilebrowser_core.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <lvgl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
constexpr uint32_t kBg = 0x101418;
constexpr size_t kMaxHtml = 16384;

struct FetchState
{
    volatile bool running;
    volatile bool done;
    volatile bool ok;
    volatile bool discard;
    char url[256];
    char *body;
    size_t len;
    char err[64];
};

FetchState s_fetch = {};

// ---- renderer -------------------------------------------------------------

bool rInit(Renderer *) { return true; }
void rCleanup(Renderer *) {}

void *rCreateLabel(Renderer *r, const char *text, int x, int y)
{
    if (!r || !r->platform_data) return nullptr;
    lv_obj_t *label = lv_label_create((lv_obj_t *)r->platform_data);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_SCROLLABLE);
    return label;
}

void *rCreateButton(Renderer *r, const char *text, int x, int y)
{
    if (!r || !r->platform_data) return nullptr;
    lv_obj_t *btn = lv_btn_create((lv_obj_t *)r->platform_data);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, 90, 32);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text ? text : "");
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_center(label);
    return btn;
}

void *textWidget(Renderer *r, const char *value, const char *placeholder, int x,
                 int y, int width, int height, bool multiline)
{
    if (!r || !r->platform_data) return nullptr;
    lv_obj_t *ta = lv_textarea_create((lv_obj_t *)r->platform_data);
    lv_obj_set_pos(ta, x, y);
    lv_obj_set_size(ta, width > 0 ? width : 240, height > 0 ? height : (multiline ? 90 : 34));
    lv_textarea_set_one_line(ta, !multiline);
    lv_textarea_set_text(ta, value ? value : "");
    if (placeholder && placeholder[0]) lv_textarea_set_placeholder_text(ta, placeholder);
    return ta;
}

void *rCreateTextInput(Renderer *r, const char *value, const char *placeholder,
                       int x, int y, int width, int height)
{
    return textWidget(r, value, placeholder, x, y, width, height, false);
}

void *rCreateTextArea(Renderer *r, const char *value, int x, int y, int width, int height)
{
    return textWidget(r, value, nullptr, x, y, width, height, true);
}

void *rCreateContainer(Renderer *r, int x, int y, int width, int height)
{
    if (!r || !r->platform_data) return nullptr;
    lv_obj_t *box = lv_obj_create((lv_obj_t *)r->platform_data);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, width > 0 ? width : lv_pct(100), height > 0 ? height : LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

void rSetTextColor(Renderer *, void *widget, uint32_t color)
{
    if (widget) lv_obj_set_style_text_color((lv_obj_t *)widget, lv_color_hex(color), LV_PART_MAIN);
}

void rSetBgColor(Renderer *, void *widget, uint32_t color)
{
    if (!widget) return;
    lv_obj_set_style_bg_color((lv_obj_t *)widget, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa((lv_obj_t *)widget, LV_OPA_COVER, LV_PART_MAIN);
}

void rSetBgGradient(Renderer *, void *widget, const LinearGradientFill *g)
{
    if (!widget || !g || g->stop_count == 0) return;
    lv_obj_t *obj = (lv_obj_t *)widget;
    lv_obj_set_style_bg_color(obj, lv_color_hex(g->stops[0].color), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(g->stops[g->stop_count - 1].color),
                                   LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
}

void rSetTextAlign(Renderer *, void *widget, int align)
{
    if (!widget) return;
    lv_text_align_t a = LV_TEXT_ALIGN_LEFT;
    if (align == 1) a = LV_TEXT_ALIGN_CENTER;
    else if (align == 2) a = LV_TEXT_ALIGN_RIGHT;
    lv_obj_set_style_text_align((lv_obj_t *)widget, a, LV_PART_MAIN);
}

void rClearContainer(Renderer *, void *container)
{
    if (container) lv_obj_clean((lv_obj_t *)container);
}

int rGetHeight(Renderer *, void *widget)
{
    return widget ? (int)lv_obj_get_height((lv_obj_t *)widget) : 0;
}

void rRegisterLink(Renderer *, void *widget, const char *url)
{
    if (!widget || !url) return;
    char *copy = (char *)malloc(strlen(url) + 1);
    if (!copy) return;
    strcpy(copy, url);
    lv_obj_add_flag((lv_obj_t *)widget, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb((lv_obj_t *)widget, BrowserApp::linkCb, LV_EVENT_CLICKED, copy);
}

RenderInterface s_iface = {
    rInit, rCleanup, rCreateLabel, rCreateButton, rCreateTextInput, rCreateTextArea,
    rRegisterLink, rCreateContainer, rSetTextColor, rSetBgColor, rSetBgGradient,
    rSetTextAlign, rClearContainer, rGetHeight, nullptr,
};
}  // namespace

BrowserApp *BrowserApp::s_active = nullptr;
char BrowserApp::barTitle_[32];

void BrowserApp::launch(void)
{
    if (s_active) return;
    BrowserApp *app = new BrowserApp();
    s_active = app;
    if (!app->open())
    {
        s_active = nullptr;
        delete app;
    }
}

bool BrowserApp::isOpen(void) { return s_active != nullptr && s_active->root_ != nullptr; }

void BrowserApp::destroyAsync(void *p) { delete static_cast<BrowserApp *>(p); }

void BrowserApp::dismiss(void)
{
    if (!s_active) return;
    BrowserApp *app = s_active;
    s_active = nullptr;
    app->close();
    lv_async_call(destroyAsync, app);
}

BrowserApp::~BrowserApp() {}

bool BrowserApp::open(void)
{
    const lv_coord_t w = lv_disp_get_hor_res(nullptr);
    const lv_coord_t h = appPageContentH();

    root_ = appPageCreateRoot(kBg);
    if (!root_) return false;

    snprintf(barTitle_, sizeof barTitle_, "%s", TR("Browser"));
    appPageBegin(barTitle_, &BrowserApp::dismiss);

    if (!tactilebrowser_core_init()) return false;
    tactilebrowser_set_renderer(&s_iface);

    ta_ = lv_textarea_create(root_);
    lv_obj_set_size(ta_, w - 66, 30);
    lv_obj_set_pos(ta_, 4, 2);
    lv_textarea_set_one_line(ta_, true);
    lv_textarea_set_max_length(ta_, 255);
    lv_textarea_set_placeholder_text(ta_, "http://...");
    lv_obj_add_event_cb(ta_, taCb, LV_EVENT_ALL, nullptr);

    lv_obj_t *go = lv_btn_create(root_);
    lv_obj_set_size(go, 56, 30);
    lv_obj_set_pos(go, w - 58, 2);
    lv_obj_add_event_cb(go, goCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *goLabel = lv_label_create(go);
    lv_label_set_text(goLabel, TR("Go"));
    lv_obj_center(goLabel);

    view_ = lv_obj_create(root_);
    lv_obj_set_pos(view_, 0, 36);
    lv_obj_set_size(view_, w, h - 62);
    lv_obj_set_style_bg_opa(view_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(view_, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_color(view_, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_style_pad_all(view_, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(view_, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(view_, LV_DIR_VER);

    status_ = lv_label_create(root_);
    lv_obj_set_width(status_, w - 8);
    lv_obj_set_pos(status_, 4, h - 24);
    lv_obj_set_style_text_color(status_, lv_color_hex(0x9AA4AE), LV_PART_MAIN);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_WRAP);

    kb_ = lv_keyboard_create(root_);
    lv_obj_set_size(kb_, w, 110);
    lv_obj_align(kb_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(kb_, LV_OBJ_FLAG_HIDDEN);

    setStatus(TR("Enter a URL"));
    timer_ = lv_timer_create(tickCb, 200, nullptr);
    return true;
}

void BrowserApp::close(void)
{
    s_fetch.discard = true;
    if (timer_)
    {
        lv_timer_del(timer_);
        timer_ = nullptr;
    }
    tactilebrowser_core_cleanup();
    appPageEnd(&BrowserApp::dismiss);
    appPageDeleteRootAsync(root_);
    root_ = nullptr;
    ta_ = nullptr;
    kb_ = nullptr;
    view_ = nullptr;
    status_ = nullptr;
}

void BrowserApp::setStatus(const char *msg)
{
    if (status_) lv_label_set_text(status_, msg ? msg : "");
}

void BrowserApp::navigate(const char *url)
{
    if (!url || !url[0]) return;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
    {
        setStatus(TR("Only http:// and https:// URLs are supported"));
        return;
    }
    if (s_fetch.running)
    {
        setStatus(TR("A page is already loading"));
        return;
    }
    if (WiFi.status() != WL_CONNECTED)
    {
        setStatus(TR("Connect to Wi-Fi first"));
        return;
    }

    memset((void *)&s_fetch, 0, sizeof s_fetch);
    snprintf(s_fetch.url, sizeof s_fetch.url, "%s", url);
    if (ta_) lv_textarea_set_text(ta_, url);
    s_fetch.running = true;
    setStatus(TR("Loading..."));

    if (xTaskCreatePinnedToCore(fetchTask, "browser-fetch", 8192, nullptr, 1, nullptr, 0) != pdPASS)
    {
        s_fetch.running = false;
        setStatus(TR("Cannot start the fetch"));
    }
}

void BrowserApp::fetchTask(void *)
{
    FetchState &f = s_fetch;
    HTTPClient http;
    http.setTimeout(9000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    // The clients must outlive http.GET(): HTTPClient keeps a pointer to them.
    WiFiClient plain;
    WiFiClientSecure secure;
    bool begun = false;
    if (strncmp(f.url, "https://", 8) == 0)
    {
        secure.setInsecure();  // no certificate store here, same as the Reader
        begun = http.begin(secure, f.url);
    }
    else
    {
        begun = http.begin(plain, f.url);
    }

    if (!begun)
    {
        snprintf(f.err, sizeof f.err, "%s", "Cannot open the connection");
        f.running = false;
        f.done = true;
        vTaskDelete(nullptr);
        return;
    }

    const int code = http.GET();
    if (code != 200)
    {
        snprintf(f.err, sizeof f.err, "HTTP %d", code);
        http.end();
        f.running = false;
        f.done = true;
        vTaskDelete(nullptr);
        return;
    }

    String body = http.getString();
    http.end();

    if (f.discard)
    {
        f.running = false;
        vTaskDelete(nullptr);
        return;
    }

    size_t len = body.length();
    if (len > kMaxHtml) len = kMaxHtml;
    char *buf = (char *)malloc(len + 1);
    if (!buf)
    {
        snprintf(f.err, sizeof f.err, "%s", "Out of memory for the page");
        f.running = false;
        f.done = true;
        vTaskDelete(nullptr);
        return;
    }
    memcpy(buf, body.c_str(), len);
    buf[len] = 0;

    f.body = buf;
    f.len = len;
    f.ok = true;
    f.running = false;
    f.done = true;
    vTaskDelete(nullptr);
}

void BrowserApp::renderPage(void)
{
    if (!view_ || !s_fetch.ok || !s_fetch.body) return;

    lv_obj_clean(view_);
    const lv_coord_t w = lv_obj_get_width(view_);
    const RenderResult result = tactilebrowser_render_html_string(
        s_fetch.url, s_fetch.body, s_fetch.len, view_, w, 4000);

    if (result != RENDER_SUCCESS)
    {
        setStatus(TR("This page could not be rendered"));
        return;
    }
    setStatus(s_fetch.url);
}

void BrowserApp::goCb(lv_event_t *)
{
    if (!s_active || !s_active->ta_) return;
    s_active->navigate(lv_textarea_get_text(s_active->ta_));
}

void BrowserApp::taCb(lv_event_t *e)
{
    if (!s_active) return;
    BrowserApp *app = s_active;
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED)
    {
        if (app->kb_)
        {
            lv_keyboard_set_textarea(app->kb_, app->ta_);
            lv_obj_clear_flag(app->kb_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
    {
        if (app->kb_)
        {
            lv_keyboard_set_textarea(app->kb_, nullptr);
            lv_obj_add_flag(app->kb_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void BrowserApp::linkCb(lv_event_t *e)
{
    const char *url = (const char *)lv_event_get_user_data(e);
    if (!url) return;
    // The handler fires from inside the renderer's own widget tree; replacing
    // the tree here would delete the widget that is still dispatching.
    if (s_active) s_active->navigate(url);
}

void BrowserApp::tickCb(lv_timer_t *)
{
    BrowserApp *app = s_active;
    if (!app) return;

    if (s_fetch.done && s_fetch.ok)
    {
        s_fetch.done = false;
        app->renderPage();
        free(s_fetch.body);
        s_fetch.body = nullptr;
        s_fetch.len = 0;
        s_fetch.ok = false;
        return;
    }
    if (s_fetch.done && !s_fetch.ok)
    {
        s_fetch.done = false;
        app->setStatus(s_fetch.err[0] ? s_fetch.err : TR("The page could not be loaded"));
    }
}
