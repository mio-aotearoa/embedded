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

// Fetches one directory listing. `dir` is "" for /zim/ itself, else a path such
// as "wikipedia" or "wikipedia/wikipedia_en_all_nopic".
//
// Blocking (call it from the LVGL task with a status already painted). It
// follows redirects by hand because the mirror 302s to lb.download.kiwix.org and
// HTTPClient cannot re-connect an explicit WiFiClientSecure across hosts.
// Returns false and writes a readable reason to `err` on failure.
bool kiwixCatalogFetch(const char* dir, std::vector<KiwixCatalogItem>& out,
                       char* err, size_t errLen);
