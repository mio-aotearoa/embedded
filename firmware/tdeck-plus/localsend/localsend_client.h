// SPDX-License-Identifier: GPL-3.0-or-later
//
// LocalSend (protocol v2, HTTP-only) sender for the T-Deck Plus.
//
// Written from the protocol description at https://github.com/localsend/protocol:
// no upstream client code is used (the official app is Flutter, the other
// implementations are Rust/Go/Win32), so this file is this repository's own
// code and there is nothing to vendor.
//
// Scope: discover peers on the LAN, then send one file from the SD card to one
// peer. Receiving, TLS/HTTPS peers, folders and multi-file batches are out of
// scope and reported as such in the UI.
#pragma once

#include <stdint.h>

struct LsPeer
{
    char alias[33];
    char ip[16];
    uint16_t port;
};

class LocalSendClient
{
  public:
    struct State
    {
        bool running;
        bool done;
        bool ok;
        uint32_t sent;
        uint32_t total;
        char err[64];
    };

    // Discovery: listens for multicast announcements, then asks every host in
    // the local /24 for /api/localsend/v2/info. Runs in its own task.
    static bool startScan(void);
    static void cancelScan(void);
    static bool scanning(void);
    static int scanProgress(void);   // 0..254, 255 = finished
    static int peerCount(void);
    static bool peer(int index, LsPeer &out);

    // One file, one peer. Poll state() for progress.
    static bool beginSend(const char *sdPath, int peerIndex);
    static void cancelSend(void);
    static const State &state(void);
    static void clearDone(void);
};
