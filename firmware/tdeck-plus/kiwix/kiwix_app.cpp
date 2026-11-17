// SPDX-License-Identifier: GPL-3.0-or-later
//
// Kiwix (ZIM) browser for the T-Deck Plus, built as an app page for wadamesh.
//
// Reuse is the point of this file: wadamesh already ships a Reader that renders
// local HTML from the SD card (scrolling, keyboard/trackball nav, back bar) and
// ReaderContent's HTML-to-text pass. So the only new UI here is the archive
// picker and title search; the article is written as HTML to the card and the
// Reader displays it. zim_reader.cpp does the format work.

#include "kiwix_app.h"

#include "AppPage.h"     // wadamesh app-page chrome: root, "< title" bar, async teardown
#include "i18n.h"        // TR()
#include "zim_reader.h"  // ZIM v5/v6 parser (this folder)

#include <Arduino.h>  // SD is pulled in from here on this core
#include <SD.h>
#include <lvgl.h>

#include <stdio.h>
#include <string.h>
#include <strings.h>  // strcasecmp

// Exposed by the wadamesh patch (patches/wadamesh/0001-kiwix-app.patch): the
// Reader's own page opener, so articles get the native reader, not a second one.
extern "C" void wadameshOpenReaderUrl(const char* url);

namespace
{
constexpr uint32_t kBg = 0x101418;
// wadamesh mounts the card at /sd; SD.open() paths are relative to that mount,
// but zim_reader uses stdio (fopen), so archive paths need the VFS prefix.
constexpr const char* kSdMount = "/sd";
constexpr size_t kMaxArticleBytes = 220 * 1024;
constexpr const char* kWorkDir = "/meshcomod/kiwix";
constexpr const char* kArticleUrl = "sd:/meshcomod/kiwix/article.htm";

bool endsWithZim(const char* name)
{
    const size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".zim") == 0;
}

bool isTextMime(const char* mime)
{
    if (!mime || !*mime) return false;
    return strstr(mime, "html") != nullptr || strstr(mime, "text/") != nullptr;
}

// ZIM articles link to other ZIM entries ("../A/Other"); the Reader resolves
// hrefs against the card, so those links would dead-end. Converting anchors to
// spans keeps the text and removes tap targets that cannot go anywhere yet.
void stripAnchors(std::vector<uint8_t>& html)
{
    std::string out;
    out.reserve(html.size());
    for (size_t i = 0; i < html.size();)
    {
        const char c = (char)html[i];
        if (c == '<' && i + 2 < html.size() && (html[i + 1] == 'a' || html[i + 1] == 'A') &&
            (html[i + 2] == ' ' || html[i + 2] == '\t' || html[i + 2] == '>' ||
             html[i + 2] == '\r' || html[i + 2] == '\n'))
        {
            out += "<span";
            i += 2;
        }
        else if (c == '<' && i + 3 < html.size() && html[i + 1] == '/' &&
                 (html[i + 2] == 'a' || html[i + 2] == 'A') && html[i + 3] == '>')
        {
            out += "</span>";
            i += 4;
        }
        else
        {
            out += c;
            ++i;
        }
    }
    html.assign(out.begin(), out.end());
}
}  // namespace

KiwixApp* KiwixApp::s_active = nullptr;
char KiwixApp::barTitle_[32];

void KiwixApp::launch(void)
{
    if (s_active) return;
    KiwixApp* app = new KiwixApp();
    s_active = app;
    if (!app->open())
    {
        s_active = nullptr;
        delete app;
    }
}

bool KiwixApp::isOpen(void) { return s_active != nullptr && s_active->root_ != nullptr; }

void KiwixApp::destroyAsync(void* p) { delete static_cast<KiwixApp*>(p); }

void KiwixApp::dismiss(void)
{
    if (!s_active) return;
    KiwixApp* app = s_active;
    s_active = nullptr;
    app->close();
    lv_async_call(destroyAsync, app);
}

KiwixApp::~KiwixApp() { archive_.close(); }

bool KiwixApp::open(void)
{
    const lv_coord_t w = lv_disp_get_hor_res(nullptr);
    const lv_coord_t h = appPageContentH();

    root_ = appPageCreateRoot(kBg);
    if (!root_) return false;

    // AppPage keeps the bar title by pointer while the page is up, so it must
    // own its storage (TR() may return a recycled ring buffer).
    snprintf(barTitle_, sizeof barTitle_, "%s", TR("Kiwix"));
    appPageBegin(barTitle_, &KiwixApp::dismiss);

    search_ = lv_textarea_create(root_);
    lv_textarea_set_one_line(search_, true);
    lv_textarea_set_placeholder_text(search_, TR("Search titles"));
    lv_obj_set_size(search_, w - 8, 34);
    lv_obj_set_pos(search_, 4, 2);
    lv_obj_add_event_cb(search_, searchCb, LV_EVENT_READY, nullptr);
    lv_obj_add_flag(search_, LV_OBJ_FLAG_HIDDEN);

    list_ = lv_list_create(root_);
    lv_obj_set_pos(list_, 0, 40);
    lv_obj_set_size(list_, w, h - 64);
    lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(list_, 0, LV_PART_MAIN);

    status_ = lv_label_create(root_);
    lv_obj_set_style_text_color(status_, lv_color_hex(0x9AA4AE), LV_PART_MAIN);
    lv_obj_set_width(status_, w - 8);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(status_, 4, h - 20);

    showArchives();
    return true;
}

void KiwixApp::close(void)
{
    appPageEnd(&KiwixApp::dismiss);      // hand the status bar back
    appPageDeleteRootAsync(root_);       // never a sync delete from a callback
    root_ = nullptr;
    list_ = nullptr;
    status_ = nullptr;
    search_ = nullptr;
    archive_.close();
    rowTitles_.clear();
    nArchives_ = 0;
}

void KiwixApp::setStatus(const char* msg) { if (status_) lv_label_set_text(status_, msg ? msg : ""); }

void KiwixApp::showArchives(void)
{
    archive_.close();
    rowTitles_.clear();
    nArchives_ = 0;
    lv_obj_clean(list_);
    if (search_) lv_obj_add_flag(search_, LV_OBJ_FLAG_HIDDEN);

    File dir = SD.open("/");
    if (!dir)
    {
        setStatus(TR("No SD card"));
        return;
    }
    File f;
    while (nArchives_ < kMaxFiles && (f = dir.openNextFile()))
    {
        const char* path = f.path();
        if (!f.isDirectory() && path && endsWithZim(path))
        {
            snprintf(archivePaths_[nArchives_], sizeof archivePaths_[0], "%s%s%s", kSdMount,
                     path[0] == '/' ? "" : "/", path);
            lv_obj_t* row = lv_list_add_btn(list_, nullptr, path);
            if (row) lv_obj_add_event_cb(row, archiveCb, LV_EVENT_CLICKED, (void*)(uintptr_t)(nArchives_ + 1));
            ++nArchives_;
        }
        f.close();
    }
    dir.close();

    setStatus(nArchives_ ? TR("Pick an archive") : TR("No .zim archive on the card"));
}

bool KiwixApp::openArchive(const char* path)
{
    char err[96];
    archive_.close();
    if (!archive_.open(path, err, sizeof err))
    {
        setStatus(err);
        return false;
    }
    if (search_) lv_textarea_set_text(search_, "");
    showTitles("");
    return true;
}

void KiwixApp::showTitles(const char* prefix)
{
    if (!archive_.isOpen())
    {
        showArchives();
        return;
    }
    rowTitles_.clear();
    lv_obj_clean(list_);
    if (search_) lv_obj_clear_flag(search_, LV_OBJ_FLAG_HIDDEN);

    if (!prefix || !*prefix)
    {
        // No query yet: offer the archive's own main page and a hint. Full
        // title lists are pointless on a Wikipedia-sized archive.
        ZimEntry main;
        if (archive_.mainPage(main) && !main.title.empty())
        {
            rowTitles_.push_back(main.title);
            lv_obj_t* row = lv_list_add_btn(list_, nullptr, main.title.c_str());
            if (row) lv_obj_add_event_cb(row, itemCb, LV_EVENT_CLICKED, (void*)(uintptr_t)rowTitles_.size());
        }
        setStatus(TR("Type a title prefix, then Enter"));
        return;
    }

    const std::vector<ZimEntry> hits = archive_.searchTitles(prefix, kMaxResults);
    for (const ZimEntry& entry : hits)
    {
        if (entry.title.empty()) continue;
        rowTitles_.push_back(entry.title);
        lv_obj_t* row = lv_list_add_btn(list_, nullptr, entry.title.c_str());
        if (row) lv_obj_add_event_cb(row, itemCb, LV_EVENT_CLICKED, (void*)(uintptr_t)rowTitles_.size());
    }
    char msg[48];
    snprintf(msg, sizeof msg, TR("%u match(es)"), (unsigned)rowTitles_.size());
    setStatus(msg);
}

void KiwixApp::openArticle(const char* title)
{
    char err[96];
    ZimEntry entry;
    if (!archive_.findByTitle(title, entry) || !archive_.resolve(entry, entry))
    {
        setStatus(TR("Article not found"));
        return;
    }
    if (!isTextMime(entry.mimeType))
    {
        setStatus(TR("Not a text article"));
        return;
    }

    std::vector<uint8_t> html;
    if (!archive_.readBlob(entry, html, err, sizeof err))
    {
        setStatus(err);
        return;
    }
    if (html.size() > kMaxArticleBytes)
    {
        setStatus(TR("Article too large"));
        return;
    }
    stripAnchors(html);

    SD.mkdir("/meshcomod");
    SD.mkdir(kWorkDir);
    File out = SD.open("/meshcomod/kiwix/article.htm", FILE_WRITE);
    if (!out)
    {
        setStatus(TR("Cannot write to the card"));
        return;
    }
    out.write(html.data(), html.size());
    out.close();

    // Leave this page, then hand the file to the firmware's Reader.
    dismiss();
    wadameshOpenReaderUrl(kArticleUrl);
}

void KiwixApp::archiveCb(lv_event_t* e)
{
    if (!s_active) return;
    const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx == 0 || idx > s_active->nArchives_) return;
    s_active->openArchive(s_active->archivePaths_[idx - 1]);
}

void KiwixApp::itemCb(lv_event_t* e)
{
    if (!s_active) return;
    const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx == 0 || idx > s_active->rowTitles_.size()) return;
    s_active->openArticle(s_active->rowTitles_[idx - 1].c_str());
}

void KiwixApp::searchCb(lv_event_t* e)
{
    (void)e;
    if (!s_active || !s_active->search_) return;
    const char* text = lv_textarea_get_text(s_active->search_);
    s_active->showTitles(text ? text : "");
}
