// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>

#include <cstddef>
#include <string>
#include <vector>

#include "kiwix_catalog.h"
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
    void openArticle(const ZimEntry &entry);
    void setStatus(const char *msg);
    void notify(const char *msg, int ms = 2400);  // status line + system toast
    void startDownload(const char *url);
    void showCatalogue(const char *dir);
    static void requestCatalogue(const char *dir);
    static void showCatalogueAsync(void *p);

    static void archiveCb(lv_event_t *e);
    static void downloadCb(lv_event_t *e);
    static void urlReadyCb(lv_event_t *e);
    static void urlCancelCb(lv_event_t *e);
    static void downloadTickCb(lv_timer_t *t);
    static void itemCb(lv_event_t *e);
    static void searchCb(lv_event_t *e);
    static void catalogueCb(lv_event_t *e);
    static void catalogItemCb(lv_event_t *e);
    static void catalogUpCb(lv_event_t *e);
    static void imeToggleCb(lv_event_t *e);
    static void catalogTickCb(lv_timer_t *t);
    static void fieldBindCb(lv_event_t *e);
    static void destroyAsync(void *p);

    static KiwixApp *s_active;
    static char barTitle_[32];

    lv_obj_t *root_ = nullptr;
    lv_obj_t *list_ = nullptr;
    lv_obj_t *status_ = nullptr;
    lv_obj_t *search_ = nullptr;
    lv_obj_t *url_ = nullptr;
    lv_timer_t *dlTimer_ = nullptr;
    lv_timer_t *catTimer_ = nullptr;
    size_t catRows_ = 0;   // rows are only built once the listing has arrived

    ZimArchive archive_;
    char archivePaths_[kMaxFiles][192] = {};
    size_t nArchives_ = 0;
    // The list rows are keyed by dirent, not by title text: two entries can
    // share a title prefix, and re-searching for the label would open the
    // first match instead of the row the user actually tapped.
    std::vector<ZimEntry> rowEntries_;
    // Catalogue rows, keyed the same way: by index into this vector.
    std::vector<KiwixCatalogItem> catItems_;
    std::string catDir_;
};
