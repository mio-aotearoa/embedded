// SPDX-License-Identifier: GPL-3.0-or-later
//
// The browser engine exposed as a function, so wadamesh's built-in Web page
// (the Reader page, which owns the address bar, history and the back bar) can
// render a fetched page in place of its text-only view. One web app, one fetch
// path, our renderer doing the layout.
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
// "core init", "no body", … so the Reader can say it instead of guessing.
const char* browserLastRenderError();

// The Reader's CJK-capable fonts, so laid-out pages keep the same glyph coverage
// as the text view instead of falling back to Montserrat.
const lv_font_t* readerWebFont(int px, bool bold);
