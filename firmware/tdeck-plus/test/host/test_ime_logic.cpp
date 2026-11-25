// Host checks for the LVGL-independent Japanese IME helpers.
#include "ime/romaji_kana.h"

#include <stdio.h>
#include <string.h>

namespace
{
int failures = 0;

void check(bool ok, const char *what)
{
    if (ok) return;
    fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}
}  // namespace

int main()
{
    char out[128];

    check(ime::utf8CharCount("") == 0, "empty UTF-8 count");
    check(ime::utf8CharCount("abc") == 3, "ASCII UTF-8 count");
    check(ime::utf8CharCount("にほん") == 3, "kana UTF-8 count");
    check(ime::utf8CharCount("漢字かな") == 4, "mixed UTF-8 count");

    check(!ime::romajiIsComplete("k"), "dangling romaji must stay incomplete");
    check(ime::romajiIsComplete("ka"), "complete romaji rejected");
    check(ime::romajiIsComplete("shinbun"), "syllabic n sequence rejected");

    size_t n = ime::romajiToKana("nihon", out, sizeof out, false);
    check(n > 0 && strcmp(out, "にほん") == 0, "nihon conversion");

    n = ime::romajiToKana("gakkou", out, sizeof out, false);
    check(n > 0 && strcmp(out, "がっこう") == 0, "sokuon conversion");

    n = ime::romajiToKana("kanji", out, sizeof out, false);
    check(n > 0 && strcmp(out, "かんじ") == 0, "kanji conversion");

    if (failures) fprintf(stderr, "%d IME logic check(s) failed\n", failures);
    return failures ? 1 : 0;
}
