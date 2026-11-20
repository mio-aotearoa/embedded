// SPDX-License-Identifier: GPL-3.0-or-later
#include "http_download.h"
#include "http_fetch.h"
#include "sd_atomic.h"
#include "task_spawn.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <stdio.h>
#include <string.h>

namespace
{
HttpDownloadState s_state;
TaskHandle_t s_task = nullptr;
StaticTaskMem s_mem{};
volatile bool s_cancel = false;

char s_url[1536];
char s_dest[192];
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

void runOneDownload(void)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        setError("Wi-Fi is not connected");
        finish(false);
        return;
    }

    char current[1536];
    snprintf(current, sizeof current, "%s", s_url);
    Serial.printf("[httpdl] heap free=%u maxblk=%u psram=%u url_len=%u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  (unsigned)strlen(current));

    HttpFetch fetch;
    if (!fetch.openGet(current, current, sizeof current, s_state.err, sizeof s_state.err,
                       /*connectTimeoutMs=*/20000, /*readTimeoutMs=*/60000, /*maxHops=*/4))
    {
        finish(false);
        return;
    }
    s_state.http_code = fetch.httpCode();
    const int length = fetch.contentLength();
    s_state.total = length > 0 ? (uint64_t)length : 0;
    Serial.printf("[httpdl] GET 200 url_len=%u len=%d\n", (unsigned)strlen(current), length);

    SdAtomicFile out;
    if (!out.begin(s_dest, s_state.err, sizeof s_state.err))
    {
        Serial.printf("[httpdl] open part failed: %s\n", s_state.err);
        fetch.close();
        finish(false);
        return;
    }

    uint8_t buf[1024];
    WiFiClient *stream = fetch.stream();
    int remaining = length;
    bool failed = false;

    while (fetch.client().connected() && (remaining > 0 || remaining == -1))
    {
        if (s_cancel)
        {
            setError("cancelled");
            failed = true;
            break;
        }
        if (!stream)
        {
            setError("no response stream");
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
        if (got <= 0) break;
        if (!out.write(buf, (size_t)got, s_state.err, sizeof s_state.err))
        {
            failed = true;
            break;
        }
        s_state.bytes += (uint64_t)got;
        if (remaining > 0) remaining -= got;
    }

    fetch.close();

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
        out.abort();
        s_state.bytes = 0;
        finish(false);
        return;
    }
    if (!out.commit(s_state.err, sizeof s_state.err))
    {
        finish(false);
        return;
    }
    finish(true);
}

void downloadTask(void *)
{
    httpTlsUsePsram();
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        runOneDownload();
    }
}

bool spawnDownloadTask(void)
{
    static const uint32_t kSizes[] = {20 * 1024, 16 * 1024, 12 * 1024, 10 * 1024};
    // Core 0 with Wi-Fi/LwIP: TLS from the other core often returns HTTP -1.
    return staticTaskSpawn(&s_task, &s_mem, downloadTask, "httpdl", nullptr, 4, 0, kSizes,
                           sizeof kSizes / sizeof kSizes[0], "httpdl") != nullptr;
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
    if (strlen(destPath) >= sizeof s_dest || strlen(destPath) + 5 >= 200)
    {
        setError("destination path is too long");
        return false;
    }

    snprintf(s_url, sizeof s_url, "%s", url);
    snprintf(s_dest, sizeof s_dest, "%s", destPath);
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
