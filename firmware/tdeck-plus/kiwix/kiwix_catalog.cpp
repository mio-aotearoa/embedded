// SPDX-License-Identifier: GPL-3.0-or-later
#include "kiwix_catalog.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/platform.h>

#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

namespace
{
// Official download.kiwix.org/zim/ now 301s the root index to hub.kiwix.org
// (WordPress), so Apache-style folder browsing there is gone. FAU still serves
// the classic directory listing the parser expects; file downloads keep using
// download.kiwix.org (subpaths still redirect to lb.download.kiwix.org).
constexpr const char* kBase = "https://ftp.fau.de/kiwix/zim/";
constexpr size_t kMaxListing = 64 * 1024;  // the biggest folder index is far below this
constexpr size_t kMaxItems = 80;
constexpr int kMaxHops = 4;

// Same TLS-buffer workaround as http_download: Arduino-ESP32 mbedtls wants
// ~32 KB contiguous internal RAM; with the UI up that fails as HTTP -1.
void* tlsPsramCalloc(size_t n, size_t size)
{
    void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
    return p;
}

void tlsPsramFree(void* p) { heap_caps_free(p); }

// GET `url` into `body`, following up to kMaxHops redirects (absolute or relative).
bool readPageOnce(const char* url, std::string& body, char* err, size_t errLen)
{
    String current(url);
    for (int hop = 0; hop <= kMaxHops; ++hop)
    {
        HTTPClient http;
        WiFiClientSecure* tls = nullptr;
        WiFiClient* plain = nullptr;
        http.setReuse(false);
        http.setConnectTimeout(20000);
        http.setTimeout(20000);
        http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
        http.setUserAgent("wadamesh-tdeck");

        bool began = false;
        if (current.startsWith("https://"))
        {
            tls = new WiFiClientSecure();
            if (!tls)
            {
                snprintf(err, errLen, "out of memory for TLS");
                return false;
            }
            tls->setInsecure();  // no cert store on the device
            tls->setHandshakeTimeout(45);
            began = http.begin(*tls, current);
        }
        else
        {
            plain = new WiFiClient();
            if (!plain)
            {
                snprintf(err, errLen, "out of memory for TCP");
                return false;
            }
            began = http.begin(*plain, current);
        }
        if (!began)
        {
            delete tls;
            delete plain;
            snprintf(err, errLen, "cannot parse the catalogue URL");
            return false;
        }

        http.addHeader("Accept-Encoding", "identity");
        const int code = http.GET();
        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308)
        {
            // getLocation() — header("Location") is empty without collectHeaders().
            String loc = http.getLocation();
            http.end();
            delete tls;
            delete plain;
            if (!loc.length())
            {
                snprintf(err, errLen, "catalogue redirect without a Location");
                return false;
            }
            if (loc.startsWith("http"))
            {
                current = loc;
            }
            else
            {
                const int scheme = current.indexOf("://");
                const int slash = current.indexOf('/', scheme < 0 ? 0 : scheme + 3);
                current = (slash < 0 ? current : current.substring(0, slash)) + loc;
            }
            continue;
        }
        if (code != 200)
        {
            snprintf(err, errLen, "HTTP %d (%s) from the catalogue", code,
                     HTTPClient::errorToString(code).c_str());
            http.end();
            delete tls;
            delete plain;
            return false;
        }

        WiFiClient* stream = http.getStreamPtr();
        body.clear();
        uint32_t last = millis();
        while (body.size() < kMaxListing)
        {
            const size_t avail = stream ? (size_t)stream->available() : 0;
            if (!avail)
            {
                if (!http.connected() || millis() - last > 20000) break;
                delay(2);
                continue;
            }
            last = millis();
            char buf[512];
            const size_t want = avail > sizeof buf ? sizeof buf : avail;
            const int got = stream->readBytes(buf, want);
            if (got <= 0) break;
            body.append(buf, (size_t)got);
        }
        http.end();
        delete tls;
        delete plain;
        if (body.empty())
        {
            snprintf(err, errLen, "the catalogue is empty");
            return false;
        }
        return true;
    }
    snprintf(err, errLen, "too many catalogue redirects");
    return false;
}

// One retry on top of readPageOnce. The page is opened right after Wi-Fi comes
// up, when the first TLS handshake can lose its socket; a bare HTTPClient -1
// then made the catalogue look broken when a second attempt succeeds.
bool readPage(const char* url, std::string& body, char* err, size_t errLen)
{
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (readPageOnce(url, body, err, errLen)) return true;
        if (attempt == 0) delay(300);
    }
    return false;
}

// Pulls names (and, for files, the size the listing prints after the date) out
// of an Apache-style index. Everything else in the page is ignored on purpose:
// no HTML parser for a page this regular.
void parseListing(const std::string& html, std::vector<KiwixCatalogItem>& out)
{
    size_t pos = 0;
    while (out.size() < kMaxItems)
    {
        pos = html.find("href=\"", pos);
        if (pos == std::string::npos) break;
        pos += 6;
        const size_t end = html.find('"', pos);
        if (end == std::string::npos) break;
        const std::string href = html.substr(pos, end - pos);
        pos = end + 1;

        if (href.empty() || href.size() > 200) continue;
        if (href[0] == '?' || href[0] == '/' || href[0] == '#') continue;
        if (href == "../" || href.compare(0, 2, "./") == 0) continue;

        const bool dir = href.back() == '/';
        const std::string name = dir ? href.substr(0, href.size() - 1) : href;
        if (name.empty() || name.find('/') != std::string::npos) continue;
        if (!dir)
        {
            std::string lower = name;
            for (char& c : lower) c = (char)tolower((unsigned char)c);
            if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".zim") != 0) continue;
        }

        KiwixCatalogItem item{};
        snprintf(item.name, sizeof item.name, "%s", name.c_str());
        item.isDir = dir;

        // The listing line ends with the size ("... 2024-07-15 21:35 2.0M").
        const size_t lineEnd = html.find('\n', end);
        std::string tail = html.substr(end, lineEnd == std::string::npos ? 80 : lineEnd - end);
        const size_t tag = tail.find('>');
        if (tag != std::string::npos) tail.erase(0, tag + 1);
        size_t t2 = tail.size();
        while (t2 > 0 && isspace((unsigned char)tail[t2 - 1])) --t2;
        size_t t1 = t2;
        while (t1 > 0 && !isspace((unsigned char)tail[t1 - 1])) --t1;
        const std::string size = tail.substr(t1, t2 - t1);

        bool numeric = !size.empty() && isdigit((unsigned char)size[0]);
        for (char c : size)
        {
            if (!isdigit((unsigned char)c) && c != '.' && c != 'K' && c != 'M' && c != 'G' &&
                c != 'T' && c != 'B' && c != 'k' && c != 'm' && c != 'g')
            {
                numeric = false;
            }
        }
        if (numeric && !dir)
        {
            snprintf(item.size, sizeof item.size, "%s", size.c_str());
            double scale = 1.0;
            switch (toupper((unsigned char)size.back()))
            {
                case 'K': scale = 1024.0; break;
                case 'M': scale = 1024.0 * 1024.0; break;
                case 'G': scale = 1024.0 * 1024.0 * 1024.0; break;
                case 'T': scale = 1024.0 * 1024.0 * 1024.0 * 1024.0; break;
                default: break;
            }
            item.bytes = (uint64_t)(atof(size.c_str()) * scale);
        }
        out.push_back(item);
    }

    std::sort(out.begin(), out.end(), [](const KiwixCatalogItem& a, const KiwixCatalogItem& b) {
        if (a.isDir != b.isDir) return a.isDir;   // folders first
        return strcasecmp(a.name, b.name) < 0;
    });
}
}  // namespace

namespace
{
std::vector<KiwixCatalogItem> s_items;
KiwixCatalogState s_state;
char s_dir[192];
char s_pending[192];
volatile bool s_have_pending = false;
TaskHandle_t s_task = nullptr;
StaticTask_t* s_tcb = nullptr;
StackType_t* s_stack = nullptr;
uint32_t s_stackBytes = 0;

void fetchOne(const char* leaf, std::vector<KiwixCatalogItem>& items, char* err, size_t errLen,
              bool& ok)
{
    ok = false;
    err[0] = '\0';
    items.clear();
    if (WiFi.status() != WL_CONNECTED)
    {
        snprintf(err, errLen, "Connect to Wi-Fi first");
        return;
    }

    char url[256];
    while (*leaf == '/') ++leaf;
    if (*leaf) snprintf(url, sizeof url, "%s%s/", kBase, leaf);
    else       snprintf(url, sizeof url, "%s", kBase);

    std::string html;
    ok = readPage(url, html, err, errLen);
    if (!ok) return;
    parseListing(html, items);
    if (items.empty())
    {
        snprintf(err, errLen, "No archives in this folder");
        ok = false;
    }
}

// Long-lived worker. Stack is reserved with xTaskCreateStatic (see
// kiwixCatalogWarmup): a plain xTaskCreate(16 KB) fails once LVGL has
// fragmented internal heap ("out of memory for catalogue task").
void catalogTask(void*)
{
    mbedtls_platform_set_calloc_free(tlsPsramCalloc, tlsPsramFree);

    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        for (;;)
        {
            char leaf[192];
            snprintf(leaf, sizeof leaf, "%s", s_dir);

            std::vector<KiwixCatalogItem> items;
            char err[96] = {};
            bool ok = false;
            fetchOne(leaf, items, err, sizeof err, ok);

            // Folder tap while this hop was in flight: drop the stale result.
            if (s_have_pending)
            {
                snprintf(s_dir, sizeof s_dir, "%s", s_pending);
                s_have_pending = false;
                continue;
            }

            if (ok) s_items.swap(items);
            else    s_items.clear();
            snprintf(s_state.err, sizeof s_state.err, "%s", ok ? "" : err);
            s_state.items = &s_items;
            s_state.ok = ok;
            s_state.running = false;
            s_state.done = true;
            break;
        }
    }
}

bool spawnCatalogTask(void)
{
    if (s_task) return true;

    if (!s_tcb)
    {
        s_tcb = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t),
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_stack)
    {
        // Prefer internal RAM (TLS is happier there). Same size ladder as the
        // ESP-AI gen worker — 16 KB contiguous often does not exist late in UI
        // life, but 8–12 KB still does.
        static const uint32_t kSizes[] = {12 * 1024, 10 * 1024, 8 * 1024, 6 * 1024};
        for (uint32_t sz : kSizes)
        {
            s_stack = (StackType_t*)heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (s_stack)
            {
                s_stackBytes = sz;
                break;
            }
        }
        if (!s_stack)
        {
            for (uint32_t sz : kSizes)
            {
                s_stack = (StackType_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (s_stack)
                {
                    s_stackBytes = sz;
                    break;
                }
            }
        }
    }
    if (!s_tcb || !s_stack) return false;

    s_task = xTaskCreateStaticPinnedToCore(
        catalogTask, "kicat", s_stackBytes / sizeof(StackType_t), nullptr, 4, s_stack, s_tcb, 0);
    if (!s_task) return false;

    Serial.printf("[kicat] stack %u B (free_int=%u max=%u psram=%u)\n",
                  (unsigned)s_stackBytes, (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return true;
}
}  // namespace

bool kiwixCatalogWarmup(void) { return spawnCatalogTask(); }

bool kiwixCatalogStart(const char* dir)
{
    const char* leaf = dir ? dir : "";
    if (strlen(leaf) >= sizeof s_dir) return false;

    // Supersede an in-flight listing instead of failing the UI.
    if (s_state.running)
    {
        snprintf(s_pending, sizeof s_pending, "%s", leaf);
        s_have_pending = true;
        return true;
    }

    snprintf(s_dir, sizeof s_dir, "%s", leaf);
    s_have_pending = false;
    s_state.running = true;
    s_state.done = false;
    s_state.ok = false;
    s_state.err[0] = '\0';

    if (!spawnCatalogTask())
    {
        s_state.running = false;
        snprintf(s_state.err, sizeof s_state.err, "out of memory for catalogue task");
        s_state.done = true;
        s_state.ok = false;
        return false;
    }

    xTaskNotifyGive(s_task);
    return true;
}

const KiwixCatalogState& kiwixCatalogState(void) { return s_state; }

void kiwixCatalogClearDone(void)
{
    if (s_state.running) return;
    s_state.done = false;
    s_state.ok = false;
    s_state.err[0] = '\0';
}
