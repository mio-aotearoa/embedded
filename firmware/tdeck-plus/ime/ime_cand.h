// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>
#include <stddef.h>
#include <stdint.h>

constexpr int kImeCandMax = 9;

struct ImeCandList
{
    char items[kImeCandMax][48];
    int count;
    int selected;  // 0-based highlight
};

void imeCandHide(void);
bool imeCandVisible(void);

// Show up to 9 candidates above the bottom of the screen. `title` is a short
// hint (pinyin / kana). Selecting calls `onPick(index, user)`.
typedef void (*ImeCandPickFn)(int index, void *user);

void imeCandShow(const ImeCandList *list, const char *title, ImeCandPickFn onPick,
                 void *user);

// Space / digit / Enter while the bar is up. Returns true if consumed.
bool imeCandHandleKey(int key);

int imeCandCount(void);
int imeCandSelected(void);
const char *imeCandItem(int index);
