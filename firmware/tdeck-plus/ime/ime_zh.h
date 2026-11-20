// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>
#include <stdbool.h>

bool imeZhDictReady(void);
bool imeZhEnsure(void);

// Space/Enter on trailing pinyin → candidate bar. Digits handled by ime_cand.
bool imeZhHandleKey(lv_obj_t *ta, int key);
