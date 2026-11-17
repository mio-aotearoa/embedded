// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>

#include <cstddef>
#include <string>
#include <vector>

#include "zim_reader.h"

// Kiwix (ZIM archive) browser, as a wadamesh app page.
//
// This module owns only the picker: list the .zim archives on the card, search
// their titles, then write the selected article's HTML to the card and hand
// that file to wadamesh's own Reader (via wadameshOpenReaderUrl), which already
// renders local HTML with scrolling, keyboard navigation and a back bar.
// Parsing lives in zim_reader.{h,cpp}; nothing here re-implements a renderer.
class KiwixApp
{
  public:
    static void launch(void);   // open (no-op if already open)
    static bool isOpen(void);   // UITask: page ownership queries
    static void dismiss(void);  // appPageBegin back hook; async teardown

  private:
    static constexpr size_t kMaxFiles = 24;
    static constexpr size_t kMaxResults = 60;

    ~KiwixApp();
    bool open(void);
    void close(void);
    void showArchives(void);
    void showTitles(const char *prefix);
    bool openArchive(const char *path);
    void openArticle(const char *title);
    void setStatus(const char *msg);

    static void archiveCb(lv_event_t *e);
    static void itemCb(lv_event_t *e);
    static void searchCb(lv_event_t *e);
    static void destroyAsync(void *p);

    static KiwixApp *s_active;
    static char barTitle_[32];

    lv_obj_t *root_ = nullptr;
    lv_obj_t *list_ = nullptr;
    lv_obj_t *status_ = nullptr;
    lv_obj_t *search_ = nullptr;

    ZimArchive archive_;
    char archivePaths_[kMaxFiles][192] = {};
    size_t nArchives_ = 0;
    std::vector<std::string> rowTitles_;
};
