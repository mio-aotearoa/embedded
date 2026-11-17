// SPDX-License-Identifier: GPL-3.0-or-later
//
// Catalogue browsing for the Kiwix mirror: lists https://download.kiwix.org/zim/
// so the Wi-Fi download can offer what is available instead of asking for a URL.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

struct KiwixCatalogItem
{
    char name[96];   // file or folder name, no trailing slash
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
// Returns false when a fetch is already running (or the arguments are bad).
// Redirects are followed by hand: the mirror 302s to lb.download.kiwix.org, and
// HTTPClient cannot re-connect an explicit WiFiClientSecure across hosts.
bool kiwixCatalogStart(const char* dir);
const KiwixCatalogState& kiwixCatalogState(void);
void kiwixCatalogClearDone(void);
