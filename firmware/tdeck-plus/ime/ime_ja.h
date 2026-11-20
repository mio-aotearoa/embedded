// SPDX-License-Identifier: GPL-3.0-or-later
//
// Japanese input extension for wadamesh's own text-entry path.
//
// Keystrokes still come from the physical keyboard. Space converts a trailing
// romaji run to kana, then (when /ime/skk_mdic.bin is present) offers kanji
// candidates. Digits 1-9 / Enter pick; Esc dismisses.
#pragma once

#include <lvgl.h>

bool imeJaActive();
void imeJaSetActive(bool on);
void imeJaToggle();

// Space-path: romaji → kana (+ optional kanji bar). Returns true when consumed.
bool imeJaCommit(lv_obj_t *ta);

// Full key hook used by ime_mode (Space / Enter / digits / Esc).
bool imeJaHandleKey(lv_obj_t *ta, int key);
