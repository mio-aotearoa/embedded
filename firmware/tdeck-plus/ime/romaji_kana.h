// SPDX-License-Identifier: GPL-3.0-or-later
//
// Romaji -> kana conversion for the Japanese input page.
//
// This is the input half of a Japanese IME: kana conversion (kana -> kanji,
// e.g. "にほん" -> "日本") needs a conversion dictionary the size of the
// pinyin one, and no portable ESP32 implementation exists, so this converter
// deliberately stops at kana and says so in the UI.
//
// Pure C++ with no LVGL, Arduino or filesystem dependencies, so it is
// host-testable.
#pragma once

#include <stddef.h>

namespace ime
{
// Converts a romaji string into hiragana (or katakana when `katakana` is set),
// writing UTF-8 into `out` and NUL-terminating it.
//
// Rules implemented: longest-match table lookup, sokuon for a doubled
// consonant ("gakkou" -> がっこう), syllabic n before a consonant / at the end /
// after an apostrophe ("shinbun", "nihon", "n'ya"), and '-' -> ー.
// A trailing incomplete syllable (e.g. "k" of "ka") is left out of the output.
// Returns the number of bytes written, excluding the terminator.
size_t romajiToKana(const char *romaji, char *out, size_t outCap, bool katakana);

// True when `romaji` is a complete sequence of syllables (no dangling prefix).
bool romajiIsComplete(const char *romaji);

// Number of UTF-8 codepoints in a NUL-terminated string.
size_t utf8CharCount(const char *text);
}  // namespace ime
