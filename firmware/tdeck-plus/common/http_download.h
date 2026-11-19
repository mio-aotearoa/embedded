// SPDX-License-Identifier: GPL-3.0-or-later
//
// One HTTP(S) download at a time, straight to the SD card, run on its own
// FreeRTOS task so the LVGL task keeps drawing. Callers poll httpDownloadState()
// from an lv_timer; nothing here touches LVGL.
//
// The download is written to "<dest>.part" and renamed only after every byte
// arrived, so an interrupted transfer never leaves a half file under the name
// the app treats as complete.
#pragma once

#include <stdint.h>

struct HttpDownloadState
{
    volatile bool running;    // a transfer is in progress
    volatile bool done;       // cleared by httpDownloadClearDone()
    volatile bool ok;         // with done: finished and renamed into place
    volatile uint64_t bytes;  // bytes written so far
    volatile uint64_t total;  // Content-Length, 0 when the server did not send one
    volatile int http_code;   // HTTP status of the final GET (0 before that)
    char err[96];             // human-readable failure reason (also set when Start fails)
    char path[192];           // destination the caller asked for
};

// Reserve the download worker stack early (while a contiguous block still
// exists). Safe to call repeatedly; returns false only when no stack fits.
bool httpDownloadWarmup(void);

// Starts the transfer. `expectedSize` is 0 when the caller has no pinned size;
// when it is non-zero the download fails if the byte count does not match.
// Returns false when a transfer is already running or the arguments are bad;
// on failure `httpDownloadState().err` explains why.
bool httpDownloadStart(const char *url, const char *destPath, uint64_t expectedSize);

const HttpDownloadState &httpDownloadState(void);

// Acknowledges a finished transfer so the next one can start cleanly.
void httpDownloadClearDone(void);

// Asks the running transfer to stop; it removes the partial file and reports
// "cancelled". No-op when nothing is running.
void httpDownloadCancel(void);

bool httpDownloadBusy(void);
