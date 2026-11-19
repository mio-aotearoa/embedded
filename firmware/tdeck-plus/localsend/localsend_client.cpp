// SPDX-License-Identifier: GPL-3.0-or-later
//
// LocalSend protocol v2 over plain HTTP. See localsend_client.h for scope.
//
// Requests are built and parsed by hand on purpose: the protocol needs a few
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

bool jsonUint(const char *json, const char *key, uint32_t *out)
{
    char needle[40];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ' || *p == ':') p++;
    char *end = nullptr;
    const unsigned long v = strtoul(p, &end, 10);
    if (end == p) return false;
    *out = (uint32_t)v;
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

void fillInfoJson(char *out, size_t cap, bool withPort)
{
    if (withPort)
    {
        snprintf(out, cap,
                 "{\"alias\":\"T-Deck\",\"version\":\"2.0\",\"deviceModel\":\"T-Deck Plus\","
                 "\"deviceType\":\"mobile\",\"fingerprint\":\"%s\",\"port\":%u,"
                 "\"protocol\":\"http\",\"download\":false}",
                 fingerprint(), (unsigned)kPort);
    }
    else
    {
        snprintf(out, cap,
                 "{\"alias\":\"T-Deck\",\"version\":\"2.0\",\"deviceModel\":\"T-Deck Plus\","
                 "\"deviceType\":\"mobile\",\"fingerprint\":\"%s\",\"download\":false}",
                 fingerprint());
    }
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
    snprintf(body, sizeof body,
             "{\"alias\":\"T-Deck\",\"version\":\"2.0\",\"deviceModel\":\"T-Deck Plus\","
             "\"deviceType\":\"mobile\",\"fingerprint\":\"%s\",\"port\":%u,"
             "\"protocol\":\"http\",\"download\":false,\"announce\":true}",
             fingerprint(), (unsigned)kPort);
    udp.beginPacket(IPAddress(224, 0, 0, 167), kPort);
    udp.write((const uint8_t *)body, strlen(body));
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

    const char *files = strstr(body, "\"files\"");
    if (!files)
    {
        httpRespond(c, 400, "text/plain", "no files");
        return;
    }

    // First file object after "files": { "id": { ... } }
    const char *brace = strchr(files, '{');
    if (!brace)
    {
        httpRespond(c, 400, "text/plain", "bad files");
        return;
    }
    const char *idQuote = strchr(brace + 1, '"');
    if (!idQuote)
    {
        httpRespond(c, 400, "text/plain", "no file id");
        return;
    }
    idQuote++;
    char fileId[24];
    size_t i = 0;
    while (*idQuote && *idQuote != '"' && i + 1 < sizeof fileId) fileId[i++] = *idQuote++;
    fileId[i] = 0;

    const char *obj = strchr(idQuote, '{');
    if (!obj)
    {
        httpRespond(c, 400, "text/plain", "no file meta");
        return;
    }

    char fileName[96];
    uint32_t size = 0;
    fileName[0] = 0;
    jsonString(obj, "fileName", fileName, sizeof fileName);
    jsonUint(obj, "size", &size);
    if (!fileName[0] || size == 0)
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

    char escapedId[48];
    char escapedTok[80];
    jsonEscape(fileId, escapedId, sizeof escapedId);
    jsonEscape(token, escapedTok, sizeof escapedTok);
    char resp[256];
    snprintf(resp, sizeof resp, "{\"sessionId\":\"%s\",\"files\":{\"%s\":\"%s\"}}",
             sessionId, escapedId, escapedTok);
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
    char tmp[104];
    snprintf(dest, sizeof dest, "/localsend/%s", s_session.fileName);
    snprintf(tmp, sizeof tmp, "%s.part", dest);

    File out = SD.open(tmp, FILE_WRITE);
    if (!out)
    {
        httpRespond(c, 500, "text/plain", "sd open failed");
        snprintf(s_recv.err, sizeof s_recv.err, "%s", "Cannot write to the SD card");
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

    // Headers already consumed; stream the body.
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
        if (out.write(buf, (size_t)got) != (size_t)got)
        {
            ok = false;
            snprintf(s_recv.err, sizeof s_recv.err, "%s", "SD write failed");
            break;
        }
        left -= (size_t)got;
        s_recv.received = (uint32_t)(contentLength - left);
    }
    out.close();

    if (ok && left == 0)
    {
        SD.remove(dest);
        if (!SD.rename(tmp, dest))
        {
            ok = false;
            snprintf(s_recv.err, sizeof s_recv.err, "%s", "Cannot finalise the file");
            SD.remove(tmp);
        }
    }
    else
    {
        SD.remove(tmp);
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
        fillInfoJson(body, sizeof body, false);
        httpRespond(c, 200, "application/json", body);
    }
    else if (strcmp(method, "POST") == 0 && strcmp(pathOnly, "/api/localsend/v2/register") == 0)
    {
        char body[768];
        if (contentLength > 0 && contentLength < sizeof body)
            readBody(c, contentLength, body, sizeof body);
        else
            body[0] = 0;
        char alias[33];
        if (jsonString(body, "alias", alias, sizeof alias) && alias[0])
            addPeer(remote, alias, kPort);
        char resp[320];
        fillInfoJson(resp, sizeof resp, false);
        httpRespond(c, 200, "application/json", resp);
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
                    char alias[33] = {0};
                    jsonString(buf, "alias", alias, sizeof alias);
                    if (alias[0]) addPeer(udp.remoteIP(), alias, (uint16_t)kPort);
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
                char alias[33];
                char protocol[8] = "http";
                jsonString(body.c_str(), "alias", alias, sizeof alias);
                jsonString(body.c_str(), "protocol", protocol, sizeof protocol);
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
        "\"download\":false},\"files\":{\"1\":{\"id\":\"1\",\"fileName\":\"%s\",\"size\":%u,"
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
