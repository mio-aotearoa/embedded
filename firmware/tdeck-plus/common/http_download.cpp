// SPDX-License-Identifier: GPL-3.0-or-later
#include "http_download.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <stdio.h>
#include <string.h>

namespace
{
HttpDownloadState s_state;
TaskHandle_t s_task = nullptr;
volatile bool s_cancel = false;

char s_url[320];
char s_dest[144];
char s_tmp[160];
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
    s_task = nullptr;
}

void removePartial()
{
    if (s_tmp[0])
    {
        SD.remove(s_tmp);
    }
}

void downloadTask(void *)
{
    if (WiFi.status() != WL_CONNECTED)
    {
        setError("Wi-Fi is not connected");
        finish(false);
        vTaskDelete(nullptr);
        return;
    }

    HTTPClient http;
    WiFiClientSecure tls;
    WiFiClient plain;
    char current[320];
    snprintf(current, sizeof current, "%s", s_url);
    int length = 0;

    // Follow redirects by hand. Both hosts this firmware pulls from (Hugging
    // Face, the Kiwix mirror) 302 to a DIFFERENT host, and HTTPClient cannot
    // re-connect a caller-supplied WiFiClientSecure across hosts: its own
    // redirect following aborts with HTTP -1 ("connection refused"), which is
    // exactly what the ESP-AI model download was reporting.
    for (int hop = 0;; ++hop)
    {
        http.setReuse(false);
        http.setConnectTimeout(15000);
        http.setTimeout(30000);
        http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
        http.setUserAgent("wadamesh-tdeck");

        const bool https = strncmp(current, "https://", 8) == 0;
        bool began = false;
        if (https)
        {
            tls.setInsecure();  // no cert store on the device; see docs/architecture.md
            began = http.begin(tls, current);
        }
        else
        {
            began = http.begin(plain, current);
        }
        if (!began)
        {
            setError("cannot parse the URL");
            finish(false);
            vTaskDelete(nullptr);
            return;
        }

        http.addHeader("Accept-Encoding", "identity");
        const int code = http.GET();
        s_state.http_code = code;
        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308)
        {
            String loc = http.header("Location");
            http.end();
            if (!loc.length() || hop >= 4)
            {
                setError(loc.length() ? "too many redirects" : "redirect without a Location");
                finish(false);
                vTaskDelete(nullptr);
                return;
            }
            if (loc.startsWith("http"))
            {
                snprintf(current, sizeof current, "%s", loc.c_str());
            }
            else
            {
                // Relative Location: resolve against the host we just talked to.
                const char* scheme = strstr(current, "://");
                const char* slash = scheme ? strchr(scheme + 3, '/') : nullptr;
                const size_t base = slash ? (size_t)(slash - current) : strlen(current);
                char next[320];
                snprintf(next, sizeof next, "%.*s%s", (int)base, current, loc.c_str());
                snprintf(current, sizeof current, "%s", next);
            }
            continue;
        }
        if (code != HTTP_CODE_OK)
        {
            snprintf(s_state.err, sizeof s_state.err, "HTTP %d (%s)", code,
                     HTTPClient::errorToString(code).c_str());
            http.end();
            finish(false);
            vTaskDelete(nullptr);
            return;
        }
        length = http.getSize();
        s_state.total = length > 0 ? (uint64_t)length : 0;
        break;
    }

    File out = SD.open(s_tmp, FILE_WRITE);
    if (!out)
    {
        setError("cannot write to the SD card");
        http.end();
        finish(false);
        vTaskDelete(nullptr);
        return;
    }

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
        s_state.bytes += (uint64_t)got;
        if (remaining > 0)
        {
            remaining -= got;
        }
    }

    out.close();
    http.end();

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
        vTaskDelete(nullptr);
        return;
    }

    // Publish atomically: only now does the file exist under its real name.
    SD.remove(s_dest);
    if (!SD.rename(s_tmp, s_dest))
    {
        setError("cannot rename the finished download");
        removePartial();
        finish(false);
        vTaskDelete(nullptr);
        return;
    }
    finish(true);
    vTaskDelete(nullptr);
}
}  // namespace

bool httpDownloadStart(const char *url, const char *destPath, uint64_t expectedSize)
{
    if (s_state.running || !url || !destPath)
    {
        return false;
    }
    const bool http = strncmp(url, "http://", 7) == 0;
    const bool https = strncmp(url, "https://", 8) == 0;
    if (!http && !https)
    {
        return false;
    }
    if (strlen(url) >= sizeof s_url || strlen(destPath) >= sizeof s_dest)
    {
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

    // 16 KB/4 like the Reader's fetch, which does the same HTTPS GET and works.
    // At 12 KB/2 the handshake failed with HTTPClient -1 ("connection refused"),
    // which is what the ESP-AI model download reported.
    if (xTaskCreate(downloadTask, "httpdl", 16384, nullptr, 4, &s_task) != pdPASS)
    {
        s_state.running = false;
        s_task = nullptr;
        return false;
    }
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
