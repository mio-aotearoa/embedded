// SPDX-License-Identifier: GPL-3.0-or-later
//
// Raise the CPU to the board max while a heavy AppPage is open, then put the
// previous clock back when the last such page closes. Refcounted so nested
// acquire/release pairs stay balanced.
#pragma once

#include <stdint.h>

// ESP32-S3 Arduino max (and this board's screen-on default). Invalid values
// like 180 are rejected by the HAL and left the V4 stuck at 80 MHz before.
#ifndef WADAMESH_CPU_BOOST_MHZ
#define WADAMESH_CPU_BOOST_MHZ 240
#endif

// Bump to WADAMESH_CPU_BOOST_MHZ if the current clock is lower. Safe to call
// repeatedly; the first acquire remembers the prior MHz.
void cpuBoostAcquire(void);

// Drop one ref; when the count hits zero, restore the MHz saved by the first
// acquire (or leave the clock alone if something else changed it since).
void cpuBoostRelease(void);

uint32_t cpuBoostSavedMhz(void);
uint8_t cpuBoostDepth(void);
