// SPDX-License-Identifier: GPL-3.0-or-later
//
// Catalogue browsing for the Kiwix mirror: lists https://ftp.fau.de/kiwix/zim/
// (Apache index; download.kiwix.org/zim/ root now redirects to hub) so the
// Wi-Fi download can offer what is available instead of asking for a URL.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

struct KiwixCatalogItem
{
    char name[128];  // file or folder name, no trailing slash
    char size[16];   // "48M" exactly as the mirror prints it (empty for folders)
    uint64_t bytes;  // size parsed from `size`, 0 when the mirror gave none
    bool isDir;
};

struct KiwixCatalogState
{
    volatile bool running;   // a listing is being fetched
    volatile bool done;      // cleared by kiwixCatalogClearDone()
    volatile bool ok;        // with done: `items` holds the listing
    char err[96];            // readable reason when !ok
    const std::vector<KiwixCatalogItem>* items;  // owned by this module, valid while done
};

// Starts fetching one directory listing on a worker task, so the LVGL task keeps
// drawing while the mirror answers. `dir` is "" for /zim/ itself, else a path
// such as "wikipedia" or "wikipedia/wikipedia_en_all_nopic".
// If a fetch is already running, the path is queued and replaces any previous
// pending path (the worker picks it up when the current hop finishes). Returns
// false only when the arguments are bad or the worker task cannot be created.
// HTTPS uses shared common/http_fetch (manual redirects + TLS in PSRAM).
// Spawn the long-lived worker early (e.g. when the Kiwix page opens), while a
// contiguous internal block for its stack is still available. Safe to call
// repeatedly; returns false only when no stack could be allocated.
bool kiwixCatalogWarmup(void);
bool kiwixCatalogStart(const char* dir);
const KiwixCatalogState& kiwixCatalogState(void);
void kiwixCatalogClearDone(void);
