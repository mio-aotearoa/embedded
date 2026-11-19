// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "AppPage.h"

#include <LvglPsramAlloc.h>
#include <lvgl.h>
#include <stdint.h>

// Shared slim-bar AppPage + RGB565 canvas for the T-Deck games. Callers either
// pass an external framebuffer (game owns it) or leave buf null to allocate.
struct GameCanvas
{
    lv_obj_t* root = nullptr;
    lv_obj_t* canvas = nullptr;
    lv_color_t* buf = nullptr;
    bool own_buf = false;
    int w = 0;
    int h = 0;
};

inline int gameCanvasPanelW(void)
{
    const lv_coord_t w = lv_disp_get_hor_res(nullptr);
    return w > 0 ? (int)w : 320;
}

// Usable height under the status bar. Prefer after appPageBeginSlim so cover is 0.
inline int gameCanvasPanelH(void)
{
    const lv_coord_t h = appPageContentH();
    return h > 0 ? (int)h : 218;
}

inline bool gameCanvasOpen(GameCanvas* g, uint32_t bg, const char* title, void (*dismiss)(),
                           lv_color_t* external_buf, int w, int h)
{
    if (!g || w <= 0 || h <= 0) return false;
    g->root = appPageCreateRoot(bg);
    if (!g->root) return false;
    appPageBeginSlim(title, dismiss);
    g->w = w;
    g->h = h;
    g->own_buf = external_buf == nullptr;
    g->buf = external_buf ? external_buf
                          : (lv_color_t*)lvglPsramAlloc((size_t)w * (size_t)h * sizeof(lv_color_t));
    if (!g->buf)
    {
        appPageEnd(dismiss);
        appPageDeleteRootAsync(g->root);
        g->root = nullptr;
        return false;
    }
    g->canvas = lv_canvas_create(g->root);
    lv_canvas_set_buffer(g->canvas, g->buf, w, h, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_size(g->canvas, w, h);
    // Bottom-align so a taller native frame (e.g. OutRun 320×240) keeps the HUD
    // when the slim content area is a few rows shorter than the panel.
    lv_obj_align(g->canvas, LV_ALIGN_BOTTOM_MID, 0, 0);
    return true;
}

inline void gameCanvasClose(GameCanvas* g, void (*dismiss)())
{
    if (!g) return;
    appPageEnd(dismiss);
    appPageDeleteRootAsync(g->root);
    g->root = nullptr;
    g->canvas = nullptr;
    if (g->own_buf && g->buf)
    {
        lvglPsramFree(g->buf);
        g->buf = nullptr;
    }
    g->own_buf = false;
    g->w = g->h = 0;
}
