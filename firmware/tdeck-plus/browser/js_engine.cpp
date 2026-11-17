// SPDX-License-Identifier: GPL-3.0-or-later
//
// Basic JavaScript support: inline <script> blocks executed with Duktape.
//
// Duktape is an embeddable ES5.1 engine (~350 KB of flash here, no JIT, no ICU)
// pinned as a submodule in third_party/duktape — the same engine TactileBrowser's
// own ESP32 build uses. It gives pages the language they actually use: var/let,
// functions, arrays, objects, while/for, string methods, JSON.
//
// There is no DOM. A page gets document.write(), document.title and
// console.log(); document.write() output is spliced in where the script was,
// which is how script-generated pages assemble their HTML. Anything else a
// script computes is discarded, and external (src=) scripts are not fetched.
#include "js_engine.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "duktape.h"

namespace
{
constexpr size_t kWriteCap = 4096;
constexpr size_t kExtraCap = 16 * 1024;
constexpr size_t kMsgCap = 128;

char s_write[kWriteCap];
size_t s_writeLen = 0;
char s_log[kMsgCap] = "";
char s_error[kMsgCap] = "";

void append(char *buf, size_t cap, size_t *len, const char *s, size_t n)
{
    if (!s || n == 0 || *len + 1 >= cap) return;
    if (n > cap - 1 - *len) n = cap - 1 - *len;
    memcpy(buf + *len, s, n);
    *len += n;
    buf[*len] = 0;
}

// document.write(...) appends the arguments' text to the current block's output.
duk_ret_t jsDocumentWrite(duk_context *ctx)
{
    const duk_idx_t n = duk_get_top(ctx);
    for (duk_idx_t i = 0; i < n; i++)
    {
        duk_size_t len = 0;
        const char *s = duk_safe_to_lstring(ctx, i, &len);
        append(s_write, kWriteCap, &s_writeLen, s, len);
    }
    return 0;
}

// console.log(...) keeps the last line for the status bar.
duk_ret_t jsConsoleLog(duk_context *ctx)
{
    char line[kMsgCap];
    size_t len = 0;
    line[0] = 0;
    const duk_idx_t n = duk_get_top(ctx);
    for (duk_idx_t i = 0; i < n; i++)
    {
        duk_size_t part = 0;
        const char *s = duk_safe_to_lstring(ctx, i, &part);
        if (i) append(line, sizeof line, &len, " ", 1);
        append(line, sizeof line, &len, s, part);
    }
    snprintf(s_log, sizeof s_log, "%s", line);
    return 0;
}

// Case-insensitive search; `needle` must already be lower case.
size_t findCi(const char *hay, size_t hayLen, size_t from, const char *needle)
{
    const size_t n = strlen(needle);
    if (n == 0 || hayLen < n) return SIZE_MAX;
    for (size_t i = from; i + n <= hayLen; i++)
    {
        size_t k = 0;
        while (k < n && tolower((unsigned char)hay[i + k]) == needle[k]) k++;
        if (k == n) return i;
    }
    return SIZE_MAX;
}

bool scriptIsExternal(const char *html, size_t from, size_t to)
{
    if (findCi(html, to, from, "src=") != SIZE_MAX) return true;
    return findCi(html, to, from, " src") != SIZE_MAX;
}

// Plain JavaScript only: Elk-style modules and JSON data blocks are left alone.
bool scriptIsRunnable(const char *html, size_t from, size_t to)
{
    const size_t type = findCi(html, to, from, "type=");
    if (type == SIZE_MAX) return true;
    size_t p = type + 5;
    while (p < to && (html[p] == ' ' || html[p] == '"' || html[p] == '\'')) p++;
    if (p + 15 <= to && strncasecmp(html + p, "text/javascript", 15) == 0) return true;
    if (p + 22 <= to && strncasecmp(html + p, "application/javascript", 22) == 0) return true;
    return false;
}

void installGlobals(duk_context *ctx)
{
    duk_push_global_object(ctx);

    duk_push_object(ctx);
    duk_push_c_function(ctx, jsDocumentWrite, DUK_VARARGS);
    duk_put_prop_string(ctx, -2, "write");
    duk_push_string(ctx, "");
    duk_put_prop_string(ctx, -2, "title");
    duk_put_prop_string(ctx, -2, "document");

    duk_push_object(ctx);
    duk_push_c_function(ctx, jsConsoleLog, DUK_VARARGS);
    duk_put_prop_string(ctx, -2, "log");
    duk_put_prop_string(ctx, -2, "console");

    duk_pop(ctx);
}
}  // namespace

const char *browserJsLastLog(void) { return s_log; }
const char *browserJsLastError(void) { return s_error; }

char *browserJsApplyScripts(const char *html, size_t len, size_t *outLenOut)
{
    if (outLenOut) *outLenOut = 0;
    s_log[0] = 0;
    s_error[0] = 0;
    if (!html || len == 0) return nullptr;
    if (findCi(html, len, 0, "<script") == SIZE_MAX) return nullptr;

    duk_context *ctx = duk_create_heap(nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!ctx) return nullptr;
    installGlobals(ctx);

    const size_t outCap = len + kExtraCap + 1;
    char *out = (char *)malloc(outCap);
    if (!out)
    {
        duk_destroy_heap(ctx);
        return nullptr;
    }
    out[0] = 0;
    size_t outLen = 0, pos = 0;

    while (pos < len)
    {
        const size_t open = findCi(html, len, pos, "<script");
        if (open == SIZE_MAX)
        {
            append(out, outCap, &outLen, html + pos, len - pos);
            break;
        }
        append(out, outCap, &outLen, html + pos, open - pos);

        const size_t openEnd = findCi(html, len, open, ">");
        const size_t close = (openEnd == SIZE_MAX) ? SIZE_MAX : findCi(html, len, openEnd + 1, "</script");
        if (openEnd == SIZE_MAX || close == SIZE_MAX)
        {
            // Unterminated script: drop it rather than swallow the rest of the page.
            pos = len;
            break;
        }

        const bool runnable = scriptIsRunnable(html, openEnd + 1, close) &&
                              !scriptIsExternal(html, open, openEnd + 1);

        if (runnable)
        {
            const size_t bodyLen = close - (openEnd + 1);
            char *src = (char *)malloc(bodyLen + 1);
            if (src)
            {
                memcpy(src, html + openEnd + 1, bodyLen);
                src[bodyLen] = 0;
                s_writeLen = 0;
                s_write[0] = 0;
                if (duk_peval_lstring(ctx, src, bodyLen) != 0 && s_error[0] == 0)
                {
                    const char *msg = duk_safe_to_string(ctx, -1);
                    snprintf(s_error, sizeof s_error, "%s", msg ? msg : "JavaScript error");
                }
                free(src);
                duk_set_top(ctx, 0);
                append(out, outCap, &outLen, s_write, s_writeLen);
            }
        }

        const size_t closeEnd = findCi(html, len, close, ">");
        pos = (closeEnd == SIZE_MAX) ? len : closeEnd + 1;
    }

    duk_destroy_heap(ctx);
    out[outLen] = 0;
    if (outLen) *outLenOut = outLen;
    return out;
}
