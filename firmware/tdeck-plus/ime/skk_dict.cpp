// SPDX-License-Identifier: GPL-3.0-or-later
#include "skk_dict.h"

#include "ime_paths.h"

#include <Arduino.h>
#include <SD.h>

#include <stdio.h>
#include <string.h>

namespace
{
File s_fp;
uint32_t s_count = 0;
uint32_t s_indexTop = 0;
uint32_t s_dataTop = 0;
bool s_ready = false;

bool readAt(uint32_t off, void *dst, size_t n)
{
    if (!s_fp) return false;
    if (!s_fp.seek(off)) return false;
    return s_fp.read((uint8_t *)dst, n) == (int)n;
}

bool keywordAt(uint32_t index, char *kw, size_t kwCap)
{
    if (index >= s_count || !kw || kwCap == 0) return false;
    uint32_t pos = 0;
    if (!readAt(s_indexTop + index * 4, &pos, 4)) return false;
    if (!s_fp.seek(s_dataTop + pos)) return false;
    size_t i = 0;
    for (;;)
    {
        int c = s_fp.read();
        if (c < 0) return false;
        if (c == ',')
        {
            kw[i] = 0;
            return true;
        }
        if (i + 1 < kwCap) kw[i++] = (char)c;
    }
}

int32_t binFind(const char *key)
{
    if (!key || !s_ready || s_count == 0) return -1;
    int32_t lo = 0, hi = (int32_t)s_count - 1;
    char kw[64];
    while (lo <= hi)
    {
        const int32_t mid = lo + ((hi - lo) >> 1);
        if (!keywordAt((uint32_t)mid, kw, sizeof kw)) return -1;
        const int cmp = strcmp(key, kw);
        if (cmp == 0) return mid;
        if (cmp > 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

// Parse "key,cand1,cand2,...," from the record at index into out[].
int parseCandidates(uint32_t index, char out[][48], int cap)
{
    if (index >= s_count || cap <= 0) return 0;
    uint32_t pos = 0, posNext = 0;
    if (!readAt(s_indexTop + index * 4, &pos, 4)) return 0;
    uint32_t size;
    if (index + 1 < s_count)
    {
        if (!readAt(s_indexTop + (index + 1) * 4, &posNext, 4)) return 0;
        size = posNext - pos;
    }
    else size = 256;

    if (size > 512) size = 512;
    char buf[513];
    if (!readAt(s_dataTop + pos, buf, size)) return 0;
    buf[size] = 0;

    // Skip the keyword up to the first comma.
    char *p = strchr(buf, ',');
    if (!p) return 0;
    ++p;

    int n = 0;
    while (*p && n < cap)
    {
        char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == 0) break;
        if (len >= 48) len = 47;
        memcpy(out[n], p, len);
        out[n][len] = 0;
        ++n;
        if (!end) break;
        p = end + 1;
    }
    return n;
}
}  // namespace

bool skkDictEnsure(void)
{
    if (s_ready) return true;
    if (!SD.exists(IME_SKK_DICT)) return false;
    s_fp = SD.open(IME_SKK_DICT, FILE_READ);
    if (!s_fp) return false;
    uint32_t hdr[3];
    if (s_fp.read((uint8_t *)hdr, 12) != 12)
    {
        s_fp.close();
        return false;
    }
    s_count = hdr[0];
    // Index always starts at byte 12 (arduino_skk get_keyword seeks SSK_BIN_HEAD_SIZE+i*4).
    // hdr[1] is keyword_index_top (informational); hdr[2] is data-region base.
    s_indexTop = 12;
    s_dataTop = hdr[2];
    if (s_count == 0 || s_count > 200000 || s_dataTop < 12)
    {
        s_fp.close();
        return false;
    }
    s_ready = true;
    return true;
}

bool skkDictReady(void) { return s_ready; }

void skkDictClose(void)
{
    if (s_fp) s_fp.close();
    s_ready = false;
    s_count = 0;
}

int skkDictLookup(const char *kana, char out[][48], int cap)
{
    if (!skkDictEnsure() || !kana || !kana[0] || cap <= 0) return 0;
    const int32_t idx = binFind(kana);
    if (idx < 0) return 0;
    return parseCandidates((uint32_t)idx, out, cap);
}
