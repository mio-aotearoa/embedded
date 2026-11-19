// SPDX-License-Identifier: GPL-3.0-or-later
#include "arcade3d_app.h"

#include "arcade_config.h"  // SCR_W / SCR_H (patched submodule)
#include "arcade_host.h"
#include "cpu_boost.h"
#include "game_canvas.h"
#include "i18n.h"

#include <Arduino.h>
#include <LvglPsramAlloc.h>
#include <stdio.h>
#include <string.h>

Arcade3dApp* Arcade3dApp::s_active = nullptr;
char Arcade3dApp::barTitle_[24];

bool Arcade3dApp::isOpen(void) { return s_active != nullptr && s_active->root_ != nullptr; }

void Arcade3dApp::steer(int dx, int dy)
{
    (void)dy;
    if (!s_active) return;
    if (dx == 0) return;
    s_active->left_ = dx < 0;
    s_active->right_ = dx > 0;
    s_active->steerUntil_ = millis() + 280;  // hold a beat after trackball nudge
    arcadeHostSetSteer(s_active->left_, s_active->right_);
}

void Arcade3dApp::launch(void)
{
    if (s_active) return;
    Arcade3dApp* app = new Arcade3dApp();
    s_active = app;
    if (!app->open())
    {
        s_active = nullptr;
        delete app;
    }
}

void Arcade3dApp::destroyAsync(void* p) { delete static_cast<Arcade3dApp*>(p); }

void Arcade3dApp::dismiss(void)
{
    if (!s_active) return;
    Arcade3dApp* app = s_active;
    s_active = nullptr;
    app->close();
    lv_async_call(destroyAsync, app);
}

Arcade3dApp::~Arcade3dApp()
{
    arcadeHostEnd();
    if (game_buf_)
    {
        lvglPsramFree(game_buf_);
        game_buf_ = nullptr;
    }
}

bool Arcade3dApp::open(void)
{
    cpuBoostAcquire();

    game_buf_ = (lv_color_t*)lvglPsramAlloc((size_t)SCR_W * (size_t)SCR_H * sizeof(lv_color_t));
    if (!game_buf_)
    {
        cpuBoostRelease();
        return false;
    }

    snprintf(barTitle_, sizeof barTitle_, "%s", TR("OutRun"));
    GameCanvas gc{};
    if (!gameCanvasOpen(&gc, 0x000000, barTitle_, &Arcade3dApp::dismiss, game_buf_, SCR_W, SCR_H) ||
        !arcadeHostBegin(reinterpret_cast<uint16_t*>(game_buf_)))
    {
        if (gc.root) gameCanvasClose(&gc, &Arcade3dApp::dismiss);
        lvglPsramFree(game_buf_);
        game_buf_ = nullptr;
        cpuBoostRelease();
        return false;
    }
    root_ = gc.root;
    canvas_ = gc.canvas;
    // game_buf_ stays owned by this app (external to GameCanvas).

    lv_obj_add_flag(canvas_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(canvas_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(canvas_, touchCb, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(canvas_, touchCb, LV_EVENT_PRESSING, this);
    lv_obj_add_event_cb(canvas_, touchCb, LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(canvas_, touchCb, LV_EVENT_PRESS_LOST, this);
    lv_obj_add_event_cb(root_, gestureCb, LV_EVENT_GESTURE, this);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_GESTURE_BUBBLE);

    hint_ = lv_label_create(root_);
    lv_label_set_text(hint_, TR("Throttle auto — hold L/R side to steer"));
    lv_obj_set_style_text_color(hint_, lv_color_hex(0xFFE08A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(hint_, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hint_, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(hint_, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(hint_, 2, LV_PART_MAIN);
    lv_obj_align(hint_, LV_ALIGN_TOP_MID, 0, 4);
    hintUntil_ = millis() + 4000;

    lv_obj_invalidate(canvas_);
    timer_ = lv_timer_create(timerCb, 33, this);  // ~30 fps
    return true;
}

void Arcade3dApp::close(void)
{
    if (timer_)
    {
        lv_timer_del(timer_);
        timer_ = nullptr;
    }
    arcadeHostSetSteer(false, false);
    arcadeHostEnd();
    GameCanvas gc{};
    gc.root = root_;
    gc.canvas = canvas_;
    gc.buf = game_buf_;
    gc.own_buf = false;
    gameCanvasClose(&gc, &Arcade3dApp::dismiss);
    root_ = nullptr;
    canvas_ = nullptr;
    hint_ = nullptr;
    cpuBoostRelease();
}

void Arcade3dApp::timerCb(lv_timer_t* t)
{
    auto* self = static_cast<Arcade3dApp*>(t->user_data);
    if (!self || !self->canvas_) return;
    // Trackball nudges expire; finger-hold is cleared in touchCb on release.
    if (self->steerUntil_ && (int32_t)(millis() - self->steerUntil_) >= 0 &&
        !lv_obj_has_state(self->canvas_, LV_STATE_PRESSED))
    {
        self->left_ = self->right_ = false;
        self->steerUntil_ = 0;
        arcadeHostSetSteer(false, false);
    }
    if (self->hint_ && self->hintUntil_ && (int32_t)(millis() - self->hintUntil_) >= 0)
    {
        lv_obj_add_flag(self->hint_, LV_OBJ_FLAG_HIDDEN);
        self->hintUntil_ = 0;
    }
    arcadeHostTick();
    lv_obj_invalidate(self->canvas_);
}

void Arcade3dApp::gestureCb(lv_event_t* e)
{
    auto* self = static_cast<Arcade3dApp*>(lv_event_get_user_data(e));
    if (!self) return;
    switch (lv_indev_get_gesture_dir(lv_indev_get_act()))
    {
        case LV_DIR_LEFT:
            self->steer(-1, 0);
            break;
        case LV_DIR_RIGHT:
            self->steer(1, 0);
            break;
        default:
            break;
    }
}

void Arcade3dApp::touchCb(lv_event_t* e)
{
    auto* self = static_cast<Arcade3dApp*>(lv_event_get_user_data(e));
    if (!self || !self->canvas_) return;
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    {
        self->left_ = self->right_ = false;
        self->steerUntil_ = 0;
        arcadeHostSetSteer(false, false);
        return;
    }
    lv_point_t p;
    lv_indev_get_point(lv_indev_get_act(), &p);
    lv_area_t a;
    lv_obj_get_content_coords(self->canvas_, &a);
    const lv_coord_t mid = (a.x1 + a.x2) / 2;
    self->left_ = p.x < mid;
    self->right_ = p.x >= mid;
    self->steerUntil_ = millis() + 1000;  // kept alive by PRESSING
    arcadeHostSetSteer(self->left_, self->right_);
}
