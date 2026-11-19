// SPDX-License-Identifier: GPL-3.0-or-later
//
// Optional Lexbor embed API for hosts that already own fetch/chrome. The
// drawer Browser app (BrowserApp) uses its own fetch + render path; wadamesh's
// Web/Reader tile stays on the upstream text-extraction path and does not call
// these helpers.
#pragma once

#include <lvgl.h>
#include <stddef.h>

// Renders `html` (fetched from `url`) into `parent`. `on_link` is called with the
// target address when a rendered link is tapped; nullptr means read-only.
// Returns false when the page could not be laid out, so the caller can fall
// back to its own text rendering.
bool browserRenderInto(lv_obj_t* parent, const char* url, const char* html, size_t len,
                       void (*on_link)(const char*));

// Why the last browserRenderInto() call fell back ("ok" on success): "parse",
// "core init", "no body", …
const char* browserLastRenderError();

// CJK-capable faces shared with the rest of the UI (implemented in UITask.cpp).
const lv_font_t* readerWebFont(int px, bool bold);
