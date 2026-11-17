// SPDX-License-Identifier: GPL-3.0-or-later
//
// Address-bar URL completion shared by the Browser, the Downloader and the
// Kiwix download field: a bare host such as "example.com/wiki" becomes
// "https://example.com/wiki", because https is what the firmware can fetch and
// what the user means by typing a domain.
//
// A URL that already carries a scheme is passed through untouched — including
// one this firmware cannot fetch — so the caller can refuse it with a real
// message instead of quietly fetching something else.
#pragma once

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Writes the completed URL into `out`. Returns false only when the result does
// not fit, so a truncated host is never fetched by accident.
inline bool urlComplete(const char *in, char *out, size_t cap)
{
    if (!in || !out || cap == 0) return false;

    while (*in && isspace((unsigned char)*in)) ++in;
    size_t len = strlen(in);
    while (len > 0 && isspace((unsigned char)in[len - 1])) --len;
    if (len == 0)
    {
        out[0] = '\0';
        return true;
    }
    if (len >= cap) return false;

    const bool http = len >= 7 && strncmp(in, "http://", 7) == 0;
    const bool https = len >= 8 && strncmp(in, "https://", 8) == 0;
    if (http || https)
    {
        memcpy(out, in, len);
        out[len] = '\0';
        return true;
    }

    // Any other scheme is left as typed; the caller reports "http and https only".
    for (size_t i = 0; i + 3 <= len; ++i)
    {
        if (in[i] == ':' && i + 3 <= len && memcmp(in + i, "://", 3) == 0)
        {
            memcpy(out, in, len);
            out[len] = '\0';
            return true;
        }
    }

    if (len + 8 > cap) return false;
    snprintf(out, cap, "https://%.*s", (int)len, in);
    return true;
}
