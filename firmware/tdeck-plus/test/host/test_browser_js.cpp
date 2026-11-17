/* Host test for the browser's JavaScript layer (firmware/tdeck-plus/browser/
 * js_engine.cpp), built against the pinned Elk engine.
 *
 * Covers what the device path actually uses: a script whose document.write()
 * output replaces the <script> block, console.log() reaching the status line,
 * plain pages passing through untouched, and external scripts being skipped
 * while the rest of the markup survives.
 *
 * Build:
 *   g++ -std=gnu++17 -I firmware/tdeck-plus/browser
 *       -I third_party/TactileBrowser/tactilebrowser_core/elk
 *       firmware/tdeck-plus/test/host/test_browser_js.cpp
 *       firmware/tdeck-plus/browser/js_engine.cpp
 *       third_party/TactileBrowser/tactilebrowser_core/elk/elk.c -o /tmp/tjs
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "js_engine.h"

static int s_fails = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
            s_fails++;                                                         \
        }                                                                      \
    } while (0)

static void writes_build_markup(void)
{
    const char *html =
        "<html><body><h1>Report</h1><script>"
        "var n = 3;"
        "for (var i = 0; i < n; i++) { document.write('<p>row ' + i + '</p>'); }"
        "</script></body></html>";
    size_t len = 0;
    char *out = browserJsApplyScripts(html, strlen(html), &len);
    CHECK(out != nullptr);
    if (!out) return;
    CHECK(len == strlen(out));
    CHECK(strstr(out, "<p>row 0</p>") != nullptr);
    CHECK(strstr(out, "<p>row 2</p>") != nullptr);
    CHECK(strstr(out, "row 3") == nullptr);
    CHECK(strstr(out, "<script") == nullptr);  // the block was replaced
    CHECK(strstr(out, "<h1>Report</h1>") != nullptr);
    CHECK(browserJsLastError()[0] == 0);
    free(out);
}

static void console_reaches_status(void)
{
    const char *html = "<body><script>console.log('loaded', 42);</script></body>";
    size_t len = 0;
    char *out = browserJsApplyScripts(html, strlen(html), &len);
    CHECK(out != nullptr);
    CHECK(strcmp(browserJsLastLog(), "loaded 42") == 0);
    free(out);
}

static void no_script_passes_through(void)
{
    const char *html = "<p>plain</p>";
    size_t len = 1;
    CHECK(browserJsApplyScripts(html, strlen(html), &len) == nullptr);
    CHECK(len == 0);
    CHECK(browserJsLastLog()[0] == 0);
}

static void external_script_skipped(void)
{
    const char *html = "<body><script src=\"app.js\">var ignored = 1;</script><p>keep</p></body>";
    size_t len = 0;
    char *out = browserJsApplyScripts(html, strlen(html), &len);
    CHECK(out != nullptr);
    if (!out) return;
    CHECK(strstr(out, "<p>keep</p>") != nullptr);
    CHECK(strstr(out, "ignored") == nullptr);
    free(out);
}

static void broken_script_reports_error(void)
{
    const char *html = "<body><script>var x = ;</script><p>after</p></body>";
    size_t len = 0;
    char *out = browserJsApplyScripts(html, strlen(html), &len);
    CHECK(out != nullptr);
    CHECK(browserJsLastError()[0] != 0);
    if (out) CHECK(strstr(out, "<p>after</p>") != nullptr);
    free(out);
}

int main(void)
{
    writes_build_markup();
    console_reaches_status();
    no_script_passes_through();
    external_script_skipped();
    broken_script_reports_error();

    if (s_fails) printf("%d check(s) failed\n", s_fails);
    else printf("all browser JS checks passed\n");
    return s_fails ? 1 : 0;
}
