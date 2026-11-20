// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>
#include <stdbool.h>

// Load /ime/cjk14.bin from the SD card into PSRAM and expose it as an LVGL
// font. Call once after the card is mounted. Idempotent.
bool imeFontEnsure(void);

bool imeFontReady(void);

// CJK face for textareas / candidate bar (null when not loaded).
const lv_font_t *imeFont(void);

// Attach as fallback behind `base` (Montserrat). Safe if font missing.
void imeFontAttachFallback(lv_font_t *base);
