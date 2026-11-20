// SPDX-License-Identifier: GPL-3.0-or-later
#include "game_steer.h"

#include <Arduino.h>

namespace
{
void fire(GameSteer *s)
{
    if (s && s->apply) s->apply(s->left, s->right, s->ctx);
}

void touchCb(lv_event_t *e)
{
    auto *s = static_cast<GameSteer *>(lv_event_get_user_data(e));
    if (!s || !s->canvas) return;
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
    {
        s->left = s->right = false;
        s->steerUntil = 0;
        fire(s);
        return;
    }
    lv_point_t p;
    lv_indev_get_point(lv_indev_get_act(), &p);
    lv_area_t a;
    lv_obj_get_content_coords(s->canvas, &a);
    const lv_coord_t mid = (a.x1 + a.x2) / 2;
    s->left = p.x < mid;
    s->right = p.x >= mid;
    s->steerUntil = millis() + 1000;
    fire(s);
}

void gestureCb(lv_event_t *e)
{
    auto *s = static_cast<GameSteer *>(lv_event_get_user_data(e));
    if (!s) return;
    switch (lv_indev_get_gesture_dir(lv_indev_get_act()))
    {
        case LV_DIR_LEFT:
            gameSteerNudge(s, -1, 280);
            break;
        case LV_DIR_RIGHT:
            gameSteerNudge(s, 1, 280);
            break;
        default:
            break;
    }
}
}  // namespace

void gameSteerBind(GameSteer *s, lv_obj_t *root, lv_obj_t *canvas,
                   void (*apply)(bool left, bool right, void *ctx), void *ctx)
{
    if (!s || !root || !canvas) return;
    s->canvas = canvas;
    s->apply = apply;
    s->ctx = ctx;
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(canvas, touchCb, LV_EVENT_PRESSED, s);
    lv_obj_add_event_cb(canvas, touchCb, LV_EVENT_PRESSING, s);
    lv_obj_add_event_cb(canvas, touchCb, LV_EVENT_RELEASED, s);
    lv_obj_add_event_cb(canvas, touchCb, LV_EVENT_PRESS_LOST, s);
    lv_obj_add_event_cb(root, gestureCb, LV_EVENT_GESTURE, s);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

void gameSteerNudge(GameSteer *s, int dx, uint32_t holdMs)
{
    if (!s || dx == 0) return;
    s->left = dx < 0;
    s->right = dx > 0;
    s->steerUntil = millis() + holdMs;
    fire(s);
}

void gameSteerTick(GameSteer *s)
{
    if (!s) return;
    if (s->steerUntil && (int32_t)(millis() - s->steerUntil) >= 0 &&
        (!s->canvas || !lv_obj_has_state(s->canvas, LV_STATE_PRESSED)))
    {
        s->left = s->right = false;
        s->steerUntil = 0;
        fire(s);
    }
    if (s->hint && s->hintUntil && (int32_t)(millis() - s->hintUntil) >= 0)
    {
        lv_obj_add_flag(s->hint, LV_OBJ_FLAG_HIDDEN);
        s->hintUntil = 0;
    }
}

lv_obj_t *gameSteerMakeHint(GameSteer *s, lv_obj_t *root, const char *text, uint32_t showMs)
{
    if (!s || !root || !text) return nullptr;
    lv_obj_t *hint = lv_label_create(root);
    lv_label_set_text(hint, text);
    lv_obj_set_style_text_color(hint, lv_color_hex(0xFFE08A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(hint, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hint, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(hint, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(hint, 2, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 4);
    s->hint = hint;
    s->hintUntil = millis() + showMs;
    return hint;
}
