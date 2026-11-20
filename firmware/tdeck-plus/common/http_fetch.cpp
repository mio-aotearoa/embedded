// SPDX-License-Identifier: GPL-3.0-or-later
#include "http_fetch.h"
#include "psram_alloc.h"

#include <WiFi.h>
#include <mbedtls/platform.h>

#include <stdio.h>
#include <string.h>

void httpTlsUsePsram(void)
{
    static bool done = false;
    if (done) return;
    mbedtls_platform_set_calloc_free(psramPreferCalloc, psramPreferFree);
    done = true;
}

bool httpResolveRedirect(const char *baseUrl, const char *loc, char *out, size_t outCap)
{
    if (!baseUrl || !loc || !out || outCap == 0) return false;
    if (!loc[0]) return false;
    if (strncmp(loc, "http://", 7) == 0 || strncmp(loc, "https://", 8) == 0)
    {
        if (strlen(loc) >= outCap) return false;
        snprintf(out, outCap, "%s", loc);
        return true;
    }
    const char *scheme = strstr(baseUrl, "://");
    const char *slash = scheme ? strchr(scheme + 3, '/') : nullptr;
    const size_t base = slash ? (size_t)(slash - baseUrl) : strlen(baseUrl);
    if (base + strlen(loc) >= outCap) return false;
    snprintf(out, outCap, "%.*s%s", (int)base, baseUrl, loc);
    return true;
}

void HttpFetch::close()
{
    http_.end();
    delete tls_;
    tls_ = nullptr;
    delete plain_;
    plain_ = nullptr;
    code_ = 0;
    length_ = -1;
}

bool HttpFetch::openGet(const char *url, char *urlBuf, size_t urlCap, char *err, size_t errLen,
                        int connectTimeoutMs, int readTimeoutMs, int maxHops)
{
    if (err && errLen) err[0] = 0;
    if (!url || !urlBuf || urlCap < 8)
    {
        if (err && errLen) snprintf(err, errLen, "bad URL buffer");
        return false;
    }
    if (url != urlBuf) snprintf(urlBuf, urlCap, "%s", url);
    if (!urlBuf[0])
    {
        if (err && errLen) snprintf(err, errLen, "empty URL");
        return false;
    }

    httpTlsUsePsram();

    for (int hop = 0;; ++hop)
    {
        close();

        http_.setReuse(false);
        http_.setConnectTimeout(connectTimeoutMs);
        http_.setTimeout(readTimeoutMs);
        http_.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
        http_.setUserAgent("Mozilla/5.0 (compatible; wadamesh-tdeck)");

        const bool https = strncmp(urlBuf, "https://", 8) == 0;
        bool began = false;
        if (https)
        {
            tls_ = new WiFiClientSecure();
            if (!tls_)
            {
                if (err && errLen) snprintf(err, errLen, "out of memory for TLS");
                return false;
            }
            tls_->setInsecure();  // no cert store on the device
            tls_->setHandshakeTimeout(45);
            began = http_.begin(*tls_, urlBuf);
        }
        else if (strncmp(urlBuf, "http://", 7) == 0)
        {
            plain_ = new WiFiClient();
            if (!plain_)
            {
                if (err && errLen) snprintf(err, errLen, "out of memory for TCP");
                return false;
            }
            began = http_.begin(*plain_, urlBuf);
        }
        else
        {
            if (err && errLen) snprintf(err, errLen, "http and https only");
            return false;
        }
        if (!began)
        {
            close();
            if (err && errLen) snprintf(err, errLen, "cannot parse the URL");
            return false;
        }

        http_.addHeader("Accept-Encoding", "identity");
        code_ = http_.GET();
        if (code_ == 301 || code_ == 302 || code_ == 303 || code_ == 307 || code_ == 308)
        {
            const String loc = http_.getLocation();
            http_.end();
            delete tls_;
            tls_ = nullptr;
            delete plain_;
            plain_ = nullptr;
            if (!loc.length() || hop >= maxHops)
            {
                if (err && errLen)
                {
                    snprintf(err, errLen, "%s",
                             loc.length() ? "too many redirects" : "redirect without a Location");
                }
                return false;
            }
            char next[1536];
            if (!httpResolveRedirect(urlBuf, loc.c_str(), next, sizeof next) ||
                strlen(next) >= urlCap)
            {
                if (err && errLen) snprintf(err, errLen, "redirect URL too long");
                return false;
            }
            snprintf(urlBuf, urlCap, "%s", next);
            continue;
        }
        if (code_ != HTTP_CODE_OK)
        {
            if (err && errLen)
            {
                snprintf(err, errLen, "HTTP %d (%s)", code_,
                         HTTPClient::errorToString(code_).c_str());
            }
            const int failed = code_;
            close();
            code_ = failed;  // keep for callers that inspect httpCode() after failure
            return false;
        }
        length_ = http_.getSize();
        return true;
    }
}
