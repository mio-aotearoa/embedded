// SPDX-License-Identifier: GPL-3.0-or-later
#include "ime_zh.h"

#include "ime_cand.h"
#include "ime_paths.h"
#include "lvgl_pinyin_ime/ime_engine.h"

#include <Arduino.h>
#include <SD.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace
{
bool s_open = false;
lv_obj_t *s_ta = nullptr;
char s_pendingPinyin[IME_ENGINE_MAX_PINYIN_LEN + 1];

bool pinyinChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '\'';
}

size_t trailingPinyin(const char *text, char *out, size_t outCap)
{
    if (!text || !out || outCap == 0) return 0;
    size_t n = strlen(text);
    size_t start = n;
    while (start > 0 && pinyinChar(text[start - 1])) --start;
    size_t len = n - start;
    if (len == 0 || len >= outCap) return 0;
    for (size_t i = 0; i < len; ++i) out[i] = (char)tolower((unsigned char)text[start + i]);
    out[len] = 0;
    return len;
}

void deleteTrailing(lv_obj_t *ta, size_t runLen)
{
    for (size_t i = 0; i < runLen; ++i) lv_textarea_del_char(ta);
}

void onPick(int index, void *user)
{
    (void)user;
    lv_obj_t *ta = s_ta;
    if (!ta || !lv_obj_is_valid(ta)) return;
    char text[64];
    if (ime_engine_candidate((size_t)index, text, sizeof text) == 0) return;
    // Replace the pending pinyin run with the chosen Hanzi.
    const size_t run = strlen(s_pendingPinyin);
    if (run) deleteTrailing(ta, run);
    lv_textarea_add_text(ta, text);
    ime_engine_choose((size_t)index);
    // If syllables remain, keep composing.
    size_t remLen = 0;
    const char *rem = ime_engine_remaining_pinyin(&remLen);
    if (rem && remLen)
    {
        snprintf(s_pendingPinyin, sizeof s_pendingPinyin, "%s", rem);
        lv_textarea_add_text(ta, s_pendingPinyin);
        ImeCandList list{};
        size_t n = ime_engine_search(s_pendingPinyin, strlen(s_pendingPinyin));
        list.count = 0;
        for (size_t i = 0; i < n && list.count < kImeCandMax; ++i)
        {
            if (ime_engine_candidate(i, list.items[list.count], sizeof list.items[0]) > 0)
                ++list.count;
        }
        list.selected = 0;
        if (list.count > 0) imeCandShow(&list, s_pendingPinyin, onPick, nullptr);
        else ime_engine_reset();
    }
    else
    {
        s_pendingPinyin[0] = 0;
        ime_engine_reset();
    }
}

bool openCandidates(lv_obj_t *ta, const char *pinyin)
{
    size_t n = ime_engine_search(pinyin, strlen(pinyin));
    if (n == 0) return false;
    ImeCandList list{};
    list.count = 0;
    list.selected = 0;
    for (size_t i = 0; i < n && list.count < kImeCandMax; ++i)
    {
        if (ime_engine_candidate(i, list.items[list.count], sizeof list.items[0]) > 0)
            ++list.count;
    }
    if (list.count == 0) return false;
    snprintf(s_pendingPinyin, sizeof s_pendingPinyin, "%s", pinyin);
    s_ta = ta;
    imeCandShow(&list, pinyin, onPick, nullptr);
    return true;
}
}  // namespace

bool imeZhEnsure(void)
{
    if (s_open) return true;
    if (!SD.exists(IME_PINYIN_DICT)) return false;
    // Engine uses fopen — must use the VFS mount path, not SD.open's "/ime/...".
    if (!ime_engine_open(IME_PINYIN_DICT_VFS, nullptr)) return false;
    ime_engine_set_max_lens(IME_ENGINE_MAX_PINYIN_LEN, 32);
    s_open = true;
    return true;
}

bool imeZhDictReady(void) { return s_open || SD.exists(IME_PINYIN_DICT); }

bool imeZhHandleKey(lv_obj_t *ta, int key)
{
    if (!ta) return false;
    if (imeCandVisible() && imeCandHandleKey(key)) return true;
    if (!imeZhEnsure()) return false;

    if (key == ' ' || key == 0x0D || key == 0x0A)
    {
        const char *text = lv_textarea_get_text(ta);
        if (!text) return false;
        if ((size_t)lv_textarea_get_cursor_pos(ta) != strlen(text)) return false;
        char py[IME_ENGINE_MAX_PINYIN_LEN + 1];
        const size_t len = trailingPinyin(text, py, sizeof py);
        if (len < 1) return false;
        ime_engine_reset();
        if (!openCandidates(ta, py)) return false;
        // Leave the pinyin visible until a candidate is picked.
        return true;
    }
    return false;
}
