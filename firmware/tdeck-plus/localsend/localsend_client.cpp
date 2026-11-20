// SPDX-License-Identifier: GPL-3.0-or-later
//
// LocalSend protocol v2 over plain HTTP. See localsend_client.h for scope.
#include "localsend_client.h"

#include "psram_alloc.h"
#include "sd_atomic.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <stdio.h>
#include <stdlib.h>
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
LocalSendClient::RecvState s_recv = {};

volatile bool s_sendBusy = false;
volatile bool s_cancelSend = false;
volatile bool s_cancelScan = false;

volatile bool s_recvWant = false;
volatile bool s_recvRunning = false;
TaskHandle_t s_recvTask = nullptr;

struct RecvSession
{
    bool active;
    char sessionId[40];
    char fileId[24];
    char token[40];
    char fileName[64];
    char peerIp[16];
    size_t size;
};
RecvSession s_session = {};

struct LsJsonAlloc : ArduinoJson::Allocator
{
    void *allocate(size_t n) override { return psramPreferAlloc(n); }
    void deallocate(void *p) override { psramPreferFree(p); }
    void *reallocate(void *p, size_t n) override
    {
        void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        return q ? q : realloc(p, n);
    }
};

LsJsonAlloc &lsJsonAlloc()
{
    static LsJsonAlloc alloc;
    return alloc;
}

bool parseJson(const char *json, JsonDocument &doc)
{
    if (!json || !json[0]) return false;
    return deserializeJson(doc, json) == DeserializationError::Ok;
}

const char *httpBody(const char *resp)
{
    if (!resp) return "";
    const char *p = strstr(resp, "\r\n\r\n");
    return p ? p + 4 : resp;
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

void fillDeviceInfo(JsonObject obj, bool withPort, bool announce)
{
    obj["alias"] = "T-Deck";
    obj["version"] = "2.0";
    obj["deviceModel"] = "T-Deck Plus";
    obj["deviceType"] = "mobile";
    obj["fingerprint"] = fingerprint();
    obj["protocol"] = "http";
    obj["download"] = false;
    if (withPort) obj["port"] = kPort;
    if (announce) obj["announce"] = true;
}

bool fillInfoJson(char *out, size_t cap, bool withPort)
{
    JsonDocument doc(&lsJsonAlloc());
    fillDeviceInfo(doc.to<JsonObject>(), withPort, false);
    return serializeJson(doc, out, cap) > 0;
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

bool urlDecodeInPlace(char *s)
{
    char *r = s;
    char *w = s;
    while (*r)
    {
        if (*r == '%' && r[1] && r[2])
        {
            char hex[3] = {r[1], r[2], 0};
            *w++ = (char)strtol(hex, nullptr, 16);
            r += 3;
        }
        else if (*r == '+')
        {
            *w++ = ' ';
            r++;
        }
        else
        {
            *w++ = *r++;
        }
    }
    *w = 0;
    return true;
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

void announceMulticast(void)
{
    WiFiUDP udp;
    if (!udp.begin(0)) return;
    char body[384];
    JsonDocument doc(&lsJsonAlloc());
    fillDeviceInfo(doc.to<JsonObject>(), true, true);
    const size_t n = serializeJson(doc, body, sizeof body);
    if (n == 0)
    {
        udp.stop();
        return;
    }
    udp.beginPacket(IPAddress(224, 0, 0, 167), kPort);
    udp.write((const uint8_t *)body, n);
    udp.endPacket();
    udp.stop();
}

void safeFileName(const char *in, char *out, size_t cap)
{
    const char *base = strrchr(in, '/');
    base = base ? base + 1 : in;
    size_t o = 0;
    for (size_t i = 0; base[i] && o + 1 < cap; i++)
    {
        const unsigned char c = (unsigned char)base[i];
        if (c < 0x20 || c == '"' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '<' || c == '>' || c == '|')
        {
            out[o++] = '_';
        }
        else
        {
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
    if (!out[0]) snprintf(out, cap, "received.bin");
}

void makeIds(char *sessionId, size_t scam, char *token, size_t tcap)
{
    const uint32_t a = (uint32_t)esp_random();
    const uint32_t b = (uint32_t)esp_random();
    snprintf(sessionId, scam, "td%08lx%08lx", (unsigned long)a, (unsigned long)b);
    snprintf(token, tcap, "tk%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
}

bool queryParam(const char *path, const char *key, char *out, size_t cap)
{
    const char *q = strchr(path, '?');
    if (!q) return false;
    q++;
    const size_t klen = strlen(key);
    while (*q)
    {
        if (strncmp(q, key, klen) == 0 && q[klen] == '=')
        {
            q += klen + 1;
            size_t o = 0;
            while (*q && *q != '&' && o + 1 < cap) out[o++] = *q++;
            out[o] = 0;
            urlDecodeInPlace(out);
            return true;
        }
        while (*q && *q != '&') q++;
        if (*q == '&') q++;
    }
    return false;
}

void httpRespond(WiFiClient &c, int code, const char *ctype, const char *body)
{
    const char *reason = "OK";
    if (code == 204) reason = "No Content";
    else if (code == 400) reason = "Bad Request";
    else if (code == 403) reason = "Forbidden";
    else if (code == 404) reason = "Not Found";
    else if (code == 409) reason = "Conflict";
    else if (code == 500) reason = "Error";
    const size_t len = body ? strlen(body) : 0;
    c.printf("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
             "Connection: close\r\n\r\n",
             code, reason, ctype ? ctype : "text/plain", (unsigned)len);
    if (len) c.write((const uint8_t *)body, len);
}

// Read request line + headers. Leaves contentLength set; body start is next.
bool readHeaders(WiFiClient &c, char *method, size_t mcap, char *path, size_t pcap,
                 size_t *contentLength)
{
    method[0] = path[0] = 0;
    *contentLength = 0;
    char line[512];
    size_t n = 0;
    const uint32_t deadline = millis() + 4000;
    // Request line
    while (millis() < deadline && n + 1 < sizeof line)
    {
        if (!c.available())
        {
            if (!c.connected()) return false;
            vTaskDelay(1);
            continue;
        }
        const char ch = (char)c.read();
        if (ch == '\n')
        {
            line[n] = 0;
            break;
        }
        if (ch != '\r') line[n++] = ch;
    }
    if (sscanf(line, "%15s %255s", method, path) < 2) return false;

    // Headers
    n = 0;
    while (millis() < deadline)
    {
        if (!c.available())
        {
            if (!c.connected() && !c.available()) break;
            vTaskDelay(1);
            continue;
        }
        const char ch = (char)c.read();
        if (ch == '\n')
        {
            line[n] = 0;
            if (n == 0 || (n == 1 && line[0] == '\r')) break;  // empty line
            if (strncasecmp(line, "Content-Length:", 15) == 0)
            {
                *contentLength = (size_t)strtoul(line + 15, nullptr, 10);
            }
            n = 0;
            continue;
        }
        if (ch != '\r' && n + 1 < sizeof line) line[n++] = ch;
    }
    return method[0] != 0 && path[0] != 0;
}

bool readBody(WiFiClient &c, size_t contentLength, char *buf, size_t cap)
{
    if (contentLength + 1 > cap) return false;
    size_t got = 0;
    const uint32_t deadline = millis() + 8000;
    while (got < contentLength && millis() < deadline)
    {
        if (!c.available())
        {
            if (!c.connected() && !c.available()) break;
            vTaskDelay(1);
            continue;
        }
        const int n = c.read((uint8_t *)buf + got, contentLength - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    buf[got] = 0;
    return got == contentLength;
}

void clearSession(void)
{
    memset(&s_session, 0, sizeof s_session);
}

void handlePrepareUpload(WiFiClient &c, const char *body, const IPAddress &remote)
{
    if (s_sendBusy || s_recv.busy || s_session.active)
    {
        httpRespond(c, 409, "text/plain", "busy");
        return;
    }

    JsonDocument req(&lsJsonAlloc());
    if (!parseJson(body, req))
    {
        httpRespond(c, 400, "text/plain", "bad json");
        return;
    }
    JsonObject files = req["files"].as<JsonObject>();
    if (files.isNull() || files.size() == 0)
    {
        httpRespond(c, 400, "text/plain", "no files");
        return;
    }

    const char *fileId = nullptr;
    const char *fileName = nullptr;
    uint32_t size = 0;
    for (JsonPair kv : files)
    {
        fileId = kv.key().c_str();
        JsonObject meta = kv.value().as<JsonObject>();
        fileName = meta["fileName"] | "";
        size = meta["size"] | 0u;
        break;  // one file per transfer
    }
    if (!fileId || !fileId[0] || !fileName || !fileName[0] || size == 0)
    {
        httpRespond(c, 400, "text/plain", "missing name/size");
        return;
    }

    char sessionId[40];
    char token[40];
    makeIds(sessionId, sizeof sessionId, token, sizeof token);

    clearSession();
    s_session.active = true;
    snprintf(s_session.sessionId, sizeof s_session.sessionId, "%s", sessionId);
    snprintf(s_session.fileId, sizeof s_session.fileId, "%s", fileId);
    snprintf(s_session.token, sizeof s_session.token, "%s", token);
    safeFileName(fileName, s_session.fileName, sizeof s_session.fileName);
    s_session.size = size;
    snprintf(s_session.peerIp, sizeof s_session.peerIp, "%u.%u.%u.%u",
             remote[0], remote[1], remote[2], remote[3]);

    s_recv.busy = false;
    s_recv.done = false;
    s_recv.ok = false;
    s_recv.received = 0;
    s_recv.total = size;
    snprintf(s_recv.fileName, sizeof s_recv.fileName, "%s", s_session.fileName);
    s_recv.err[0] = 0;

    JsonDocument respDoc(&lsJsonAlloc());
    respDoc["sessionId"] = sessionId;
    respDoc["files"][s_session.fileId] = token;
    char resp[256];
    if (serializeJson(respDoc, resp, sizeof resp) == 0)
    {
        clearSession();
        httpRespond(c, 500, "text/plain", "json");
        return;
    }
    httpRespond(c, 200, "application/json", resp);
}

void handleUpload(WiFiClient &c, const char *path, size_t contentLength, const IPAddress &remote)
{
    char sessionId[64];
    char fileId[32];
    char token[64];
    sessionId[0] = fileId[0] = token[0] = 0;
    queryParam(path, "sessionId", sessionId, sizeof sessionId);
    queryParam(path, "fileId", fileId, sizeof fileId);
    queryParam(path, "token", token, sizeof token);

    char peer[16];
    snprintf(peer, sizeof peer, "%u.%u.%u.%u", remote[0], remote[1], remote[2], remote[3]);

    if (!s_session.active || strcmp(sessionId, s_session.sessionId) != 0 ||
        strcmp(fileId, s_session.fileId) != 0 || strcmp(token, s_session.token) != 0 ||
        strcmp(peer, s_session.peerIp) != 0)
    {
        httpRespond(c, 403, "text/plain", "invalid token");
        return;
    }

    if (contentLength == 0) contentLength = s_session.size;
    if (contentLength == 0 || contentLength > 32u * 1024u * 1024u)
    {
        httpRespond(c, 400, "text/plain", "bad size");
        clearSession();
        return;
    }

    SD.mkdir("/localsend");
    char dest[96];
    snprintf(dest, sizeof dest, "/localsend/%s", s_session.fileName);

    SdAtomicFile out;
    if (!out.begin(dest, s_recv.err, sizeof s_recv.err))
    {
        httpRespond(c, 500, "text/plain", "sd open failed");
        s_recv.done = true;
        s_recv.ok = false;
        clearSession();
        return;
    }

    s_recv.busy = true;
    s_recv.done = false;
    s_recv.ok = false;
    s_recv.received = 0;
    s_recv.total = (uint32_t)contentLength;
    s_recv.err[0] = 0;

    uint8_t buf[kChunk];
    size_t left = contentLength;
    bool ok = true;
    const uint32_t deadline = millis() + 120000;
    while (left > 0 && millis() < deadline && s_recvWant)
    {
        size_t avail = c.available();
        if (avail == 0)
        {
            if (!c.connected())
            {
                ok = false;
                break;
            }
            vTaskDelay(1);
            continue;
        }
        const size_t want = left < sizeof buf ? left : sizeof buf;
        const size_t take = avail < want ? avail : want;
        const int got = c.read(buf, take);
        if (got <= 0)
        {
            ok = false;
            break;
        }
        if (!out.write(buf, (size_t)got, s_recv.err, sizeof s_recv.err))
        {
            ok = false;
            break;
        }
        left -= (size_t)got;
        s_recv.received = (uint32_t)(contentLength - left);
    }

    if (ok && left == 0)
    {
        if (!out.commit(s_recv.err, sizeof s_recv.err)) ok = false;
    }
    else
    {
        out.abort();
        if (!s_recv.err[0])
            snprintf(s_recv.err, sizeof s_recv.err, "%s",
                     s_recvWant ? "Transfer incomplete" : "Cancelled");
        ok = false;
    }

    if (ok) httpRespond(c, 200, "text/plain", "");
    else httpRespond(c, 500, "text/plain", s_recv.err);

    s_recv.busy = false;
    s_recv.done = true;
    s_recv.ok = ok;
    if (ok) s_recv.received = (uint32_t)contentLength;
    clearSession();
}

void handleClient(WiFiClient &c)
{
    char method[16];
    char path[288];
    size_t contentLength = 0;
    if (!readHeaders(c, method, sizeof method, path, sizeof path, &contentLength))
    {
        c.stop();
        return;
    }

    const IPAddress remote = c.remoteIP();
    const char *q = strchr(path, '?');
    char pathOnly[160];
    size_t pl = 0;
    for (const char *p = path; *p && *p != '?' && pl + 1 < sizeof pathOnly; p++)
        pathOnly[pl++] = *p;
    pathOnly[pl] = 0;
    (void)q;

    if (strcmp(method, "GET") == 0 &&
        (strcmp(pathOnly, "/api/localsend/v2/info") == 0 || strcmp(pathOnly, "/api/localsend/v1/info") == 0))
    {
        char body[320];
        if (fillInfoJson(body, sizeof body, false))
            httpRespond(c, 200, "application/json", body);
        else
            httpRespond(c, 500, "text/plain", "json");
    }
    else if (strcmp(method, "POST") == 0 && strcmp(pathOnly, "/api/localsend/v2/register") == 0)
    {
        char body[768];
        if (contentLength > 0 && contentLength < sizeof body)
            readBody(c, contentLength, body, sizeof body);
        else
            body[0] = 0;
        JsonDocument reg(&lsJsonAlloc());
        if (parseJson(body, reg))
        {
            const char *alias = reg["alias"] | "";
            if (alias[0]) addPeer(remote, alias, kPort);
        }
        char resp[320];
        if (fillInfoJson(resp, sizeof resp, false))
            httpRespond(c, 200, "application/json", resp);
        else
            httpRespond(c, 500, "text/plain", "json");
    }
    else if (strcmp(method, "POST") == 0 &&
             strcmp(pathOnly, "/api/localsend/v2/prepare-upload") == 0)
    {
        char body[1536];
        if (contentLength == 0 || contentLength >= sizeof body ||
            !readBody(c, contentLength, body, sizeof body))
        {
            httpRespond(c, 400, "text/plain", "bad body");
        }
        else
        {
            handlePrepareUpload(c, body, remote);
        }
    }
    else if (strcmp(method, "POST") == 0 && strcmp(pathOnly, "/api/localsend/v2/upload") == 0)
    {
        handleUpload(c, path, contentLength, remote);
    }
    else if (strcmp(method, "POST") == 0 && strcmp(pathOnly, "/api/localsend/v2/cancel") == 0)
    {
        char sessionId[64];
        sessionId[0] = 0;
        queryParam(path, "sessionId", sessionId, sizeof sessionId);
        if (s_session.active && sessionId[0] && strcmp(sessionId, s_session.sessionId) == 0)
        {
            clearSession();
            s_recv.busy = false;
            s_recv.done = true;
            s_recv.ok = false;
            snprintf(s_recv.err, sizeof s_recv.err, "%s", "Cancelled by sender");
        }
        httpRespond(c, 200, "text/plain", "");
    }
    else
    {
        httpRespond(c, 404, "text/plain", "not found");
    }
    c.stop();
}

void receiverTask(void *arg)
{
    (void)arg;
    WiFiServer server(kPort);
    server.begin();
    s_recv.listening = true;
    s_recvRunning = true;
    announceMulticast();
    uint32_t lastAnnounce = millis();

    while (s_recvWant)
    {
        if (millis() - lastAnnounce > 15000)
        {
            announceMulticast();
            lastAnnounce = millis();
        }
        WiFiClient client = server.available();
        if (client)
        {
            client.setTimeout(20);
            handleClient(client);
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    server.end();
    clearSession();
    s_recv.listening = false;
    s_recv.busy = false;
    s_recvRunning = false;
    s_recvTask = nullptr;
    vTaskDelete(nullptr);
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
                    JsonDocument doc(&lsJsonAlloc());
                    if (parseJson(buf, doc))
                    {
                        const char *alias = doc["alias"] | "";
                        if (alias[0]) addPeer(udp.remoteIP(), alias, (uint16_t)kPort);
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        udp.stop();
    }

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
                JsonDocument doc(&lsJsonAlloc());
                if (parseJson(body.c_str(), doc))
                {
                    const char *alias = doc["alias"] | "";
                    const char *protocol = doc["protocol"] | "http";
                    if (alias[0] && strcmp(protocol, "http") == 0) addPeer(ip, alias, kPort);
                }
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

    char body[640];
    size_t bodyLen = 0;
    {
        JsonDocument prepDoc(&lsJsonAlloc());
        fillDeviceInfo(prepDoc["info"].to<JsonObject>(), true, false);
        JsonObject fileMeta = prepDoc["files"]["1"].to<JsonObject>();
        fileMeta["id"] = "1";
        fileMeta["fileName"] = base;
        fileMeta["size"] = (uint32_t)size;
        fileMeta["fileType"] = "application/octet-stream";
        bodyLen = serializeJson(prepDoc, body, sizeof body);
        if (bodyLen == 0)
        {
            snprintf(s_state.err, sizeof s_state.err, "%s", "Cannot build prepare-upload");
            file.close();
            s_state.running = false;
            s_state.done = true;
            delete job;
            vTaskDelete(nullptr);
            return;
        }
    }

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
                                 "application/json", body, bodyLen, nullptr, 0,
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
    {
        JsonDocument ack(&lsJsonAlloc());
        if (parseJson(httpBody(resp), ack))
        {
            const char *sid = ack["sessionId"] | "";
            const char *tok = ack["files"]["1"] | "";
            if (sid[0]) snprintf(sessionId, sizeof sessionId, "%s", sid);
            if (tok[0]) snprintf(token, sizeof token, "%s", tok);
        }
    }

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
    // UDP discovery wants port 53317; if the receiver holds TCP only, UDP can
    // still bind. If scan's udp.begin(kPort) fails because something else owns
    // it, the /24 HTTP sweep still finds peers — including ourselves skipped.
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
    if (s_sendBusy || s_recv.busy || !sdPath || !sdPath[0]) return false;
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

bool LocalSendClient::startReceiver(void)
{
    if (s_recvRunning || s_recvWant) return true;
    if (WiFi.status() != WL_CONNECTED) return false;
    s_recvWant = true;
    memset((void *)&s_recv, 0, sizeof s_recv);
    if (xTaskCreatePinnedToCore(receiverTask, "localsend-rx", 10240, nullptr, 1, &s_recvTask,
                                0) != pdPASS)
    {
        s_recvWant = false;
        return false;
    }
    return true;
}

void LocalSendClient::stopReceiver(void)
{
    s_recvWant = false;
    // Task clears itself; wait briefly so the port is free for the next open.
    for (int i = 0; i < 50 && s_recvRunning; i++) vTaskDelay(pdMS_TO_TICKS(20));
}

const LocalSendClient::RecvState &LocalSendClient::recvState(void) { return s_recv; }

void LocalSendClient::clearRecvDone(void)
{
    if (s_recv.busy) return;
    s_recv.done = false;
    s_recv.ok = false;
    s_recv.received = 0;
    s_recv.total = 0;
    s_recv.fileName[0] = 0;
    s_recv.err[0] = 0;
}
