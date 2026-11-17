// airdrop_bridge.h -- run one AirDrop receive window on an esp32-photoframe
// device, then hand the received image to the caller.
//
// ESP32Drop's AWDL link takes the 2.4 GHz radio exclusively, so this cannot run
// beside esp32-photoframe's Wi-Fi station. It is a boot mode: run the window,
// receive one image, tear AWDL down, then let photoframe's own image pipeline
// put the file on the panel.
//
// The implementation compiles ESP32Drop's receive path unmodified
// (awdl_port_esp32.cpp + ad_port_esp32.cpp) behind a tiny Arduino shim.
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
// caller may start Wi-Fi. On timeout/error nothing is left running.
//
// Blocking; call from app_main (or a task with >= 8 KB stack headroom).
esp_err_t airdrop_bridge_receive_to_file(char *out_path, size_t out_path_cap, uint32_t timeout_ms);

// Release the radio if a window was started and not finished (error paths).
void airdrop_bridge_abort(void);

#ifdef __cplusplus
}
#endif
