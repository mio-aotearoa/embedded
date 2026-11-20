// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>
#include <stdint.h>

enum ImeMode : uint8_t
{
    IME_OFF = 0,
    IME_JA = 1,
    IME_ZH = 2,
};

ImeMode imeMode(void);
void imeModeSet(ImeMode m);
ImeMode imeModeCycle(void);  // Off → JA → ZH → Off

// Short status for toasts / app status lines (never null).
const char *imeModeStatus(void);

// True when the active mode has its dictionary on the card.
bool imeModeDictReady(void);

// Load font (if present) and warm dictionary handles. Safe to call often.
void imeEnsureReady(void);

// Keyboard hook for AppPage / composer textareas.
// Returns true when the key was consumed (caller must not also insert it).
bool imeHandleKey(lv_obj_t *ta, int key);

// Hide candidate UI (page close / mode off).
void imeDismissUi(void);
