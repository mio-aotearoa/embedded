// SPDX-License-Identifier: GPL-3.0-or-later
#include "http_download.h"
#include "meshcomod_sd_io.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/platform.h>

#include <stdio.h>
#include <string.h>

namespace
{
HttpDownloadState s_state;
TaskHandle_t s_task = nullptr;
StaticTask_t *s_tcb = nullptr;
StackType_t *s_stack = nullptr;
uint32_t s_stackBytes = 0;
volatile bool s_cancel = false;

// Hugging Face 302s to a signed CDN URL that is ~1.1 KB; 320 B truncated it and
// the next hop then failed as HTTP -1 ("connection refused").
char s_url[1536];
char s_dest[192];
char s_tmp[208];
uint64_t s_expected = 0;

void setError(const char *msg)
{
    snprintf(s_state.err, sizeof s_state.err, "%s", msg);
}

void finish(bool ok)
{
    s_state.ok = ok;
    s_state.running = false;
    s_state.done = true;
}

// Arduino-ESP32 ships a prebuilt mbedtls that always allocates the full TLS
// record buffers from internal RAM (~16 KB x2). With the UI up the T-Deck only
// has ~18 KB max contiguous internal, so HTTPS fails as HTTP -1. Point mbedtls
// at PSRAM for the rest of the run (browser/Kiwix HTTPS need the same).
void *tlsPsramCalloc(size_t n, size_t size)
{
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p)
    {
        p = heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
    }
    return p;
}

void tlsPsramFree(void *p)
{
    heap_caps_free(p);
}

void removePartial()
{
    if (s_tmp[0])
    {
        SD.remove(s_tmp);
    }
}

void runOneDownload(void)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        setError("Wi-Fi is not connected");
        finish(false);
        return;
    }

    HTTPClient http;
    // Heap-allocated so the winning hop's client outlives the redirect loop and
    // can still feed getStreamPtr() while the body is written.
    WiFiClientSecure *tls = nullptr;
    WiFiClient *plain = nullptr;
    char current[1536];
    snprintf(current, sizeof current, "%s", s_url);
    int length = 0;

    // Follow redirects by hand. Both hosts this firmware pulls from (Hugging
    // Face, the Kiwix mirror) 302 to a DIFFERENT host, and HTTPClient cannot
    // re-connect a caller-supplied WiFiClientSecure across hosts: its own
    // redirect following aborts with HTTP -1 ("connection refused"), which is
    // exactly what the ESP-AI model download was reporting.
    //
    // Use getLocation() — header("Location") is empty unless collectHeaders()
    // was called; Location is always stored on the client via getLocation().
    // Fresh TLS client per hop: a reused WiFiClientSecure after end() also
    // surfaces as connection refused on the CDN host.
    for (int hop = 0;; ++hop)
    {
        http.end();
        delete tls;
        tls = nullptr;
        delete plain;
        plain = nullptr;

        http.setReuse(false);
        http.setConnectTimeout(20000);
        http.setTimeout(60000);
        http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
        http.setUserAgent("Mozilla/5.0 (compatible; wadamesh-tdeck)");

        const bool https = strncmp(current, "https://", 8) == 0;
        bool began = false;
        if (https)
        {
            tls = new WiFiClientSecure();
            if (!tls)
            {
                setError("out of memory for TLS");
                finish(false);
                return;
            }
            tls->setInsecure();  // no cert store on the device; see docs/architecture.md
            tls->setHandshakeTimeout(45);
            // Contiguous internal heap is what TLS needs; log it so a -1 can be
            // told apart from a real refused connection vs an OOM handshake.
            char host[128];
            host[0] = 0;
            {
                const char *p = current + 8;
                size_t n = 0;
                while (p[n] && p[n] != '/' && p[n] != ':' && n + 1 < sizeof host)
                {
                    host[n] = p[n];
                    ++n;
                }
                host[n] = 0;
            }
            IPAddress ip;
            const bool resolved = host[0] && WiFi.hostByName(host, ip);
            Serial.printf("[httpdl] heap free=%u maxblk=%u psram=%u dns=%s ip=%s\n",
                          (unsigned)ESP.getFreeHeap(),
                          (unsigned)ESP.getMaxAllocHeap(),
                          (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                          resolved ? "ok" : "fail",
                          resolved ? ip.toString().c_str() : "-");
            began = http.begin(*tls, current);
        }
        else
        {
            plain = new WiFiClient();
            if (!plain)
            {
                setError("out of memory for TCP");
                finish(false);
                return;
            }
            began = http.begin(*plain, current);
        }
        if (!began)
        {
            setError("cannot parse the URL");
            delete tls;
            delete plain;
            finish(false);
            return;
        }

        http.addHeader("Accept-Encoding", "identity");
        const int code = http.GET();
        s_state.http_code = code;
        Serial.printf("[httpdl] hop %d GET %d url_len=%u host='%.48s'\n", hop, code,
                      (unsigned)strlen(current),
                      current + (https ? 8 : 7));
        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308)
        {
            // Copy before end(): getLocation() refers to HTTPClient's storage.
            const String loc = http.getLocation();
            http.end();
            delete tls;
            tls = nullptr;
            delete plain;
            plain = nullptr;
            if (!loc.length() || hop >= 4)
            {
                setError(loc.length() ? "too many redirects" : "redirect without a Location");
                finish(false);
                return;
            }
            if (loc.length() >= sizeof current)
            {
                setError("redirect URL too long");
                finish(false);
                return;
            }
            if (loc.startsWith("http"))
            {
                snprintf(current, sizeof current, "%s", loc.c_str());
            }
            else
            {
                // Relative Location: resolve against the host we just talked to.
                const char *scheme = strstr(current, "://");
                const char *slash = scheme ? strchr(scheme + 3, '/') : nullptr;
                const size_t base = slash ? (size_t)(slash - current) : strlen(current);
                char next[1536];
                snprintf(next, sizeof next, "%.*s%s", (int)base, current, loc.c_str());
                snprintf(current, sizeof current, "%s", next);
            }
            Serial.printf("[httpdl] redirect -> url_len=%u\n", (unsigned)strlen(current));
            continue;
        }
        if (code != HTTP_CODE_OK)
        {
            snprintf(s_state.err, sizeof s_state.err, "HTTP %d (%s)", code,
                     HTTPClient::errorToString(code).c_str());
            http.end();
            delete tls;
            delete plain;
            finish(false);
            return;
        }
        length = http.getSize();
        s_state.total = length > 0 ? (uint64_t)length : 0;
        break;
    }

    // Ensure parent dirs exist (callers mkdir too; do it here so a bad path
    // still fails with a clear log rather than a bare open error).
    {
        char dir[160];
        snprintf(dir, sizeof dir, "%s", s_tmp);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir)
        {
            *slash = 0;
            // Walk /a, /a/b, … so nested destinations work.
            for (char *p = dir + 1; *p; ++p)
            {
                if (*p != '/') continue;
                *p = 0;
                SD.mkdir(dir);
                *p = '/';
            }
            SD.mkdir(dir);
            meshcomodMarkSdIo();
        }
    }
    File out = SD.open(s_tmp, FILE_WRITE);
    if (!out)
    {
        Serial.printf("[httpdl] SD.open('%s') failed\n", s_tmp);
        setError("cannot write to the SD card");
        http.end();
        delete tls;
        delete plain;
        finish(false);
        return;
    }
    meshcomodMarkSdIo();

    uint8_t buf[1024];
    WiFiClient *stream = http.getStreamPtr();
    int remaining = length;
    bool failed = false;

    while (http.connected() && (remaining > 0 || remaining == -1))
    {
        if (s_cancel)
        {
            setError("cancelled");
            failed = true;
            break;
        }
        const size_t available = stream->available();
        if (!available)
        {
            delay(2);
            continue;
        }
        const size_t want = available > sizeof buf ? sizeof buf : available;
        const int got = stream->readBytes(buf, want);
        if (got <= 0)
        {
            break;
        }
        if (out.write(buf, (size_t)got) != (size_t)got)
        {
            setError("SD write failed");
            failed = true;
            break;
        }
        meshcomodMarkSdIo();
        s_state.bytes += (uint64_t)got;
        if (remaining > 0)
        {
            remaining -= got;
        }
    }

    out.close();
    meshcomodMarkSdIo();
    http.end();
    delete tls;
    delete plain;

    if (!failed && remaining > 0)
    {
        setError("connection closed before the end of the file");
        failed = true;
    }
    if (!failed && s_expected && s_state.bytes != s_expected)
    {
        setError("downloaded size does not match the expected size");
        failed = true;
    }

    if (failed)
    {
        removePartial();
        s_state.bytes = 0;
        finish(false);
        return;
    }

    // Publish atomically: only now does the file exist under its real name.
    SD.remove(s_dest);
    if (!SD.rename(s_tmp, s_dest))
    {
        setError("cannot rename the finished download");
        removePartial();
        finish(false);
        return;
    }
    meshcomodMarkSdIo();
    finish(true);
}

// Long-lived worker (same pattern as the Kiwix catalogue): reserve the stack
// once with xTaskCreateStatic. A fresh xTaskCreate(24 KB) fails once LVGL has
// fragmented internal heap — that was "Cannot start the download" after browsing
// the catalogue.
void downloadTask(void *)
{
    mbedtls_platform_set_calloc_free(tlsPsramCalloc, tlsPsramFree);
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        runOneDownload();
    }
}

bool spawnDownloadTask(void)
{
    if (s_task) return true;

    if (!s_tcb)
    {
        s_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t),
                                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_stack)
    {
        // Prefer internal RAM (TLS handshakes are happier there). Ladder down if
        // the UI already ate the big contiguous blocks.
        static const uint32_t kSizes[] = {20 * 1024, 16 * 1024, 12 * 1024, 10 * 1024};
        for (uint32_t sz : kSizes)
        {
            s_stack = (StackType_t *)heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
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
                s_stack = (StackType_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (s_stack)
                {
                    s_stackBytes = sz;
                    break;
                }
            }
        }
    }
    if (!s_tcb || !s_stack) return false;

    // Pin to core 0 with Wi-Fi/LwIP: TLS from the other core often returns HTTP -1.
    s_task = xTaskCreateStaticPinnedToCore(downloadTask, "httpdl",
                                           s_stackBytes / sizeof(StackType_t), nullptr, 4,
                                           s_stack, s_tcb, 0);
    if (!s_task) return false;
    Serial.printf("[httpdl] stack %u B (free_int=%u max=%u psram=%u)\n",
                  (unsigned)s_stackBytes, (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMaxAllocHeap(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return true;
}
}  // namespace

bool httpDownloadWarmup(void) { return spawnDownloadTask(); }

bool httpDownloadStart(const char *url, const char *destPath, uint64_t expectedSize)
{
    if (s_state.running || !url || !destPath)
    {
        setError(s_state.running ? "A download is already running" : "bad download arguments");
        return false;
    }
    const bool http = strncmp(url, "http://", 7) == 0;
    const bool https = strncmp(url, "https://", 8) == 0;
    if (!http && !https)
    {
        setError("URL must start with http:// or https://");
        return false;
    }
    if (strlen(url) >= sizeof s_url)
    {
        setError("URL is too long");
        return false;
    }
    if (strlen(destPath) >= sizeof s_dest || strlen(destPath) + 5 >= sizeof s_tmp)
    {
        setError("destination path is too long");
        return false;
    }

    snprintf(s_url, sizeof s_url, "%s", url);
    snprintf(s_dest, sizeof s_dest, "%s", destPath);
    snprintf(s_tmp, sizeof s_tmp, "%s.part", s_dest);
    s_expected = expectedSize;
    s_cancel = false;

    s_state.running = true;
    s_state.done = false;
    s_state.ok = false;
    s_state.bytes = 0;
    s_state.total = 0;
    s_state.http_code = 0;
    s_state.err[0] = '\0';
    snprintf(s_state.path, sizeof s_state.path, "%s", destPath);

    if (!spawnDownloadTask())
    {
        s_state.running = false;
        setError("out of memory for download task");
        return false;
    }

    xTaskNotifyGive(s_task);
    return true;
}

const HttpDownloadState &httpDownloadState(void)
{
    return s_state;
}

void httpDownloadClearDone(void)
{
    if (!s_state.running)
    {
        s_state.done = false;
        s_state.ok = false;
        s_state.err[0] = '\0';
    }
}

void httpDownloadCancel(void)
{
    if (s_state.running)
    {
        s_cancel = true;
    }
}

bool httpDownloadBusy(void)
{
    return s_state.running;
}
