// SPDX-License-Identifier: GPL-3.0-or-later
//
// Web browser page for the T-Deck Plus (Apps → Browser).
//
// HTML/CSS come from the pinned TactileBrowser core (third_party/TactileBrowser,
// Lexbor inside). That core talks to the platform only through RenderInterface
// function pointers, so it needs no LVGL 9 port: this file supplies an LVGL 8.3
// renderer, an HTTP(S) fetcher and the page chrome. Inline scripts run on Duktape
// (js_engine.cpp) before layout. wadamesh's Web/Reader tile stays on the upstream
// text-extraction path; Lexbor lives only behind this drawer app.
#include "browser_app.h"

#include "AppPage.h"
#include "i18n.h"
#include "url_util.h"   // bare host -> https://
#include "ime_ja.h"
#include "tactilebrowser_core.h"
#include "web_enhance.h"
#include "js_engine.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <lvgl.h>

#include <esp_heap_caps.h>
#include <lexbor/core/lexbor.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
// ---- allocator ------------------------------------------------------------
// Lexbor's default allocator is plain malloc, which on this Arduino/IDF build
// serves INTERNAL RAM only. A real page's DOM is several times its markup, so
// the parse ran out of internal RAM and returned RENDER_ERROR_PARSE. Point
// Lexbor at PSRAM (internal RAM stays for radio/TLS), falling back to internal
// only if PSRAM is exhausted.
void *lxbAlloc(size_t n)
{
    if (n == 0) n = 1;
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}

void *lxbRealloc(void *dst, size_t n)
{
    if (!dst) return lxbAlloc(n);
    void *p = lxbAlloc(n);
    if (!p) return nullptr;
    const size_t old = heap_caps_get_allocated_size(dst);
    memcpy(p, dst, old < n ? old : n);
    heap_caps_free(dst);
    return p;
}

void *lxbCalloc(size_t num, size_t size)
{
    if (size && num > (size_t)-1 / size) return nullptr;
    const size_t n = num * size;
    void *p = lxbAlloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void lxbFree(void *p) { heap_caps_free(p); }

// Idempotent: must run before the first Lexbor allocation (css_parser_init()).
void lexborUsePsram()
{
    static bool done = false;
    if (!done)
    {
        done = lexbor_memory_setup(lxbAlloc, lxbRealloc, lxbCalloc, lxbFree) == LXB_STATUS_OK;
    }
}

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

// Optional embed path (browserRenderInto): when a host page owns the chrome,
// link taps go there instead of to BrowserApp.
void (*s_into_link_cb)(const char*) = nullptr;

// Why the last browserRenderInto() call fell back ("ok" on success).
const char* s_render_error = "ok";

// ---- renderer -------------------------------------------------------------

bool rInit(Renderer *) { return true; }
void rCleanup(Renderer *) {}

// Web pages assume a light canvas. Near-black ink on our dark chrome looked like
// a blank black screen when CSS set color without a background.
constexpr uint32_t kWebInk = 0x1A1A1A;
constexpr uint32_t kWebPaper = 0xFFFFFF;

static uint32_t colorLuma(uint32_t c)
{
    const uint32_t r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
    return (r * 30 + g * 59 + b * 11) / 100;
}

void *rCreateLabel(Renderer *r, const char *text, int x, int y)
{
    if (!r || !r->platform_data) return nullptr;
    lv_obj_t *label = lv_label_create((lv_obj_t *)r->platform_data);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, lv_color_hex(kWebInk), LV_PART_MAIN);
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
    if (!widget) return;
    // Engine default is white; treat that as "unset" and use readable ink on paper.
    if (color == 0xFFFFFFu || color == 0xFFFFFEu) color = kWebInk;
    lv_obj_set_style_text_color((lv_obj_t *)widget, lv_color_hex(color), LV_PART_MAIN);
}

void rSetBgColor(Renderer *, void *widget, uint32_t color)
{
    if (!widget) return;
    // Pure black full-bleed fills hide the page on a dark device; keep paper.
    if (colorLuma(color) < 8) color = kWebPaper;
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

void rSetTextStyle(Renderer *, void *widget, int font_size, bool bold)
{
    if (!widget) return;
    lv_obj_set_style_text_font((lv_obj_t *)widget, readerWebFont(font_size, bold),
                               LV_PART_MAIN);
}

void rRegisterLink(Renderer *, void *widget, const char *url)
{
    if (!widget || !url) return;
    char *copy = (char *)malloc(strlen(url) + 1);
    if (!copy) return;
    strcpy(copy, url);
    lv_obj_set_style_text_decor((lv_obj_t *)widget, LV_TEXT_DECOR_UNDERLINE, LV_PART_MAIN);
    lv_obj_add_flag((lv_obj_t *)widget, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb((lv_obj_t *)widget, BrowserApp::linkCb, LV_EVENT_CLICKED, copy);
}

RenderInterface s_iface = {
    rInit, rCleanup, rCreateLabel, rCreateButton, rCreateTextInput, rCreateTextArea,
    rRegisterLink, rCreateContainer, rSetTextColor, rSetBgColor, rSetBgGradient,
    rSetTextAlign, rClearContainer, rGetHeight, nullptr, rSetTextStyle,
};
}  // namespace

const char* browserLastRenderError() { return s_render_error; }

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

    lexborUsePsram();
    if (!tactilebrowser_core_init()) return false;
    tactilebrowser_set_renderer(&s_iface);

    // The tall "< Browser" bar is two rows high while the root starts one row
    // down, so its lower row covers the top of the page: lay out below it.
    const lv_coord_t top = appPageBarCover() + 2;

    ta_ = lv_textarea_create(root_);
    lv_obj_set_size(ta_, w - 66, 30);
    lv_obj_set_pos(ta_, 4, top);
    lv_textarea_set_one_line(ta_, true);
    lv_textarea_set_max_length(ta_, 255);
    lv_textarea_set_placeholder_text(ta_, "example.com");
    lv_obj_add_event_cb(ta_, taCb, LV_EVENT_ALL, nullptr);

    lv_obj_t *go = lv_btn_create(root_);
    lv_obj_set_size(go, 56, 30);
    lv_obj_set_pos(go, w - 58, top);
    lv_obj_add_event_cb(go, goCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *goLabel = lv_label_create(go);
    lv_label_set_text(goLabel, TR("Go"));
    lv_obj_center(goLabel);

    view_ = lv_obj_create(root_);
    lv_obj_set_pos(view_, 0, top + 34);
    lv_obj_set_size(view_, w, h - top - 34 - 26);
    lv_obj_set_style_bg_opa(view_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(view_, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_color(view_, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_style_pad_all(view_, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(view_, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(view_, LV_DIR_VER);
    lv_obj_add_event_cb(view_, viewCb, LV_EVENT_CLICKED, nullptr);

    status_ = lv_label_create(root_);
    lv_obj_set_width(status_, w - 8);
    lv_obj_set_pos(status_, 4, h - 24);
    lv_obj_set_style_text_color(status_, lv_color_hex(0x9AA4AE), LV_PART_MAIN);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_WRAP);

    // No on-screen keyboard: the T-Deck has a physical one. Binding the field
    // makes wadamesh's key handler type straight into it.
    appPageBindTextField(ta_);

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
    appPageBindTextField(nullptr);      // the field is about to be deleted
    appPageEnd(&BrowserApp::dismiss);
    appPageDeleteRootAsync(root_);
    root_ = nullptr;
    ta_ = nullptr;
    view_ = nullptr;
    status_ = nullptr;
}

void BrowserApp::setStatus(const char *msg)
{
    if (status_) lv_label_set_text(status_, msg ? msg : "");
}

void BrowserApp::navigate(const char *url)
{
    // Typing a bare host ("example.com") means https: that is what the firmware
    // can fetch, and it is what an address bar is expected to assume.
    char full[256];
    if (!urlComplete(url, full, sizeof full))
    {
        setStatus(TR("That URL is too long"));
        return;
    }
    if (!full[0]) return;
    if (strncmp(full, "http://", 7) != 0 && strncmp(full, "https://", 8) != 0)
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
    snprintf(s_fetch.url, sizeof s_fetch.url, "%s", full);
    if (ta_ && strcmp(lv_textarea_get_text(ta_), full) != 0)
    {
        // Show what is actually fetched ("example.com" gains its https://), but
        // never fight the user's own text when it already matches.
        lv_textarea_set_text(ta_, full);
        lv_textarea_set_cursor_pos(ta_, LV_TEXTAREA_CURSOR_LAST);
    }
    s_fetch.running = true;
    setStatus(TR("Connecting\xE2\x80\xA6"));

    if (xTaskCreatePinnedToCore(fetchTask, "browser-fetch", 8192, nullptr, 1, nullptr, 0) != pdPASS)
    {
        s_fetch.running = false;
        setStatus(TR("Cannot start the fetch"));
    }
}

void BrowserApp::fetchTask(void *)
{
    FetchState &f = s_fetch;

    // Follow redirects by hand. Most real hosts answer with a 301/302 to another
    // host or to a longer path (duckduckgo.com does), and HTTPClient cannot
    // re-connect a caller-supplied WiFiClientSecure across hosts: with its own
    // following enabled that abort surfaces as a bare "HTTP -1".
    char current[256];
    snprintf(current, sizeof current, "%s", f.url);

    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    int code = 0;
    for (int hop = 0; hop <= 4; ++hop)
    {
        http.setTimeout(9000);
        http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
        // Some sites reject an unfamiliar agent outright; this is honest but
        // still recognisable as a browser.
        http.setUserAgent("Mozilla/5.0 (compatible; wadamesh-tdeck)");

        secure.setInsecure();  // no certificate store here, same as the Reader
        const bool begun = strncmp(current, "https://", 8) == 0
                               ? http.begin(secure, current)
                               : http.begin(plain, current);
        if (!begun)
        {
            snprintf(f.err, sizeof f.err, "%s", "Cannot open the connection");
            f.running = false;
            f.done = true;
            vTaskDelete(nullptr);
            return;
        }

        code = http.GET();
        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308)
        {
            const String loc = http.getLocation();
            http.end();
            if (!loc.length() || hop == 4)
            {
                snprintf(f.err, sizeof f.err, "%s",
                         loc.length() ? "Too many redirects" : "Redirect without a Location");
                f.running = false;
                f.done = true;
                vTaskDelete(nullptr);
                return;
            }
            if (loc.length() >= sizeof current)
            {
                snprintf(f.err, sizeof f.err, "%s", "Redirect URL too long");
                f.running = false;
                f.done = true;
                vTaskDelete(nullptr);
                return;
            }
            if (loc.startsWith("http"))
            {
                snprintf(current, sizeof current, "%s", loc.c_str());
            }
            else
            {
                const char* scheme = strstr(current, "://");
                const char* slash = scheme ? strchr(scheme + 3, '/') : nullptr;
                const size_t base = slash ? (size_t)(slash - current) : strlen(current);
                char next[256];
                snprintf(next, sizeof next, "%.*s%s", (int)base, current, loc.c_str());
                snprintf(current, sizeof current, "%s", next);
            }
            continue;
        }
        break;
    }

    if (code != 200)
    {
        snprintf(f.err, sizeof f.err, "HTTP %d (%s)", code,
                 HTTPClient::errorToString(code).c_str());
        http.end();
        f.running = false;
        f.done = true;
        vTaskDelete(nullptr);
        return;
    }

    String body = http.getString();
    http.end();
    // Remember the address that actually served the page, so links resolve and
    // the status line shows the truth after a redirect.
    snprintf(f.url, sizeof f.url, "%s", current);

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

    // Inline scripts run first: document.write() output replaces its <script>
    // block, so Lexbor lays out what the page produced rather than the source.
    size_t scriptedLen = 0;
    char *scripted = browserJsApplyScripts(s_fetch.body, s_fetch.len, &scriptedLen);
    const char *html = scripted ? scripted : s_fetch.body;
    const size_t htmlLen = scripted ? scriptedLen : s_fetch.len;

    const RenderResult result = tactilebrowser_render_html_string(
        s_fetch.url, html, htmlLen, view_, w, 4000);
    free(scripted);

    if (result != RENDER_SUCCESS)
    {
        setStatus(TR("This page could not be rendered"));
        return;
    }
    // A script error or console.log() line says more than the URL does.
    const char *jsError = browserJsLastError();
    const char *jsLog = browserJsLastLog();
    if (jsError[0]) setStatus(jsError);
    else if (jsLog[0]) setStatus(jsLog);
    else setStatus(s_fetch.url);
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
    if (code == LV_EVENT_LONG_PRESSED)
    {
        // Japanese extension lives in wadamesh's typing path; toggle it per field.
        imeJaToggle();
        app->setStatus(imeJaActive() ? TR("Japanese: romaji, Space = kana")
                                     : TR("Enter a URL"));
        return;
    }
    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED)
    {
        appPageBindTextField(app->ta_);   // physical keys type here
        return;
    }
    if (code == LV_EVENT_READY)           // Enter = Go
    {
        app->navigate(lv_textarea_get_text(app->ta_));
        return;
    }
    if (code == LV_EVENT_CANCEL)
    {
        appPageBindTextField(nullptr);
    }
}

void BrowserApp::viewCb(lv_event_t *)
{
    // Tap on the page itself: the keyboard goes back to wadamesh's nav handling.
    appPageBindTextField(nullptr);
}

void BrowserApp::linkCb(lv_event_t *e)
{
    const char *url = (const char *)lv_event_get_user_data(e);
    if (!url) return;
    // The handler fires from inside the renderer's own widget tree; replacing
    // the tree here would delete the widget that is still dispatching.
    appPageBindTextField(nullptr);   // following a link is not typing
    if (s_active) s_active->navigate(url);
    else if (s_into_link_cb) s_into_link_cb(url);   // embed host owns navigation
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

bool browserRenderInto(lv_obj_t* parent, const char* url, const char* html, size_t len,
                       void (*on_link)(const char*))
{
    s_render_error = "no markup";
    if (!parent || !html || len == 0) return false;
    lexborUsePsram();
    if (!tactilebrowser_core_init()) { s_render_error = "core init"; return false; }
    tactilebrowser_set_renderer(&s_iface);

    // Inline scripts first, exactly as this app does: document.write() output is
    // what gets laid out.
    size_t scriptedLen = 0;
    char* scripted = browserJsApplyScripts(html, len, &scriptedLen);
    const char* doc = scripted ? scripted : html;
    const size_t docLen = scripted ? scriptedLen : len;

    s_into_link_cb = on_link;
    const bool had_scripts = scripted != nullptr;
    const RenderResult result = tactilebrowser_render_html_string(
        url ? url : "", doc, docLen, parent, lv_obj_get_width(parent), 4000);
    free(scripted);
    switch (result)
    {
        case RENDER_SUCCESS:      s_render_error = "ok"; break;
        case RENDER_ERROR_PARSE:  s_render_error = "parse"; break;
        case RENDER_ERROR_MEMORY: s_render_error = "memory"; break;
        case RENDER_ERROR_NETWORK:s_render_error = "network"; break;
        default:                  s_render_error = "no body"; break;
    }
    Serial.printf("[browser] render %s url='%s' bytes=%u scripts=%s -> %s\n",
                  result == RENDER_SUCCESS ? "html" : "fail",
                  url ? url : "", (unsigned)docLen,
                  had_scripts ? "yes" : "no", s_render_error);
    return result == RENDER_SUCCESS;
}
