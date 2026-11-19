// airdrop_bridge.h -- run one AirDrop receive window on an esp32-photoframe
// device, then hand the received image to the caller.
//
// Default (switch): exclusive boot-time receive window — ESP32Drop owns the
// radio alone, as upstream documents.
//
// CONFIG_AIRDROP_COEX: ch6 2.4 GHz dual-protocol receive experiment. The router
// must already be on channel 6 (the only 2.4 GHz AWDL channel). We keep the
// photoframe STA/HTTP path up and attach AWDL via APSTA so the same radio can
// receive ordinary Wi-Fi frames and AWDL/AirDrop frames on that channel.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// The name that appears in the sender's AirDrop share sheet.
#ifndef AIRDROP_BRIDGE_NAME
#define AIRDROP_BRIDGE_NAME "reTerminal"
#endif

// How long the AirDrop window stays open once started, in milliseconds.
#ifndef AIRDROP_BRIDGE_WINDOW_MS
#define AIRDROP_BRIDGE_WINDOW_MS (180 * 1000)
#endif

// Only 2.4 GHz AWDL channel; coex requires the STA's AP to be on this channel.
#ifndef AIRDROP_COEX_CHANNEL
#define AIRDROP_COEX_CHANNEL 6
#endif

// Upper bound on the decoded transfer held in PSRAM. 0 keeps ESP32Drop's own
// default (6 MiB); the panel only needs a few hundred KB, so this is set lower
// to leave PSRAM for photoframe's image buffers.
#ifndef AIRDROP_BRIDGE_MAX_RECEIVE_BYTES
#define AIRDROP_BRIDGE_MAX_RECEIVE_BYTES (4u * 1024u * 1024u)
#endif

// Boot-mode trigger: the button to hold at power-on. Defaults to the board's
// wake/select button, pulled up, active low.
#ifndef AIRDROP_BRIDGE_TRIGGER_GPIO
#include "board_hal.h"
#define AIRDROP_BRIDGE_TRIGGER_GPIO BOARD_HAL_WAKEUP_KEY
#endif
#ifndef AIRDROP_BRIDGE_TRIGGER_ACTIVE_LOW
#define AIRDROP_BRIDGE_TRIGGER_ACTIVE_LOW 1
#endif

// True when the device should enter the AirDrop boot window instead of normal
// photoframe operation. Reads the trigger pin; safe before Wi-Fi is started.
bool airdrop_bridge_boot_requested(void);

// Run the receive window. On ESP_OK `out_path` holds the received JPEG or PNG
// (inside the photoframe storage mount) and the radio has been released, so the
// caller may start Wi-Fi. Coex keeps the already-running ch6 station up so Wi-Fi
// and AWDL can both receive during the window.
// A failed teardown keeps is_active() true; reboot before reusing the radio.
//
// Blocking; call from app_main (or a task with >= 8 KB stack headroom).
esp_err_t airdrop_bridge_receive_to_file(char *out_path, size_t out_path_cap, uint32_t timeout_ms);

bool airdrop_bridge_is_active(void);

// Release the radio if a window was started and not finished (error paths).
void airdrop_bridge_abort(void);

#ifdef __cplusplus
}
#endif
