// SPDX-License-Identifier: GPL-3.0-or-later
#include "kiwix_catalog.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

namespace
{
constexpr const char* kBase = "https://download.kiwix.org/zim/";
constexpr size_t kMaxListing = 64 * 1024;  // the biggest folder index is far below this
constexpr size_t kMaxItems = 80;
constexpr int kMaxHops = 4;

// GET `url` into `body`, following up to kMaxHops redirects (absolute or relative).
bool readPage(const char* url, std::string& body, char* err, size_t errLen)
{
    String current(url);
    for (int hop = 0; hop <= kMaxHops; ++hop)
    {
        WiFiClientSecure tls;
        WiFiClient plain;
        HTTPClient http;
        http.setReuse(false);
        http.setConnectTimeout(15000);
        http.setTimeout(20000);
        http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
        http.setUserAgent("wadamesh-tdeck");

        bool began = false;
        if (current.startsWith("https://"))
        {
            tls.setInsecure();  // no cert store on the device
            began = http.begin(tls, current);
        }
        else
        {
            began = http.begin(plain, current);
        }
        if (!began)
        {
            snprintf(err, errLen, "cannot parse the catalogue URL");
            return false;
        }

        http.addHeader("Accept-Encoding", "identity");
        const int code = http.GET();
        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308)
        {
            String loc = http.header("Location");
            http.end();
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
            snprintf(err, errLen, "HTTP %d from the catalogue", code);
            http.end();
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

bool kiwixCatalogFetch(const char* dir, std::vector<KiwixCatalogItem>& out,
                       char* err, size_t errLen)
{
    out.clear();
    if (!err || errLen == 0) return false;
    err[0] = '\0';
    if (WiFi.status() != WL_CONNECTED)
    {
        snprintf(err, errLen, "Connect to Wi-Fi first");
        return false;
    }

    char url[256];
    const char* leaf = dir ? dir : "";
    while (*leaf == '/') ++leaf;
    if (*leaf)
    {
        snprintf(url, sizeof url, "%s%s/", kBase, leaf);
    }
    else
    {
        snprintf(url, sizeof url, "%s", kBase);
    }

    std::string html;
    if (!readPage(url, html, err, errLen)) return false;

    parseListing(html, out);
    if (out.empty())
    {
        snprintf(err, errLen, "No archives in this folder");
        return false;
    }
    return true;
}
