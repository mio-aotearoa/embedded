// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared HTTP(S) GET with manual redirect following. HTTPClient cannot reconnect
// a caller-supplied WiFiClientSecure across hosts, so every app that fetches
// over TLS (downloads, Kiwix catalogue, Browser) uses this instead of copying
// the hop loop.
#pragma once

#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include <stddef.h>
#include <stdint.h>

// Point mbedtls record buffers at PSRAM (Arduino-ESP32 otherwise needs ~32 KB
// contiguous internal RAM and HTTPS fails as HTTP -1 with the UI up). Idempotent.
void httpTlsUsePsram(void);

// Resolve a Location header against the request URL. Absolute http(s) locs copy
// through; relative locs keep the previous origin.
bool httpResolveRedirect(const char *baseUrl, const char *loc, char *out, size_t outCap);

// One GET session: follow redirects, leave the connection open on HTTP 200.
class HttpFetch
{
  public:
    HttpFetch() = default;
    ~HttpFetch() { close(); }
    HttpFetch(const HttpFetch &) = delete;
    HttpFetch &operator=(const HttpFetch &) = delete;

    // `urlBuf` must be writable; on success it holds the final URL after redirects.
    // `url` may alias `urlBuf`.
    bool openGet(const char *url, char *urlBuf, size_t urlCap, char *err, size_t errLen,
                 int connectTimeoutMs = 20000, int readTimeoutMs = 60000, int maxHops = 4);

    void close();

    int httpCode() const { return code_; }
    // Content-Length, or -1 when the server omitted it (cached at openGet).
    int contentLength() const { return length_; }
    HTTPClient &client() { return http_; }
    WiFiClient *stream() { return http_.getStreamPtr(); }

  private:
    HTTPClient http_;
    WiFiClientSecure *tls_ = nullptr;
    WiFiClient *plain_ = nullptr;
    int code_ = 0;
    int length_ = -1;
};
