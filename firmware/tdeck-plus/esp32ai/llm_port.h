// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>

// Real slvDev/esp32-ai TinyStories runtime, weights on the SD card.
//
// model.bin + tokenizer.json must sit at /esp32ai/tinystories/ with the pinned
// byte sizes. EnsureLoaded stages the int8 hot set into PSRAM and leaves the
// PLE table / token embedding on the card (per-token SD reads).

bool esp32aiModelReady(void);

// Load weights + scratch. Returns false with a short reason in err (may be null).
bool esp32aiEnsureLoaded(char *err, size_t errCap);

void esp32aiUnload(void);

bool esp32aiIsLoaded(void);

// Optional per-token UTF-8 callback (may be null). Generate is blocking.
// Returns bytes written to out (NUL-terminated if outCap>0), or -1 on error.
typedef void (*Esp32AiTokenFn)(const char *utf8, int len, void *user);

int esp32aiGenerate(const char *prompt, char *out, size_t outCap, int maxTokens,
                    Esp32AiTokenFn onToken, void *user, char *err, size_t errCap);
