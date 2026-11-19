// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpu_boost.h"

#include <Arduino.h>

namespace
{
uint8_t s_depth = 0;
uint32_t s_saved_mhz = 0;
bool s_we_boosted = false;
}  // namespace

void cpuBoostAcquire(void)
{
#if defined(ESP32) && !defined(CONFIG_IDF_TARGET_ESP32P4)
    if (s_depth == 0)
    {
        s_saved_mhz = getCpuFrequencyMhz();
        s_we_boosted = false;
        if (s_saved_mhz < WADAMESH_CPU_BOOST_MHZ)
        {
            if (setCpuFrequencyMhz(WADAMESH_CPU_BOOST_MHZ))
            {
                s_we_boosted = true;
                Serial.printf("[cpu] boost %u -> %u MHz\n", (unsigned)s_saved_mhz,
                              (unsigned)WADAMESH_CPU_BOOST_MHZ);
            }
        }
    }
    if (s_depth < 255) ++s_depth;
#else
    if (s_depth < 255) ++s_depth;
#endif
}

void cpuBoostRelease(void)
{
    if (s_depth == 0) return;
    --s_depth;
#if defined(ESP32) && !defined(CONFIG_IDF_TARGET_ESP32P4)
    if (s_depth != 0) return;
    if (!s_we_boosted) return;
    // Only undo our own boost: if another path changed the clock, leave it.
    if (getCpuFrequencyMhz() == WADAMESH_CPU_BOOST_MHZ && s_saved_mhz > 0 &&
        s_saved_mhz != WADAMESH_CPU_BOOST_MHZ)
    {
        setCpuFrequencyMhz(s_saved_mhz);
        Serial.printf("[cpu] restore %u MHz\n", (unsigned)s_saved_mhz);
    }
    s_we_boosted = false;
    s_saved_mhz = 0;
#endif
}

uint32_t cpuBoostSavedMhz(void) { return s_saved_mhz; }
uint8_t cpuBoostDepth(void) { return s_depth; }
