// SPDX-License-Identifier: GPL-3.0-or-later
#include "ime_ja.h"

#include "romaji_kana.h"

#include <ctype.h>
#include <string.h>

namespace
{
bool s_on = false;
constexpr size_t kMaxRun = 24;

bool romajiChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '\'' || c == '-';
}

size_t trailingRunLen(const char* text)
{
    size_t n = strlen(text);
    size_t start = n;
    while (start > 0 && romajiChar(text[start - 1])) --start;
    return n - start;
}
}  // namespace

bool imeJaActive() { return s_on; }
void imeJaSetActive(bool on) { s_on = on; }
void imeJaToggle() { s_on = !s_on; }

bool imeJaCommit(lv_obj_t* ta)
{
    if (!s_on || !ta) return false;
    const char* text = lv_textarea_get_text(ta);
    if (!text) return false;
    const size_t textLen = strlen(text);
    if ((size_t)lv_textarea_get_cursor_pos(ta) != textLen) return false;  // caret elsewhere
    const size_t runLen = trailingRunLen(text);
    if (runLen < 2 || runLen > kMaxRun) return false;

    char romaji[kMaxRun + 1];
    const char* run = text + strlen(text) - runLen;
    for (size_t i = 0; i < runLen; ++i) romaji[i] = (char)tolower((unsigned char)run[i]);
    romaji[runLen] = '\0';
    if (!ime::romajiIsComplete(romaji)) return false;

    char kana[4 * kMaxRun + 1];
    const size_t n = ime::romajiToKana(romaji, kana, sizeof kana, false);
    if (n == 0) return false;

    for (size_t i = 0; i < runLen; ++i) lv_textarea_del_char(ta);
    lv_textarea_add_text(ta, kana);
    return true;
}
