// SPDX-License-Identifier: GPL-3.0-or-later
#include "arcade3d_app.h"

#include "arcade_config.h"
#include "arcade_host.h"
#include "cpu_boost.h"
#include "game_canvas.h"
#include "i18n.h"

#include <Arduino.h>
#include <LvglPsramAlloc.h>
#include <stdio.h>
#include <string.h>

Arcade3dApp *Arcade3dApp::s_active = nullptr;
char Arcade3dApp::barTitle_[24];

bool Arcade3dApp::isOpen(void) { return s_active != nullptr && s_active->root_ != nullptr; }

void Arcade3dApp::applySteer(bool left, bool right, void *ctx)
{
    (void)ctx;
    arcadeHostSetSteer(left, right);
}

void Arcade3dApp::steer(int dx, int dy)
{
    (void)dy;
    if (!s_active) return;
    gameSteerNudge(&s_active->steer_, dx, 280);
}

void Arcade3dApp::launch(void)
{
    if (s_active) return;
    Arcade3dApp *app = new Arcade3dApp();
    s_active = app;
    if (!app->open())
    {
        s_active = nullptr;
        delete app;
    }
}

void Arcade3dApp::destroyAsync(void *p) { delete static_cast<Arcade3dApp *>(p); }

void Arcade3dApp::dismiss(void)
{
    if (!s_active) return;
    Arcade3dApp *app = s_active;
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

    game_buf_ = (lv_color_t *)lvglPsramAlloc((size_t)SCR_W * (size_t)SCR_H * sizeof(lv_color_t));
    if (!game_buf_)
    {
        cpuBoostRelease();
        return false;
    }

    snprintf(barTitle_, sizeof barTitle_, "%s", TR("OutRun"));
    GameCanvas gc{};
    if (!gameCanvasOpen(&gc, 0x000000, barTitle_, &Arcade3dApp::dismiss, game_buf_, SCR_W, SCR_H) ||
        !arcadeHostBegin(reinterpret_cast<uint16_t *>(game_buf_)))
    {
        if (gc.root) gameCanvasClose(&gc, &Arcade3dApp::dismiss);
        lvglPsramFree(game_buf_);
        game_buf_ = nullptr;
        cpuBoostRelease();
        return false;
    }
    root_ = gc.root;
    canvas_ = gc.canvas;

    gameSteerBind(&steer_, root_, canvas_, &Arcade3dApp::applySteer, this);
    gameSteerMakeHint(&steer_, root_, TR("Throttle auto — hold L/R side to steer"), 4000);

    lv_obj_invalidate(canvas_);
    timer_ = lv_timer_create(timerCb, 33, this);
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
    steer_ = {};
    cpuBoostRelease();
}

void Arcade3dApp::timerCb(lv_timer_t *t)
{
    auto *self = static_cast<Arcade3dApp *>(t->user_data);
    if (!self || !self->canvas_) return;
    gameSteerTick(&self->steer_);
    arcadeHostTick();
    lv_obj_invalidate(self->canvas_);
}
