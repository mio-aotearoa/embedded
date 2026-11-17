// SPDX-License-Identifier: GPL-3.0-or-later
//
// Japanese input extension for wadamesh's own text-entry path.
//
// It is not a keyboard: keystrokes still come from the physical keyboard (or the
// shared bindable field), and this module only rewrites the trailing romaji run
// of a field into kana when Space or Enter is pressed. Toggled per field with a
// long press on that field, so English typing is unaffected.
#pragma once

#include <lvgl.h>

bool imeJaActive();
void imeJaSetActive(bool on);
void imeJaToggle();

// Converts the trailing romaji run of `ta` to hiragana when it is a complete
// syllable sequence. Returns true when kana were inserted (the caller must then
// NOT also insert the space/Enter it was handling).
bool imeJaCommit(lv_obj_t* ta);
