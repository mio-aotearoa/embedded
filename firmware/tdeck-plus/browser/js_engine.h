// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>

// Basic JavaScript for the browser page.
//
// Inline <script> blocks run through Elk, the small JS engine already vendored
// in the pinned TactileBrowser tree (third_party/TactileBrowser/.../elk), so no
// new dependency is added. There is no DOM: the page sees document.write(),
// document.title and console.log(). document.write() output replaces the
// script block in the markup, which is how script-generated pages are built;
// console.log() and errors go to the status line.

// Returns a new NUL-terminated HTML string with every inline <script> replaced
// by what it wrote, or nullptr when the page has no script (render the original)
// or when the JS heap cannot be allocated. The caller frees the result.
char *browserJsApplyScripts(const char *html, size_t len, size_t *outLen);

// Diagnostics from the last browserJsApplyScripts() call (never nullptr).
const char *browserJsLastLog(void);
const char *browserJsLastError(void);
