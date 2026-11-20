// SPDX-License-Identifier: GPL-3.0-or-later
#include "ime_cand.h"

#include "ime_font.h"

#include <stdio.h>
#include <string.h>

namespace
{
lv_obj_t *s_bar = nullptr;
lv_obj_t *s_label = nullptr;
ImeCandList s_list{};
ImeCandPickFn s_onPick = nullptr;
void *s_user = nullptr;

void refreshLabel(void)
{
    if (!s_label) return;
    char buf[420];
    size_t o = 0;
    for (int i = 0; i < s_list.count && o + 8 < sizeof buf; ++i)
    {
        const char *mark = (i == s_list.selected) ? ">" : " ";
        int n = snprintf(buf + o, sizeof buf - o, "%s%d.%s ", mark, i + 1, s_list.items[i]);
        if (n < 0) break;
        o += (size_t)n;
    }
    lv_label_set_text(s_label, buf);
}

void ensureBar(void)
{
    if (s_bar) return;
    // lv_layer_sys so AppPages on lv_layer_top do not cover the strip.
    s_bar = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(s_bar);
    lv_obj_set_size(s_bar, lv_disp_get_hor_res(nullptr) - 8, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x1A222A), 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_90, 0);
    lv_obj_set_style_pad_all(s_bar, 4, 0);
    lv_obj_set_style_radius(s_bar, 4, 0);
    lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);

    s_label = lv_label_create(s_bar);
    lv_label_set_long_mode(s_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_label, lv_disp_get_hor_res(nullptr) - 16);
    if (imeFont()) lv_obj_set_style_text_font(s_label, imeFont(), 0);
    lv_obj_set_style_text_color(s_label, lv_color_hex(0xE8EEF4), 0);
    lv_label_set_text(s_label, "");
}
}  // namespace

void imeCandHide(void)
{
    s_onPick = nullptr;
    s_user = nullptr;
    s_list.count = 0;
    if (s_bar) lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
}

bool imeCandVisible(void)
{
    return s_bar && !lv_obj_has_flag(s_bar, LV_OBJ_FLAG_HIDDEN) && s_list.count > 0;
}

void imeCandShow(const ImeCandList *list, const char *title, ImeCandPickFn onPick, void *user)
{
    (void)title;
    if (!list || list->count <= 0)
    {
        imeCandHide();
        return;
    }
    ensureBar();
    s_list = *list;
    if (s_list.count > kImeCandMax) s_list.count = kImeCandMax;
    if (s_list.selected < 0 || s_list.selected >= s_list.count) s_list.selected = 0;
    s_onPick = onPick;
    s_user = user;
    if (imeFont() && s_label) lv_obj_set_style_text_font(s_label, imeFont(), 0);
    refreshLabel();
    lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_bar);
}

bool imeCandHandleKey(int key)
{
    if (!imeCandVisible()) return false;

    if (key >= '1' && key <= '9')
    {
        const int idx = key - '1';
        if (idx < s_list.count && s_onPick)
        {
            ImeCandPickFn fn = s_onPick;
            void *u = s_user;
            imeCandHide();
            fn(idx, u);
        }
        return true;
    }
    if (key == ' ')
    {
        s_list.selected = (s_list.selected + 1) % s_list.count;
        refreshLabel();
        return true;
    }
    if (key == 0x0D || key == 0x0A)  // Enter: pick highlighted
    {
        if (s_onPick)
        {
            ImeCandPickFn fn = s_onPick;
            void *u = s_user;
            const int sel = s_list.selected;
            imeCandHide();
            fn(sel, u);
        }
        return true;
    }
    if (key == 27)  // Esc: dismiss, keep composition
    {
        imeCandHide();
        return true;
    }
    return false;
}

int imeCandCount(void) { return s_list.count; }
int imeCandSelected(void) { return s_list.selected; }
const char *imeCandItem(int index)
{
    if (index < 0 || index >= s_list.count) return "";
    return s_list.items[index];
}
