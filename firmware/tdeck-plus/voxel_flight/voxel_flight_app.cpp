// SPDX-License-Identifier: GPL-3.0-or-later
#include "voxel_flight_app.h"

#include "app_toast.h"
#include "cpu_boost.h"
#include "game_canvas.h"
#include "i18n.h"
#include "voxel_host.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

VoxelFlightApp* VoxelFlightApp::s_active = nullptr;
char VoxelFlightApp::barTitle_[24];

bool VoxelFlightApp::isOpen(void) { return s_active != nullptr && s_active->root_ != nullptr; }

void VoxelFlightApp::steer(int dx, int dy)
{
    (void)dy;
    if (!s_active || dx == 0) return;
    s_active->left_ = dx < 0;
    s_active->right_ = dx > 0;
    s_active->steerUntil_ = millis() + 280;
    voxelHostSetSteer(dx < 0 ? -1 : 1);
}

void VoxelFlightApp::launch(void)
{
    if (s_active) return;
    VoxelFlightApp* app = new VoxelFlightApp();
    s_active = app;
    if (!app->open())
    {
        s_active = nullptr;
        delete app;
    }
}

void VoxelFlightApp::destroyAsync(void* p) { delete static_cast<VoxelFlightApp*>(p); }

void VoxelFlightApp::dismiss(void)
{
    if (!s_active) return;
    VoxelFlightApp* app = s_active;
    s_active = nullptr;
    app->close();
    lv_async_call(destroyAsync, app);
}

VoxelFlightApp::~VoxelFlightApp() { voxelHostEnd(); }

bool VoxelFlightApp::open(void)
{
    cpuBoostAcquire();  // ray-march + map gen; restore on close/dismiss

    snprintf(barTitle_, sizeof barTitle_, "%s", TR("Voxel Flight"));

    const int want_w = gameCanvasPanelW();
    const int want_h = gameCanvasPanelH();

    if (!voxelHostBegin(want_w, want_h))
    {
        cpuBoostRelease();
        return false;
    }

    GameCanvas gc{};
    lv_color_t* fb = reinterpret_cast<lv_color_t*>(voxelHostFramebuffer());
    if (!fb || !gameCanvasOpen(&gc, 0x000010, barTitle_, &VoxelFlightApp::dismiss, fb,
                               voxelHostWidth(), voxelHostHeight()))
    {
        if (gc.root) gameCanvasClose(&gc, &VoxelFlightApp::dismiss);
        voxelHostEnd();
        cpuBoostRelease();
        return false;
    }
    root_ = gc.root;
    canvas_ = gc.canvas;

    lv_obj_add_flag(canvas_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(canvas_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(canvas_, touchCb, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(canvas_, touchCb, LV_EVENT_PRESSING, this);
    lv_obj_add_event_cb(canvas_, touchCb, LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(canvas_, touchCb, LV_EVENT_PRESS_LOST, this);
    lv_obj_add_event_cb(root_, gestureCb, LV_EVENT_GESTURE, this);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_GESTURE_BUBBLE);

    hint_ = lv_label_create(root_);
    lv_label_set_text(hint_, TR("Hold L/R to bank — release = autopilot"));
    lv_obj_set_style_text_color(hint_, lv_color_hex(0xFFE08A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(hint_, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hint_, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(hint_, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(hint_, 2, LV_PART_MAIN);
    lv_obj_align(hint_, LV_ALIGN_TOP_MID, 0, 4);
    hintUntil_ = millis() + 4500;
    appToast(TR("Hold L/R to bank — release = autopilot"), 2800);

    lv_obj_invalidate(canvas_);
    timer_ = lv_timer_create(timerCb, 33, this);
    return true;
}

void VoxelFlightApp::close(void)
{
    if (timer_)
    {
        lv_timer_del(timer_);
        timer_ = nullptr;
    }
    voxelHostSetSteer(0);
    voxelHostEnd();
    GameCanvas gc{};
    gc.root = root_;
    gc.canvas = canvas_;
    gc.buf = reinterpret_cast<lv_color_t*>(voxelHostFramebuffer());
    gc.own_buf = false;
    gameCanvasClose(&gc, &VoxelFlightApp::dismiss);
    root_ = nullptr;
    canvas_ = nullptr;
    hint_ = nullptr;
    cpuBoostRelease();
}

void VoxelFlightApp::timerCb(lv_timer_t* t)
{
    auto* self = static_cast<VoxelFlightApp*>(t->user_data);
    if (!self || !self->canvas_) return;
    if (self->steerUntil_ && (int32_t)(millis() - self->steerUntil_) >= 0 &&
        !lv_obj_has_state(self->canvas_, LV_STATE_PRESSED))
    {
        self->left_ = self->right_ = false;
        self->steerUntil_ = 0;
        voxelHostSetSteer(0);
    }
    if (self->hint_ && self->hintUntil_ && (int32_t)(millis() - self->hintUntil_) >= 0)
    {
        lv_obj_add_flag(self->hint_, LV_OBJ_FLAG_HIDDEN);
        self->hintUntil_ = 0;
    }
    voxelHostTick();
    voxelHostSetButton(false);
    lv_obj_invalidate(self->canvas_);
}

void VoxelFlightApp::gestureCb(lv_event_t* e)
{
    auto* self = static_cast<VoxelFlightApp*>(lv_event_get_user_data(e));
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

void VoxelFlightApp::touchCb(lv_event_t* e)
{
    auto* self = static_cast<VoxelFlightApp*>(lv_event_get_user_data(e));
    if (!self || !self->canvas_) return;
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    {
        self->left_ = self->right_ = false;
        self->steerUntil_ = 0;
        voxelHostSetSteer(0);
        return;
    }
    lv_point_t p;
    lv_indev_get_point(lv_indev_get_act(), &p);
    lv_area_t a;
    lv_obj_get_content_coords(self->canvas_, &a);
    const lv_coord_t mid = (a.x1 + a.x2) / 2;
    self->left_ = p.x < mid;
    self->right_ = p.x >= mid;
    self->steerUntil_ = millis() + 1000;
    voxelHostSetSteer(self->left_ ? -1 : 1);
}
