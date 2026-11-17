// SPDX-License-Identifier: GPL-3.0-or-later
//
// LocalSend protocol v2 over plain HTTP. See localsend_client.h for scope.
//
// Requests are built and parsed by hand on purpose: the protocol needs three
// small JSON documents, and doing it here keeps the app free of a JSON library
// the rest of this firmware does not otherwise use for uploads.
#include "localsend_client.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <stdio.h>
#include <string.h>

namespace
{
constexpr uint16_t kPort = 53317;
constexpr int kMaxPeers = 16;
constexpr size_t kChunk = 4096;

LsPeer s_peers[kMaxPeers];
volatile int s_peerCount = 0;
volatile bool s_scanning = false;
volatile int s_scanProgress = 255;

LocalSendClient::State s_state = {};

volatile bool s_sendBusy = false;
volatile bool s_cancelSend = false;
volatile bool s_cancelScan = false;

void jsonEscape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 7 < cap; i++)
    {
        const unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\')
        {
            out[o++] = '\\';
            out[o++] = (char)c;
        }
        else if (c < 0x20)
        {
            o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
        }
        else
        {
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
}

void urlEncode(const char *in, char *out, size_t cap)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 4 < cap; i++)
    {
        const unsigned char c = (unsigned char)in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
        {
            out[o++] = (char)c;
        }
        else
        {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        }
    }
    out[o] = 0;
}

// Pull a string field out of a flat JSON object. Good enough for the protocol's
// documented shape; returns false when the key is absent.
bool jsonString(const char *json, const char *key, char *out, size_t cap)
{
    char needle[40];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return false;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < cap)
    {
        if (*p == '\\' && p[1]) p++;
        out[o++] = *p++;
    }
    out[o] = 0;
    return true;
}

const char *fingerprint()
{
    static char fp[33];
    if (fp[0]) return fp;
    uint8_t mac[6] = {0};
    WiFi.macAddress(mac);
    snprintf(fp, sizeof fp, "tdeck-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return fp;
}

void addPeer(const IPAddress &ip, const char *alias, uint16_t port)
{
    if (!alias || !alias[0]) return;
    char ipText[16];
    snprintf(ipText, sizeof ipText, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);

    const int n = s_peerCount;
    for (int i = 0; i < n; i++)
    {
        if (strcmp(s_peers[i].ip, ipText) == 0 && s_peers[i].port == port) return;
    }
    if (n >= kMaxPeers) return;

    snprintf(s_peers[n].alias, sizeof s_peers[n].alias, "%s", alias);
    snprintf(s_peers[n].ip, sizeof s_peers[n].ip, "%s", ipText);
    s_peers[n].port = port;
    __sync_synchronize();
    s_peerCount = n + 1;
}

// Writes a request head plus (optionally) a body/stream, then reads the response
// status and body. Returns the HTTP status, or -1 when the socket did not open.
int httpRequest(const IPAddress &ip, uint16_t port, const char *path, const char *ctype,
                const char *body, size_t bodyLen, File *stream, size_t streamLen,
                char *resp, size_t respCap)
{
    WiFiClient client;
    if (!client.connect(ip, port, 1500)) return -1;

    char host[16];
    snprintf(host, sizeof host, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    const size_t len = stream ? streamLen : bodyLen;

    client.printf("POST %s HTTP/1.1\r\nHost: %s:%u\r\nContent-Type: %s\r\n"
                  "Content-Length: %u\r\nConnection: close\r\n\r\n",
                  path, host, (unsigned)port, ctype, (unsigned)len);

    if (stream)
    {
        uint8_t buf[kChunk];
        size_t left = streamLen;
        while (left > 0 && !s_cancelSend)
        {
            const size_t want = left < sizeof buf ? left : sizeof buf;
            const int got = stream->read(buf, want);
            if (got <= 0) break;
            if (client.write(buf, (size_t)got) != (size_t)got) break;
            left -= (size_t)got;
            s_state.sent = (uint32_t)(streamLen - left);
        }
        if (left != 0)
        {
            client.stop();
            snprintf(s_state.err, sizeof s_state.err, "%s",
                     s_cancelSend ? "Cancelled" : "The card stopped reading");
            return -2;
        }
    }
    else if (bodyLen > 0)
    {
        client.write((const uint8_t *)body, bodyLen);
    }

    const uint32_t deadline = millis() + 6000;
    size_t used = 0;
    while (millis() < deadline && used + 1 < respCap)
    {
        while (client.available() && used + 1 < respCap)
        {
            resp[used++] = (char)client.read();
        }
        resp[used] = 0;
        if (strstr(resp, "\r\n\r\n")) break;
        if (!client.connected() && !client.available()) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    client.stop();

    int status = -1;
    if (sscanf(resp, "HTTP/%*d.%*d %d", &status) != 1) return -1;
    return status;
}

void discover(void *arg)
{
    (void)arg;
    s_scanProgress = 0;

    // 1) Multicast announcements: a peer that starts (or re-announces) while we
    // listen tells us its alias and port.
    WiFiUDP udp;
    if (udp.begin(kPort))
    {
        udp.beginMulticast(IPAddress(224, 0, 0, 167), kPort);
        const uint32_t until = millis() + 3000;
        char buf[768];
        while (!s_cancelScan && millis() < until)
        {
            const int len = udp.parsePacket();
            if (len > 0)
            {
                const int n = udp.read(buf, sizeof buf - 1);
                if (n > 0)
                {
                    buf[n] = 0;
                    char alias[33] = {0};
                    jsonString(buf, "alias", alias, sizeof alias);
                    if (alias[0]) addPeer(udp.remoteIP(), alias, (uint16_t)kPort);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        udp.stop();
    }

    // 2) HTTP fallback for networks that filter multicast: ask each host.
    const IPAddress local = WiFi.localIP();
    if (local[0] != 0)
    {
        HTTPClient http;
        http.setConnectTimeout(150);
        http.setTimeout(400);
        for (int h = 1; h < 255 && !s_cancelScan; h++)
        {
            s_scanProgress = h;
            if (h == local[3]) continue;
            IPAddress ip(local[0], local[1], local[2], (uint8_t)h);
            char url[72];
            snprintf(url, sizeof url, "http://%u.%u.%u.%u:%u/api/localsend/v2/info",
                     ip[0], ip[1], ip[2], ip[3], (unsigned)kPort);
            if (!http.begin(url)) continue;
            const int code = http.GET();
            if (code == 200)
            {
                const String body = http.getString();
                char alias[33];
                char protocol[8] = "http";
                jsonString(body.c_str(), "alias", alias, sizeof alias);
                jsonString(body.c_str(), "protocol", protocol, sizeof protocol);
                // We speak plain HTTP only, so an HTTPS-only peer is not listed
                // rather than failing later at prepare-upload.
                if (strcmp(protocol, "http") == 0) addPeer(ip, alias, kPort);
            }
            http.end();
            vTaskDelay(1);
        }
    }

    s_scanProgress = 255;
    s_scanning = false;
    vTaskDelete(nullptr);
}

void sendTask(void *arg)
{
    struct Job
    {
        char path[128];
        LsPeer peer;
    };
    Job *job = static_cast<Job *>(arg);
    struct BusyGuard
    {
        ~BusyGuard() { s_sendBusy = false; }
    } guard;

    s_state.running = true;
    s_state.done = false;
    s_state.ok = false;
    s_state.sent = 0;
    s_state.total = 0;
    s_state.err[0] = 0;
    s_cancelSend = false;

    File file = SD.open(job->path, FILE_READ);
    if (!file || file.isDirectory())
    {
        snprintf(s_state.err, sizeof s_state.err, "%s", "Cannot open the file");
        s_state.running = false;
        s_state.done = true;
        delete job;
        vTaskDelete(nullptr);
        return;
    }

    const size_t size = (size_t)file.size();
    s_state.total = (uint32_t)size;

    const char *base = strrchr(job->path, '/');
    base = base ? base + 1 : job->path;
    char escaped[96];
    jsonEscape(base, escaped, sizeof escaped);

    char body[512];
    const int bodyLen = snprintf(
        body, sizeof body,
        "{\"info\":{\"alias\":\"T-Deck\",\"version\":\"2.0\",\"deviceModel\":\"T-Deck Plus\","
        "\"deviceType\":\"mobile\",\"fingerprint\":\"%s\",\"port\":%u,\"protocol\":\"http\","
        "\"download\":false},\"files\":{\"1\":{\"fileName\":\"%s\",\"size\":%u,"
        "\"fileType\":\"application/octet-stream\"}}}",
        fingerprint(), (unsigned)kPort, escaped, (unsigned)size);

    char resp[768];
    resp[0] = 0;
    char ipText[16];
    snprintf(ipText, sizeof ipText, "%s", job->peer.ip);
    IPAddress peerIp;
    if (!peerIp.fromString(ipText))
    {
        snprintf(s_state.err, sizeof s_state.err, "%s", "Bad peer address");
        file.close();
        s_state.running = false;
        s_state.done = true;
        delete job;
        vTaskDelete(nullptr);
        return;
    }

    const int prep = httpRequest(peerIp, job->peer.port, "/api/localsend/v2/prepare-upload",
                                 "application/json", body, (size_t)bodyLen, nullptr, 0,
                                 resp, sizeof resp);
    if (prep != 200)
    {
        snprintf(s_state.err, sizeof s_state.err, "%s (%d)", "The receiver refused", prep);
        file.close();
        s_state.running = false;
        s_state.done = true;
        delete job;
        vTaskDelete(nullptr);
        return;
    }

    char sessionId[64];
    char token[64];
    sessionId[0] = token[0] = 0;
    jsonString(resp, "sessionId", sessionId, sizeof sessionId);
    const char *files = strstr(resp, "\"files\"");
    if (files) jsonString(files, "1", token, sizeof token);

    if (!sessionId[0] || !token[0])
    {
        snprintf(s_state.err, sizeof s_state.err, "%s", "The receiver sent no upload token");
        file.close();
        s_state.running = false;
        s_state.done = true;
        delete job;
        vTaskDelete(nullptr);
        return;
    }

    char sid[192];
    char tok[192];
    urlEncode(sessionId, sid, sizeof sid);
    urlEncode(token, tok, sizeof tok);
    char path[512];
    snprintf(path, sizeof path,
             "/api/localsend/v2/upload?sessionId=%s&fileId=1&token=%s", sid, tok);

    resp[0] = 0;
    const int up = httpRequest(peerIp, job->peer.port, path, "application/octet-stream",
                               nullptr, 0, &file, size, resp, sizeof resp);
    file.close();

    s_state.running = false;
    s_state.done = true;
    s_state.ok = (up == 200);
    if (up == 200)
    {
        s_state.sent = (uint32_t)size;
    }
    else if (up == -2)
    {
        // err already set by httpRequest
    }
    else
    {
        snprintf(s_state.err, sizeof s_state.err, "%s (%d)", "Upload failed", up);
        // Tell the receiver to drop the session rather than leave it pending.
        char cancelPath[256];
        snprintf(cancelPath, sizeof cancelPath, "/api/localsend/v2/cancel?sessionId=%s", sid);
        httpRequest(peerIp, job->peer.port, cancelPath, "application/json", "{}", 2,
                    nullptr, 0, resp, sizeof resp);
    }

    delete job;
    vTaskDelete(nullptr);
}
}  // namespace

bool LocalSendClient::startScan(void)
{
    if (s_scanning) return true;
    if (WiFi.status() != WL_CONNECTED) return false;
    s_peerCount = 0;
    s_scanProgress = 0;
    s_cancelScan = false;
    s_scanning = true;
    if (xTaskCreatePinnedToCore(discover, "localsend-scan", 6144, nullptr, 1, nullptr, 0) != pdPASS)
    {
        s_scanning = false;
        return false;
    }
    return true;
}

void LocalSendClient::cancelScan(void) { s_cancelScan = true; }
bool LocalSendClient::scanning(void) { return s_scanning; }
int LocalSendClient::scanProgress(void) { return s_scanProgress; }
int LocalSendClient::peerCount(void) { return s_peerCount; }

bool LocalSendClient::peer(int index, LsPeer &out)
{
    if (index < 0 || index >= s_peerCount) return false;
    __sync_synchronize();
    out = s_peers[index];
    return true;
}

bool LocalSendClient::beginSend(const char *sdPath, int peerIndex)
{
    if (s_sendBusy || !sdPath || !sdPath[0]) return false;
    if (WiFi.status() != WL_CONNECTED) return false;

    struct Job
    {
        char path[128];
        LsPeer peer;
    };
    Job *job = new Job();
    if (!job) return false;
    snprintf(job->path, sizeof job->path, "%s", sdPath);
    if (!peer(peerIndex, job->peer))
    {
        delete job;
        return false;
    }

    s_sendBusy = true;
    if (xTaskCreatePinnedToCore(sendTask, "localsend-tx", 9216, job, 1, nullptr, 0) != pdPASS)
    {
        s_sendBusy = false;
        delete job;
        return false;
    }
    return true;
}

void LocalSendClient::cancelSend(void) { s_cancelSend = true; }

const LocalSendClient::State &LocalSendClient::state(void) { return s_state; }

void LocalSendClient::clearDone(void)
{
    if (s_state.running) return;
    s_state.done = false;
    s_state.ok = false;
    s_state.sent = 0;
    s_state.total = 0;
    s_state.err[0] = 0;
}
