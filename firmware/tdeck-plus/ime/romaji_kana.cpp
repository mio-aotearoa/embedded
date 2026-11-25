// SPDX-License-Identifier: GPL-3.0-or-later
#include "romaji_kana.h"

#include <string.h>

namespace ime
{
namespace
{
struct Entry
{
    const char *romaji;
    const char *kana;  // hiragana, UTF-8
};

// Longest romaji first so a greedy prefix match picks "kya" over "ki".
constexpr Entry kTable[] = {
    {"kya", "きゃ"}, {"kyu", "きゅ"}, {"kyo", "きょ"},
    {"gya", "ぎゃ"}, {"gyu", "ぎゅ"}, {"gyo", "ぎょ"},
    {"sha", "しゃ"}, {"shu", "しゅ"}, {"sho", "しょ"},
    {"sya", "しゃ"}, {"syu", "しゅ"}, {"syo", "しょ"},
    {"ja", "じゃ"},  {"ju", "じゅ"},  {"jo", "じょ"},
    {"jya", "じゃ"}, {"jyu", "じゅ"}, {"jyo", "じょ"},
    {"cha", "ちゃ"}, {"chu", "ちゅ"}, {"cho", "ちょ"},
    {"cya", "ちゃ"}, {"cyu", "ちゅ"}, {"cyo", "ちょ"},
    {"nya", "にゃ"}, {"nyu", "にゅ"}, {"nyo", "にょ"},
    {"hya", "ひゃ"}, {"hyu", "ひゅ"}, {"hyo", "ひょ"},
    {"bya", "びゃ"}, {"byu", "びゅ"}, {"byo", "びょ"},
    {"pya", "ぴゃ"}, {"pyu", "ぴゅ"}, {"pyo", "ぴょ"},
    {"mya", "みゃ"}, {"myu", "みゅ"}, {"myo", "みょ"},
    {"rya", "りゃ"}, {"ryu", "りゅ"}, {"ryo", "りょ"},
    {"shi", "し"},   {"chi", "ち"},   {"tsu", "つ"},
    {"fu", "ふ"},    {"ji", "じ"},    {"di", "ぢ"},   {"du", "づ"},
    {"ka", "か"}, {"ki", "き"}, {"ku", "く"}, {"ke", "け"}, {"ko", "こ"},
    {"ga", "が"}, {"gi", "ぎ"}, {"gu", "ぐ"}, {"ge", "げ"}, {"go", "ご"},
    {"sa", "さ"}, {"si", "し"}, {"su", "す"}, {"se", "せ"}, {"so", "そ"},
    {"za", "ざ"}, {"zi", "じ"}, {"zu", "ず"}, {"ze", "ぜ"}, {"zo", "ぞ"},
    {"ta", "た"}, {"ti", "ち"}, {"tu", "つ"}, {"te", "て"}, {"to", "と"},
    {"da", "だ"}, {"de", "で"}, {"do", "ど"},
    {"na", "な"}, {"ni", "に"}, {"nu", "ぬ"}, {"ne", "ね"}, {"no", "の"},
    {"ha", "は"}, {"hi", "ひ"}, {"hu", "ふ"}, {"he", "へ"}, {"ho", "ほ"},
    {"ba", "ば"}, {"bi", "び"}, {"bu", "ぶ"}, {"be", "べ"}, {"bo", "ぼ"},
    {"pa", "ぱ"}, {"pi", "ぴ"}, {"pu", "ぷ"}, {"pe", "ぺ"}, {"po", "ぽ"},
    {"ma", "ま"}, {"mi", "み"}, {"mu", "む"}, {"me", "め"}, {"mo", "も"},
    {"ya", "や"}, {"yu", "ゆ"}, {"yo", "よ"},
    {"ra", "ら"}, {"ri", "り"}, {"ru", "る"}, {"re", "れ"}, {"ro", "ろ"},
    {"wa", "わ"}, {"wo", "を"}, {"wi", "ゐ"}, {"we", "ゑ"},
    {"a", "あ"}, {"i", "い"}, {"u", "う"}, {"e", "え"}, {"o", "お"},
};

constexpr size_t kTableSize = sizeof(kTable) / sizeof(kTable[0]);
constexpr const char *kSokuon = "っ";
constexpr const char *kSyllabicN = "ん";
constexpr const char *kLongVowel = "ー";

bool isVowel(char c) { return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o'; }

size_t appendKana(const char *kana, char *out, size_t used, size_t cap, bool katakana)
{
    for (const char *p = kana; *p && used + 1 < cap; p++)
    {
        unsigned char c = (unsigned char)*p;
        if (katakana && c >= 0xE3)
        {
            // The table only holds hiragana; katakana is the same block shifted
            // by 0x60 at the code-point level. Decode this 3-byte sequence.
            if ((unsigned char)p[1] && (unsigned char)p[2])
            {
                const unsigned cp = ((c & 0x0F) << 12) | (((unsigned char)p[1] & 0x3F) << 6) |
                                    ((unsigned char)p[2] & 0x3F);
                const unsigned shifted = cp + 0x60;
                out[used++] = (char)(0xE0 | (shifted >> 12));
                if (used + 1 < cap) out[used++] = (char)(0x80 | ((shifted >> 6) & 0x3F));
                if (used + 1 < cap) out[used++] = (char)(0x80 | (shifted & 0x3F));
                p += 2;
                continue;
            }
        }
        out[used++] = *p;
    }
    return used;
}

// Length of the romaji entry that matches at `s`, or 0.
size_t matchAt(const char *s)
{
    for (size_t i = 0; i < kTableSize; i++)
    {
        const size_t len = strlen(kTable[i].romaji);
        if (strncmp(s, kTable[i].romaji, len) == 0) return len;
    }
    return 0;
}

const char *kanaFor(const char *s, size_t len)
{
    for (size_t i = 0; i < kTableSize; i++)
    {
        if (strlen(kTable[i].romaji) == len && strncmp(s, kTable[i].romaji, len) == 0)
        {
            return kTable[i].kana;
        }
    }
    return nullptr;
}
}  // namespace

size_t romajiToKana(const char *romaji, char *out, size_t outCap, bool katakana)
{
    if (!out || outCap == 0) return 0;
    out[0] = 0;
    if (!romaji) return 0;

    size_t used = 0;
    for (size_t i = 0; romaji[i];)
    {
        const char *s = romaji + i;
        const char c = s[0];

        // Doubled consonant (not n): sokuon, then the rest converts normally.
        if (c == s[1] && !isVowel(c) && c != 'n' && c >= 'a' && c <= 'z')
        {
            used = appendKana(kSokuon, out, used, outCap, katakana);
            i++;
            continue;
        }

        if (c == '-')
        {
            used = appendKana(kLongVowel, out, used, outCap, katakana);
            i++;
            continue;
        }

        if (c == '\'')
        {
            i++;
            continue;
        }

        // Syllabic n: "nn" is one ん, "n'" is ん, and n before a consonant or at
        // the end is ん. "ny"/"ni"/... must still reach the table, so try the
        // normal match first.
        if (c == 'n')
        {
            const size_t m = matchAt(s);
            const bool nextIsVowelOrY = s[1] && (isVowel(s[1]) || s[1] == 'y');
            if (m > 0 && !nextIsVowelOrY && m <= 1)
            {
                used = appendKana(kSyllabicN, out, used, outCap, katakana);
                i++;
                continue;
            }
            if (m > 1)
            {
                used = appendKana(kanaFor(s, m), out, used, outCap, katakana);
                i += m;
                continue;
            }
            // n at end, or n before a consonant, or "nn". Only "nn" followed
            // by another consonant/end is a single ん ("ann" -> あん); in
            // "onna"/"konnichiwa" the second n starts the next syllable, so
            // one n is consumed and "na"/"ni" converts normally.
            used = appendKana(kSyllabicN, out, used, outCap, katakana);
            const bool secondStartsSyllable =
                s[1] == 'n' && s[2] && (isVowel(s[2]) || s[2] == 'y');
            i += (s[1] == 'n' && !secondStartsSyllable) ? 2 : 1;
            continue;
        }

        const size_t m = matchAt(s);
        if (m == 0) return used;  // unknown letter: stop, leaving what we have
        used = appendKana(kanaFor(s, m), out, used, outCap, katakana);
        i += m;
    }
    out[used < outCap ? used : outCap - 1] = 0;
    return used;
}

bool romajiIsComplete(const char *romaji)
{
    if (!romaji) return true;
    size_t i = 0;
    while (romaji[i])
    {
        const char *s = romaji + i;
        if (s[0] == s[1] && !isVowel(s[0]) && s[0] != 'n' && s[0] >= 'a' && s[0] <= 'z')
        {
            i++;
            continue;
        }
        if (s[0] == '-' || s[0] == '\'')
        {
            i++;
            continue;
        }
        if (s[0] == 'n')
        {
            const size_t m = matchAt(s);
            const bool nextIsVowelOrY = s[1] && (isVowel(s[1]) || s[1] == 'y');
            if (m > 0 && !nextIsVowelOrY && m <= 1)
            {
                i++;
                continue;
            }
            if (m > 1)
            {
                i += m;
                continue;
            }
            const bool secondStartsSyllable =
                s[1] == 'n' && s[2] && (isVowel(s[2]) || s[2] == 'y');
            i += (s[1] == 'n' && !secondStartsSyllable) ? 2 : 1;
            continue;
        }
        const size_t m = matchAt(s);
        if (m == 0) return false;
        i += m;
    }
    return true;
}

size_t utf8CharCount(const char *text)
{
    if (!text) return 0;
    size_t count = 0;
    for (; *text; ++text)
    {
        if (((unsigned char)*text & 0xC0) != 0x80) ++count;
    }
    return count;
}
}  // namespace ime
