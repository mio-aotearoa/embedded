// SPDX-License-Identifier: GPL-3.0-or-later
//
// LocalSend (protocol v2, HTTP-only) for the T-Deck Plus.
//
// Protocol: https://github.com/localsend/protocol (v2, HTTP-only). No ESP32
// LocalSend library exists; Flutter/Rust/TS ports are not linkable here. JSON
// uses ArduinoJson (already in the wadamesh env); transfer is WiFiClient +
// HTTPClient — same stack as the rest of the firmware.
//
// Scope: discover peers; send one SD file; receive one file to SD while the
// page is open (:53317). TLS peers, folders and multi-file batches are out.
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

    struct RecvState
    {
        bool listening;   // HTTP receiver is up
        bool busy;        // a transfer is in progress
        bool done;        // last transfer finished (sticky until clearRecvDone)
        bool ok;
        uint32_t received;
        uint32_t total;
        char fileName[64];
        char err[64];
    };

    // Discovery: listens for multicast announcements, then asks each host in
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

    // Receive: bind :53317, announce on multicast, accept prepare-upload/upload.
    // Auto-accepts one file onto /localsend/<name> on the SD card.
    static bool startReceiver(void);
    static void stopReceiver(void);
    static const RecvState &recvState(void);
    static void clearRecvDone(void);
};
