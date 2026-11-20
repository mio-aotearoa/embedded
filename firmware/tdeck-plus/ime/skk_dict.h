// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>

// Read-only SKK middle dictionary on SD (/ime/skk_mdic.bin). Binary layout
// matches Tamakichi/arduino_skk. No SD.begin() — the firmware already owns the card.

bool skkDictEnsure(void);
bool skkDictReady(void);
void skkDictClose(void);

// Look up hiragana `kana` (UTF-8). Fills up to `cap` candidate strings
// (UTF-8, NUL-terminated). Returns the number written.
int skkDictLookup(const char *kana, char out[][48], int cap);
