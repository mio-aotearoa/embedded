// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// System-layer toast used by wadamesh (lv_layer_sys), so it stays visible over
// our AppPages on lv_layer_top. Implemented in the wadamesh patch as
// wadameshShowAlert → UITask::showAlert.
extern "C" void wadameshShowAlert(const char *text, int duration_ms);

inline void appToast(const char *text, int duration_ms = 2400)
{
    if (!text || !text[0]) return;
    wadameshShowAlert(text, duration_ms);
}
