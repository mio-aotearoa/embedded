// SPDX-License-Identifier: GPL-3.0-or-later
#include "ime_mode.h"

#include "ime_cand.h"
#include "ime_font.h"
#include "ime_ja.h"
#include "ime_paths.h"
#include "ime_zh.h"
#include "skk_dict.h"

#include <Arduino.h>
#include <SD.h>

#include <stdio.h>

namespace
{
ImeMode s_mode = IME_OFF;
char s_status[96];
}  // namespace

ImeMode imeMode(void) { return s_mode; }

void imeDismissUi(void) { imeCandHide(); }

void imeModeSet(ImeMode m)
{
    if (m == s_mode) return;
    imeDismissUi();
    s_mode = m;
    imeJaSetActive(m == IME_JA);
    if (m != IME_OFF) imeEnsureReady();
}

ImeMode imeModeCycle(void)
{
    ImeMode next = IME_OFF;
    switch (s_mode)
    {
        case IME_OFF: next = IME_JA; break;
        case IME_JA: next = IME_ZH; break;
        case IME_ZH: next = IME_OFF; break;
    }
    imeModeSet(next);
    return s_mode;
}

void imeEnsureReady(void)
{
    imeFontEnsure();
    if (s_mode == IME_JA) skkDictEnsure();
    if (s_mode == IME_ZH) imeZhEnsure();
}

bool imeModeDictReady(void)
{
    switch (s_mode)
    {
        case IME_JA: return skkDictReady() || SD.exists(IME_SKK_DICT);
        case IME_ZH: return imeZhDictReady();
        default: return true;
    }
}

const char *imeModeStatus(void)
{
    switch (s_mode)
    {
        case IME_JA:
            if (!SD.exists(IME_SKK_DICT))
                snprintf(s_status, sizeof s_status, "Japanese: kana (download SKK for kanji)");
            else
                snprintf(s_status, sizeof s_status, "Japanese: Space = kana/kanji");
            break;
        case IME_ZH:
            if (!SD.exists(IME_PINYIN_DICT))
                snprintf(s_status, sizeof s_status, "Chinese: download dict_pinyin.dat");
            else
                snprintf(s_status, sizeof s_status, "Chinese: Space = candidates");
            break;
        default:
            snprintf(s_status, sizeof s_status, "IME off");
            break;
    }
    if (s_mode != IME_OFF && !imeFontReady() && !SD.exists(IME_CJK_FONT))
    {
        // Append a short font hint without overflowing.
        size_t n = strlen(s_status);
        if (n + 18 < sizeof s_status) snprintf(s_status + n, sizeof s_status - n, " · no CJK font");
    }
    return s_status;
}

bool imeHandleKey(lv_obj_t *ta, int key)
{
    if (!ta) return false;
    if (s_mode == IME_OFF) return false;
    if (imeCandVisible())
    {
        if (key == ' ' || (key >= '1' && key <= '9') || key == 0x0D || key == 0x0A || key == 27)
            return imeCandHandleKey(key);
        // Any edit or cursor key invalidates the candidate list. The caller
        // still handles the key normally after this returns false.
        imeCandHide();
    }
    if (s_mode == IME_JA) return imeJaHandleKey(ta, key);
    if (s_mode == IME_ZH) return imeZhHandleKey(ta, key);
    return false;
}
