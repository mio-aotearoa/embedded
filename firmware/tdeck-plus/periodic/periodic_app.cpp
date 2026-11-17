// SPDX-License-Identifier: GPL-3.0-or-later
//
// Periodic table for the T-Deck Plus.
//
// Adapted from NeoCalculator's PeriodicTableApp (El-EnderJ, GPL-3.0-or-later):
// the 18x10 layout, category colours and element data come from there. This
// version draws the grid in a single LVGL object (118 buttons would be heavier
// than the 320x240 screen justifies) and uses the firmware's AppPage chrome.
#include "periodic_app.h"

#include "AppPage.h"
#include "i18n.h"
#include "ChemDatabase.h"  // third_party/neocalculator (pinned submodule), GPL-3.0-or-later

#include <lvgl.h>

#include <stdio.h>
#include <string.h>

namespace
{
constexpr uint32_t kBg = 0x101418;
constexpr int kCols = 18;
constexpr int kRows = 10;
constexpr int kCellW = 17;
constexpr int kCellH = 19;
constexpr int kGridW = kCols * kCellW;
constexpr int kGridX = (320 - kGridW) / 2;

// Same palette as the upstream app.
constexpr uint32_t kCategoryColors[] = {
    0x5BC0EB,  // NonMetal
    0xA855F7,  // NobleGas
    0xFF6B6B,  // AlkaliMetal
    0xFFA07A,  // AlkalineEarth
    0x20B2AA,  // Metalloid
    0x98FB98,  // Halogen
    0xFFD93D,  // TransitionMetal
    0x6BCB77,  // PostTransitionMetal
    0xFF8FA3,  // Lanthanide
    0xC084FC,  // Actinide
    0x888888,  // Unknown
};

constexpr const char *kCategoryNames[] = {
    "Non-metal", "Noble gas", "Alkali metal", "Alkaline earth", "Metalloid",
    "Halogen", "Transition metal", "Post-transition", "Lanthanide", "Actinide",
    "Unknown",
};

uint32_t categoryColor(chem::ChemCategory category)
{
    const int i = (int)category;
    if (i < 0 || i >= (int)(sizeof kCategoryColors / sizeof kCategoryColors[0]))
    {
        return 0x888888;
    }
    return kCategoryColors[i];
}

const char *categoryName(chem::ChemCategory category)
{
    const int i = (int)category;
    if (i < 0 || i >= (int)(sizeof kCategoryNames / sizeof kCategoryNames[0]))
    {
        return "Unknown";
    }
    return kCategoryNames[i];
}
}  // namespace

PeriodicApp *PeriodicApp::s_active = nullptr;
char PeriodicApp::barTitle_[32];

void PeriodicApp::launch(void)
{
    if (s_active) return;
    PeriodicApp *app = new PeriodicApp();
    s_active = app;
    if (!app->open())
    {
        s_active = nullptr;
        delete app;
    }
}

bool PeriodicApp::isOpen(void) { return s_active != nullptr && s_active->root_ != nullptr; }

void PeriodicApp::destroyAsync(void *p) { delete static_cast<PeriodicApp *>(p); }

void PeriodicApp::dismiss(void)
{
    if (!s_active) return;
    PeriodicApp *app = s_active;
    s_active = nullptr;
    app->close();
    lv_async_call(destroyAsync, app);
}

PeriodicApp::~PeriodicApp() {}

bool PeriodicApp::open(void)
{
    const lv_coord_t w = lv_disp_get_hor_res(nullptr);
    const lv_coord_t h = appPageContentH();

    root_ = appPageCreateRoot(kBg);
    if (!root_) return false;

    snprintf(barTitle_, sizeof barTitle_, "%s", TR("Periodic Table"));
    appPageBegin(barTitle_, &PeriodicApp::dismiss);

    // The tall "< Periodic Table" bar overlaps the top of the root, and ten
    // full-height rows no longer fit below it: squeeze the rows into the room
    // that is left and keep the detail line on screen.
    const lv_coord_t bar = appPageBarCover();
    cellH_ = (lv_coord_t)((h - bar - 34) / kRows);
    if (cellH_ > kCellH) cellH_ = kCellH;
    if (cellH_ < 8) cellH_ = 8;
    const lv_coord_t gridH = (lv_coord_t)(kRows * cellH_);
    grid_ = lv_obj_create(root_);
    lv_obj_remove_style_all(grid_);
    lv_obj_set_size(grid_, w, gridH);
    lv_obj_set_pos(grid_, 0, bar);
    lv_obj_add_flag(grid_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(grid_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(grid_, drawCb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(grid_, pressedCb, LV_EVENT_PRESSED, nullptr);

    detail_ = lv_label_create(root_);
    lv_obj_set_width(detail_, w - 8);
    lv_obj_set_pos(detail_, 4, bar + gridH + 1);
    lv_obj_set_style_text_font(detail_, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(detail_, lv_color_hex(0xC8D2DC), LV_PART_MAIN);
    lv_label_set_long_mode(detail_, LV_LABEL_LONG_WRAP);
    updateDetail();

    return true;
}

void PeriodicApp::close(void)
{
    appPageEnd(&PeriodicApp::dismiss);
    appPageDeleteRootAsync(root_);
    root_ = nullptr;
    grid_ = nullptr;
    detail_ = nullptr;
}

void PeriodicApp::select(int z)
{
    if (z < 1 || z > 118) return;
    selected_ = z;
    updateDetail();
    if (grid_) lv_obj_invalidate(grid_);
}

void PeriodicApp::updateDetail(void)
{
    if (!detail_) return;
    const chem::ElementData &el = chem::ELEMENTS[selected_ - 1];
    char first[96];
    char second[128];
    snprintf(first, sizeof first, "#%u  %s  %s \xE2\x80\x94 %.3f g/mol",
                 (unsigned)el.atomicNumber, el.symbol, el.name, (double)el.mass);
    if (el.electronegativity > 0.0f)
    {
        snprintf(second, sizeof second, "%s \xC2\xB7 EN %.2f \xC2\xB7 %s",
                 categoryName(el.category), (double)el.electronegativity, el.electronConfig);
    }
    else
    {
        snprintf(second, sizeof second, "%s \xC2\xB7 EN n/a \xC2\xB7 %s",
                 categoryName(el.category), el.electronConfig);
    }
    char text[256];
    snprintf(text, sizeof text, "%s\n%s", first, second);
    lv_label_set_text(detail_, text);
}

void PeriodicApp::drawCb(lv_event_t *e)
{
    // The root is deleted asynchronously, so a frame can still be drawn after
    // dismiss() cleared s_active. Never dereference it in that window.
    PeriodicApp *app = s_active;
    if (!app) return;

    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *draw = lv_event_get_draw_ctx(e);
    if (!draw || !draw->clip_area) return;

    lv_area_t origin;
    lv_obj_get_coords(obj, &origin);
    const int ox = origin.x1 + kGridX;
    const int oy = origin.y1;
    const lv_area_t *clip = draw->clip_area;

    for (int row = 0; row < kRows; row++)
    {
        for (int col = 0; col < kCols; col++)
        {
            const int z = chem::TABLE_LAYOUT[row][col];
            if (z < 1) continue;

            lv_area_t cell = {ox + col * kCellW, oy + row * app->cellH_,
                              ox + col * kCellW + kCellW - 1,
                              oy + row * app->cellH_ + app->cellH_ - 1};
            if (cell.x2 < clip->x1 || cell.x1 > clip->x2 ||
                cell.y2 < clip->y1 || cell.y1 > clip->y2)
            {
                continue;
            }

            const chem::ElementData &el = chem::ELEMENTS[z - 1];
            const bool chosen = (z == app->selected_);

            lv_draw_rect_dsc_t rect;
            lv_draw_rect_dsc_init(&rect);
            rect.radius = 1;
            rect.bg_opa = LV_OPA_COVER;
            rect.bg_color = lv_color_hex(categoryColor(el.category));
            rect.border_width = chosen ? 2 : 1;
            rect.border_opa = LV_OPA_COVER;
            rect.border_color = chosen ? lv_color_white() : lv_color_hex(0x101418);
            lv_draw_rect(draw, &rect, &cell);

            lv_draw_label_dsc_t label;
            lv_draw_label_dsc_init(&label);
            label.font = &lv_font_montserrat_12;
            label.color = lv_color_black();
            label.align = LV_TEXT_ALIGN_CENTER;
            lv_area_t text = cell;
            text.y1 += 3;
            lv_draw_label(draw, &label, &text, el.symbol, nullptr);
        }
    }
}

void PeriodicApp::pressedCb(lv_event_t *e)
{
    if (!s_active) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_obj_t *obj = lv_event_get_target(e);
    lv_area_t origin;
    lv_obj_get_coords(obj, &origin);

    const int x = p.x - origin.x1 - kGridX;
    const int y = p.y - origin.y1;
    if (x < 0 || y < 0) return;
    const int col = x / kCellW;
    const int row = y / s_active->cellH_;
    if (col >= kCols || row >= kRows) return;

    const int z = chem::TABLE_LAYOUT[row][col];
    if (z >= 1) s_active->select(z);
}
