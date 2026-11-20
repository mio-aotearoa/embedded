// SPDX-License-Identifier: GPL-3.0-or-later
#include "ime_ja.h"

#include "ime_cand.h"
#include "romaji_kana.h"
#include "skk_dict.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace
{
bool s_on = false;
constexpr size_t kMaxRun = 24;
lv_obj_t *s_ta = nullptr;
char s_pendingKana[96];

bool romajiChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '\'' || c == '-';
}

size_t trailingRunLen(const char *text)
{
    size_t n = strlen(text);
    size_t start = n;
    while (start > 0 && romajiChar(text[start - 1])) --start;
    return n - start;
}

void onKanjiPick(int index, void *user)
{
    (void)user;
    lv_obj_t *ta = s_ta;
    if (!ta || !lv_obj_is_valid(ta)) return;
    const char *kanji = imeCandItem(index);
    if (!kanji || !kanji[0]) return;
    // Replace trailing kana (same UTF-8 length as s_pendingKana) with kanji.
    const size_t kanaBytes = strlen(s_pendingKana);
    for (size_t i = 0; i < kanaBytes; ++i) lv_textarea_del_char(ta);
    lv_textarea_add_text(ta, kanji);
    s_pendingKana[0] = 0;
}

bool openKanjiCandidates(lv_obj_t *ta, const char *kana)
{
    ImeCandList list{};
    list.count = skkDictLookup(kana, list.items, kImeCandMax);
    list.selected = 0;
    if (list.count <= 0) return false;
    snprintf(s_pendingKana, sizeof s_pendingKana, "%s", kana);
    s_ta = ta;
    imeCandShow(&list, kana, onKanjiPick, nullptr);
    return true;
}

bool convertRomaji(lv_obj_t *ta, bool offerKanji)
{
    const char *text = lv_textarea_get_text(ta);
    if (!text) return false;
    const size_t textLen = strlen(text);
    if ((size_t)lv_textarea_get_cursor_pos(ta) != textLen) return false;
    const size_t runLen = trailingRunLen(text);
    if (runLen < 2 || runLen > kMaxRun) return false;

    char romaji[kMaxRun + 1];
    const char *run = text + textLen - runLen;
    for (size_t i = 0; i < runLen; ++i) romaji[i] = (char)tolower((unsigned char)run[i]);
    romaji[runLen] = '\0';
    if (!ime::romajiIsComplete(romaji)) return false;

    char kana[4 * kMaxRun + 1];
    const size_t n = ime::romajiToKana(romaji, kana, sizeof kana, false);
    if (n == 0) return false;

    for (size_t i = 0; i < runLen; ++i) lv_textarea_del_char(ta);
    lv_textarea_add_text(ta, kana);

    // Space path: if the SKK dict is on the card, offer kanji candidates.
    if (offerKanji && skkDictEnsure()) openKanjiCandidates(ta, kana);
    return true;
}
}  // namespace

bool imeJaActive() { return s_on; }
void imeJaSetActive(bool on)
{
    s_on = on;
    if (!on)
    {
        imeCandHide();
        s_pendingKana[0] = 0;
    }
}
void imeJaToggle() { imeJaSetActive(!s_on); }

bool imeJaCommit(lv_obj_t *ta)
{
    if (!s_on || !ta) return false;
    if (imeCandVisible() && imeCandHandleKey(' ')) return true;  // cycle when bar up
    return convertRomaji(ta, true);
}

bool imeJaHandleKey(lv_obj_t *ta, int key)
{
    if (!s_on || !ta) return false;
    if (imeCandVisible())
    {
        if (key == ' ' || (key >= '1' && key <= '9') || key == 0x0D || key == 0x0A || key == 27)
            return imeCandHandleKey(key);
    }
    if (key == ' ') return convertRomaji(ta, true);
    if (key == 0x0D || key == 0x0A)
    {
        // Enter: commit romaji→kana only (no kanji bar).
        return convertRomaji(ta, false);
    }
    return false;
}
