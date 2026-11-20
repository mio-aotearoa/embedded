// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Card paths for IME assets (Download presets land here).
// Arduino SD.open / SD.exists use paths relative to the card root ("/ime/...").
// POSIX fopen (libgooglepinyin) needs the VFS mount ("/sd/ime/...") — T-Deck
// mounts the card at /sd via SD.begin(..., "/sd", ...).
#define IME_DIR            "/ime"
#define IME_PINYIN_DICT    "/ime/dict_pinyin.dat"
#define IME_PINYIN_DICT_VFS "/sd/ime/dict_pinyin.dat"
#define IME_SKK_DICT       "/ime/skk_mdic.bin"
#define IME_CJK_FONT       "/ime/cjk14.bin"
