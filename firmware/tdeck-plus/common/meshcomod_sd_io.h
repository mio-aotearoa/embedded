// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Lights the wadamesh status-bar microSD activity LED (UITask::markSdIo).
// Defined in third_party/wadamesh/src/ui-touch/UITask.cpp (our patch series).
#ifdef __cplusplus
extern "C" {
#endif
void meshcomodMarkSdIo(void);
#ifdef __cplusplus
}
#endif
