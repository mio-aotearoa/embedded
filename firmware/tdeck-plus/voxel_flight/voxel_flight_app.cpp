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

VoxelFlightApp *VoxelFlightApp::s_active = nullptr;
char VoxelFlightApp::barTitle_[24];

bool VoxelFlightApp::isOpen(void) { return s_active != nullptr && s_active->root_ != nullptr; }

void VoxelFlightApp::applySteer(bool left, bool right, void *ctx)
{
    (void)ctx;
    if (left) voxelHostSetSteer(-1);
    else if (right) voxelHostSetSteer(1);
    else voxelHostSetSteer(0);
}

void VoxelFlightApp::steer(int dx, int dy)
{
    (void)dy;
    if (!s_active) return;
    gameSteerNudge(&s_active->steer_, dx, 280);
}

void VoxelFlightApp::launch(void)
{
    if (s_active) return;
    VoxelFlightApp *app = new VoxelFlightApp();
    s_active = app;
    if (!app->open())
    {
        s_active = nullptr;
        delete app;
    }
}

void VoxelFlightApp::destroyAsync(void *p) { delete static_cast<VoxelFlightApp *>(p); }

void VoxelFlightApp::dismiss(void)
{
    if (!s_active) return;
    VoxelFlightApp *app = s_active;
    s_active = nullptr;
    app->close();
    lv_async_call(destroyAsync, app);
}

VoxelFlightApp::~VoxelFlightApp() { voxelHostEnd(); }

bool VoxelFlightApp::open(void)
{
    cpuBoostAcquire();

    snprintf(barTitle_, sizeof barTitle_, "%s", TR("Voxel Flight"));
    const int want_w = gameCanvasPanelW();
    const int want_h = gameCanvasPanelH();

    if (!voxelHostBegin(want_w, want_h))
    {
        cpuBoostRelease();
        return false;
    }

    GameCanvas gc{};
    lv_color_t *fb = reinterpret_cast<lv_color_t *>(voxelHostFramebuffer());
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

    gameSteerBind(&steer_, root_, canvas_, &VoxelFlightApp::applySteer, this);
    gameSteerMakeHint(&steer_, root_, TR("Hold L/R to bank — release = autopilot"), 4500);
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
    gc.buf = reinterpret_cast<lv_color_t *>(voxelHostFramebuffer());
    gc.own_buf = false;
    gameCanvasClose(&gc, &VoxelFlightApp::dismiss);
    root_ = nullptr;
    canvas_ = nullptr;
    steer_ = {};
    cpuBoostRelease();
}

void VoxelFlightApp::timerCb(lv_timer_t *t)
{
    auto *self = static_cast<VoxelFlightApp *>(t->user_data);
    if (!self || !self->canvas_) return;
    gameSteerTick(&self->steer_);
    voxelHostTick();
    voxelHostSetButton(false);
    lv_obj_invalidate(self->canvas_);
}
