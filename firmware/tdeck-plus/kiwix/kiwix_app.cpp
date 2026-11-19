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
#include "http_download.h"  // Wi-Fi -> SD downloader shared with the ESP-AI page
#include "i18n.h"        // TR()
#include "ime_ja.h"     // romaji -> kana in wadamesh's typing path
#include "url_util.h"   // bare host -> https://
#include "zim_reader.h"  // ZIM v5/v6 parser (this folder)
#include "src/extra/libs/sjpg/tjpgd.h"  // wadamesh's JPEG decoder (same copy UITask uses)

#include <Arduino.h>  // SD is pulled in from here on this core
#include <SD.h>
#include <ctype.h>
#include <esp_heap_caps.h>
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
constexpr const char* kDownloadDir = "/kiwix";
// A small, stable archive from the Kiwix mirror, offered as the starting point
// for the URL field; any .zim URL can be typed over it.
constexpr const char* kDefaultZimUrl =
    "https://download.kiwix.org/zim/wikipedia/speedtest_en_blob-mini_2024-05.zim";

// ---- article images -------------------------------------------------------
// ZIM HTML points at images with ZIM paths ("../I/foo.jpg"). The Reader is a
// text renderer, so the first image is shown here, decoded with the tjpgd copy
// wadamesh already builds, before handing the article to the Reader.
static lv_obj_t* s_imgOverlay = nullptr;
static lv_img_dsc_t s_imgDsc;
static uint8_t* s_imgBuf = nullptr;

struct JpgIo { const uint8_t* data; size_t len; size_t pos; uint16_t* out; int ow; int oh; };

extern "C" {
static size_t jpgIn(JDEC* jd, uint8_t* buf, size_t n)
{
    JpgIo* io = (JpgIo*)jd->device;
    size_t avail = io->len - io->pos;
    if (n > avail) n = avail;
    if (buf) memcpy(buf, io->data + io->pos, n);
    io->pos += n;
    return n;
}
static int jpgOut(JDEC* jd, void* bitmap, JRECT* r)
{
    JpgIo* io = (JpgIo*)jd->device;
    const uint8_t* s = (const uint8_t*)bitmap;
    for (int y = (int)r->top; y <= (int)r->bottom; ++y)
    {
        uint16_t* d = (y < io->oh) ? io->out + (size_t)y * io->ow + (int)r->left : nullptr;
        for (int x = (int)r->left; x <= (int)r->right; ++x)
        {
            if (d && x < io->ow)
            {
                lv_color_t c = lv_color_make(s[0], s[1], s[2]);
                *d++ = c.full;
            }
            s += 3;
        }
    }
    return 1;
}
}  // extern "C"

static void imageTapCb(lv_event_t*)
{
    if (s_imgOverlay)
    {
        lv_obj_del(s_imgOverlay);
        s_imgOverlay = nullptr;
    }
    if (s_imgBuf)
    {
        heap_caps_free(s_imgBuf);
        s_imgBuf = nullptr;
    }
    KiwixApp::dismiss();
    wadameshOpenReaderUrl(kArticleUrl);
}

static bool showArticleImage(ZimArchive& ar, const std::string& src, const ZimEntry& article)
{
    std::string path;
    if (src.compare(0, 5, "data:") == 0 || src.compare(0, 4, "http") == 0) return false;
    size_t cut = src.find_first_of("?#");
    path = src.substr(0, cut);
    while (path.compare(0, 3, "../") == 0) path.erase(0, 3);
    if (path.compare(0, 2, "./") == 0) path.erase(0, 2);
    if (!(path.size() > 2 && path[1] == '/'))
    {
        std::string base(1, article.ns);
        base += '/'; base += article.path;
        const size_t slash = base.rfind('/');
        base.resize(slash == std::string::npos ? 2 : slash + 1);
        path = base + path;
    }
    if (path.size() < 4) return false;
    std::string lower = path;
    for (char& c : lower) c = (char)tolower((unsigned char)c);
    if (lower.size() < 5 || (lower.rfind(".jpg") != lower.size() - 4 &&
                             lower.rfind(".jpeg") != lower.size() - 5)) return false;

    ZimEntry img;
    if (!ar.findByPath(path.c_str(), img) || !ar.resolve(img, img)) return false;
    std::vector<uint8_t> jpeg;
    char err[96];
    if (!ar.readBlob(img, jpeg, err, sizeof err) || jpeg.size() < 4 ||
        jpeg[0] != 0xFF || jpeg[1] != 0xD8) return false;

    static uint8_t* pool = nullptr;
    if (!pool)
    {
        pool = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
        if (!pool) pool = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_8BIT);
    }
    if (!pool) return false;
    JpgIo io{jpeg.data(), jpeg.size(), 0, nullptr, 0, 0};
    JDEC jd;
    if (jd_prepare(&jd, jpgIn, pool, 4096, &io) != JDR_OK) return false;
    int scale = 0;
    const int maxDim = 300;
    while (scale < 3 && (((int)jd.width >> scale) > maxDim || ((int)jd.height >> scale) > maxDim)) ++scale;
    const int ow = (int)jd.width >> scale, oh = (int)jd.height >> scale;
    if (ow <= 0 || oh <= 0 || ow > maxDim || oh > maxDim) return false;
    s_imgBuf = (uint8_t*)heap_caps_malloc((size_t)ow * oh * 2, MALLOC_CAP_SPIRAM);
    if (!s_imgBuf) s_imgBuf = (uint8_t*)heap_caps_malloc((size_t)ow * oh * 2, MALLOC_CAP_8BIT);
    if (!s_imgBuf) return false;
    io.out = (uint16_t*)s_imgBuf; io.ow = ow; io.oh = oh;
    if (jd_decomp(&jd, jpgOut, (uint8_t)scale) != JDR_OK)
    {
        heap_caps_free(s_imgBuf); s_imgBuf = nullptr;
        return false;
    }

    memset(&s_imgDsc, 0, sizeof s_imgDsc);
    s_imgDsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_imgDsc.header.w = ow; s_imgDsc.header.h = oh;
    s_imgDsc.data_size = (uint32_t)ow * oh * 2;
    s_imgDsc.data = s_imgBuf;

    s_imgOverlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_imgOverlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_imgOverlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_imgOverlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_imgOverlay, 0, 0);
    lv_obj_clear_flag(s_imgOverlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_imgOverlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_imgOverlay, imageTapCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* im = lv_img_create(s_imgOverlay);
    lv_img_set_src(im, &s_imgDsc);
    lv_obj_center(im);
    lv_obj_t* hint = lv_label_create(s_imgOverlay);
    lv_label_set_text(hint, "Tap for article");
    lv_obj_set_style_text_color(hint, lv_color_hex(0xE0E0E0), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);
    return true;
}

static bool firstImageSrc(const std::vector<uint8_t>& html, std::string& out)
{
    const std::string text(html.begin(), html.end());
    size_t pos = 0;
    while ((pos = text.find("<img", pos)) != std::string::npos)
    {
        size_t src = text.find("src", pos);
        size_t end = text.find('>', pos);
        if (src == std::string::npos || (end != std::string::npos && src > end)) { pos += 4; continue; }
        size_t q = text.find_first_of("\"'", src);
        if (q == std::string::npos) { pos += 4; continue; }
        size_t qe = text.find(text[q], q + 1);
        if (qe == std::string::npos) { pos += 4; continue; }
        out = text.substr(q + 1, qe - q - 1);
        return !out.empty();
    }
    return false;
}

// ".../wikipedia_en_all_nopic.zim?x=1" -> "wikipedia_en_all_nopic.zim"; the
// name is used as a file name, so anything unusual is rejected.
std::string zimNameFromUrl(const char* url)
{
    if (!url) return {};
    std::string s(url);
    const size_t cut = s.find_first_of("?#");
    if (cut != std::string::npos) s.resize(cut);
    const size_t slash = s.rfind('/');
    std::string name = slash == std::string::npos ? s : s.substr(slash + 1);
    if (name.size() < 5) return {};
    for (char c : name)
    {
        const unsigned char u = (unsigned char)c;
        if (!(isalnum(u) || c == '.' || c == '-' || c == '_')) return {};
    }
    std::string lower = name;
    for (char& c : lower) c = (char)tolower((unsigned char)c);
    if (lower.rfind(".zim") != lower.size() - 4) return {};
    return name;
}

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

    // The tall "< Kiwix" bar overlaps the top of the root; start below it.
    const lv_coord_t bar = appPageBarCover();

    search_ = lv_textarea_create(root_);
    lv_textarea_set_one_line(search_, true);
    lv_textarea_set_placeholder_text(search_, TR("Search titles"));
    lv_obj_set_size(search_, w - 8, 34);
    lv_obj_set_pos(search_, 4, bar + 2);
    lv_obj_add_event_cb(search_, searchCb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(search_, imeToggleCb, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_add_event_cb(search_, fieldBindCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(search_, LV_OBJ_FLAG_HIDDEN);

    // URL field for the Wi-Fi download; only shown after tapping the download
    // row, so the archive list keeps the screen otherwise.
    url_ = lv_textarea_create(root_);
    lv_textarea_set_one_line(url_, true);
    lv_textarea_set_placeholder_text(url_, TR("https://\xE2\x80\xA6.zim"));
    lv_obj_set_size(url_, w - 8, 34);
    lv_obj_set_pos(url_, 4, bar + 40);
    lv_obj_add_event_cb(url_, urlReadyCb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(url_, imeToggleCb, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_add_event_cb(url_, fieldBindCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(url_, urlCancelCb, LV_EVENT_CANCEL, nullptr);
    lv_obj_add_flag(url_, LV_OBJ_FLAG_HIDDEN);

    list_ = lv_list_create(root_);
    lv_obj_set_pos(list_, 0, bar + 40);
    lv_obj_set_size(list_, w, h - bar - 64);
    lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(list_, 0, LV_PART_MAIN);

    status_ = lv_label_create(root_);
    lv_obj_set_style_text_color(status_, lv_color_hex(0x9AA4AE), LV_PART_MAIN);
    lv_obj_set_width(status_, w - 8);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(status_, 4, h - 20);

    showArchives();
    appPageBindTextField(nullptr);      // the archive list needs no typing
    // Reserve the catalogue worker stack now, before list/widgets fragment the
    // last contiguous internal block (otherwise Catalogue reports OOM).
    (void)kiwixCatalogWarmup();
    return true;
}

void KiwixApp::close(void)
{
    httpDownloadCancel();
    if (dlTimer_)
    {
        lv_timer_del(dlTimer_);
        dlTimer_ = nullptr;
    }
    if (catTimer_)
    {
        lv_timer_del(catTimer_);
        catTimer_ = nullptr;
    }
    appPageBindTextField(nullptr);       // a field of ours is about to go away
    appPageEnd(&KiwixApp::dismiss);      // hand the status bar back
    appPageDeleteRootAsync(root_);       // never a sync delete from a callback
    root_ = nullptr;
    list_ = nullptr;
    status_ = nullptr;
    search_ = nullptr;
    archive_.close();
    rowEntries_.clear();
    nArchives_ = 0;

    // The article-image overlay lives on lv_layer_top, outside root_, so it
    // would outlive the page if the app is torn down while it is showing.
    if (s_imgOverlay)
    {
        lv_obj_del(s_imgOverlay);
        s_imgOverlay = nullptr;
    }
    if (s_imgBuf)
    {
        heap_caps_free(s_imgBuf);
        s_imgBuf = nullptr;
    }
}

void KiwixApp::setStatus(const char* msg) { if (status_) lv_label_set_text(status_, msg ? msg : ""); }

void KiwixApp::showArchives(void)
{
    appPageBindTextField(nullptr);   // the search field is hidden again
    archive_.close();
    rowEntries_.clear();
    nArchives_ = 0;
    lv_obj_clean(list_);
    if (search_) lv_obj_add_flag(search_, LV_OBJ_FLAG_HIDDEN);
    if (url_) lv_obj_add_flag(url_, LV_OBJ_FLAG_HIDDEN);
    if (list_) lv_obj_clear_flag(list_, LV_OBJ_FLAG_HIDDEN);

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

    // Browse what the mirror offers before asking for a URL.
    lv_obj_t* cat = lv_list_add_btn(list_, nullptr, TR("Catalogue: browse Kiwix"));
    if (cat) lv_obj_add_event_cb(cat, catalogueCb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* dl = lv_list_add_btn(list_, nullptr, TR("Download .zim over Wi-Fi"));
    if (dl) lv_obj_add_event_cb(dl, downloadCb, LV_EVENT_CLICKED, nullptr);

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
    appPageBindTextField(search_);      // hardware keys type into the search
    return true;
}

void KiwixApp::showTitles(const char* prefix)
{
    if (!archive_.isOpen())
    {
        showArchives();
        return;
    }
    rowEntries_.clear();
    lv_obj_clean(list_);
    if (search_) lv_obj_clear_flag(search_, LV_OBJ_FLAG_HIDDEN);

    if (!prefix || !*prefix)
    {
        // No query yet: offer the archive's own main page and a hint. Full
        // title lists are pointless on a Wikipedia-sized archive.
        ZimEntry main;
        if (archive_.mainPage(main) && !main.title.empty())
        {
            rowEntries_.push_back(main);
            lv_obj_t* row = lv_list_add_btn(list_, nullptr, main.title.c_str());
            if (row) lv_obj_add_event_cb(row, itemCb, LV_EVENT_CLICKED, (void*)(uintptr_t)rowEntries_.size());
        }
        setStatus(TR("Type a title prefix, then Enter"));
        return;
    }

    const std::vector<ZimEntry> hits = archive_.searchTitles(prefix, kMaxResults);
    for (ZimEntry entry : hits)
    {
        ZimEntry resolved;
        if (!archive_.resolve(entry, resolved) || resolved.title.empty()) continue;
        rowEntries_.push_back(resolved);
        lv_obj_t* row = lv_list_add_btn(list_, nullptr, resolved.title.c_str());
        if (row) lv_obj_add_event_cb(row, itemCb, LV_EVENT_CLICKED, (void*)(uintptr_t)rowEntries_.size());
    }
    char msg[48];
    snprintf(msg, sizeof msg, TR("%u match(es)"), (unsigned)rowEntries_.size());
    setStatus(msg);
}

void KiwixApp::openArticle(const ZimEntry& entry)
{
    char err[96];
    ZimEntry article;
    if (!archive_.resolve(entry, article))
    {
        setStatus(TR("Article not found"));
        return;
    }
    if (!isTextMime(article.mimeType))
    {
        setStatus(TR("Not a text article"));
        return;
    }

    std::vector<uint8_t> html;
    if (!archive_.readBlob(article, html, err, sizeof err))
    {
        setStatus(err);
        return;
    }
    if (html.size() > kMaxArticleBytes)
    {
        setStatus(TR("Article too large"));
        return;
    }
    std::string imageSrc;
    const bool hasImage = firstImageSrc(html, imageSrc);
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

    // Show the article's first image (if any) before the Reader takes over;
    // a tap continues to the article. No image: go straight to the Reader.
    if (hasImage && showArticleImage(archive_, imageSrc, article)) return;

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
    if (idx == 0 || idx > s_active->rowEntries_.size()) return;
    s_active->openArticle(s_active->rowEntries_[idx - 1]);
}

void KiwixApp::searchCb(lv_event_t* e)
{
    (void)e;
    if (!s_active || !s_active->search_) return;
    const char* text = lv_textarea_get_text(s_active->search_);
    s_active->showTitles(text ? text : "");
}

void KiwixApp::fieldBindCb(lv_event_t* e)
{
    // Tapping either field hands the physical keyboard to it (they swap places).
    appPageBindTextField(lv_event_get_target(e));
}

void KiwixApp::imeToggleCb(lv_event_t* e)
{
    (void)e;
    imeJaToggle();
    if (s_active)
        s_active->setStatus(imeJaActive() ? TR("Japanese: romaji, Space = kana")
                                          : TR("Type a title prefix, then Enter"));
}

// Defer catalogue navigation out of list-button event handlers: wiping the
// list deletes the button mid-CLICKED and LVGL re-enters / drops the fetch
// ("already loading" + empty list). Same pattern as wadamesh's trust rebuild.
char s_catNavDir[192];
bool s_catNavQueued = false;

// lv_list_add_btn puts the caption in LV_LABEL_LONG_SCROLL_CIRCULAR, so long
// .zim names marquee forever. Switch the caption to wrap and let the row grow.
void catalogWrapRow(lv_obj_t* row)
{
    if (!row) return;
    const uint32_t n = lv_obj_get_child_cnt(row);
    if (!n) return;
    lv_obj_t* lbl = lv_obj_get_child(row, n - 1);  // caption after optional icon
    if (!lbl) return;
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(lbl, 1);
    lv_obj_set_height(lbl, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(row, 4, LV_PART_MAIN);
    lv_obj_set_height(row, LV_SIZE_CONTENT);
}

// One row per folder/archive of the mirror's index, browsed a level at a time.
// The listing is fetched on a worker task; this only starts it and clears the
// list, and catalogTickCb fills the rows when the answer is in.
void KiwixApp::showCatalogue(const char* dir)
{
    catDir_ = dir ? dir : "";
    if (search_) lv_obj_add_flag(search_, LV_OBJ_FLAG_HIDDEN);
    if (url_) lv_obj_add_flag(url_, LV_OBJ_FLAG_HIDDEN);
    if (list_) lv_obj_clear_flag(list_, LV_OBJ_FLAG_HIDDEN);
    appPageBindTextField(nullptr);

    if (!kiwixCatalogStart(catDir_.c_str()))
    {
        const KiwixCatalogState& st = kiwixCatalogState();
        setStatus(st.err[0] ? st.err : TR("Could not start the catalogue"));
        return;
    }

    // Only wipe after the worker accepted the path (or queued it).
    catItems_.clear();
    catRows_ = 0;
    if (list_) lv_obj_clean(list_);
    setStatus(TR("Loading the catalogue\xE2\x80\xA6"));
    if (!catTimer_) catTimer_ = lv_timer_create(catalogTickCb, 200, nullptr);
}

void KiwixApp::catalogTickCb(lv_timer_t* t)
{
    (void)t;
    KiwixApp* app = s_active;
    if (!app) return;
    const KiwixCatalogState& st = kiwixCatalogState();
    if (st.running || !st.done) return;

    const bool ok = st.ok;
    char err[96];
    snprintf(err, sizeof err, "%s", st.err);
    const std::vector<KiwixCatalogItem>* items = st.items;
    kiwixCatalogClearDone();

    if (app->catTimer_)
    {
        lv_timer_del(app->catTimer_);
        app->catTimer_ = nullptr;
    }

    if (!ok)
    {
        app->setStatus(err);
        lv_obj_t* back = lv_list_add_btn(app->list_, LV_SYMBOL_LEFT, TR("Back to archives"));
        if (back)
        {
            catalogWrapRow(back);
            lv_obj_add_event_cb(back, catalogUpCb, LV_EVENT_CLICKED, nullptr);
        }
        return;
    }

    if (items) app->catItems_ = *items;
    app->catRows_ = app->catItems_.size();

    if (!app->catDir_.empty())
    {
        lv_obj_t* up = lv_list_add_btn(app->list_, LV_SYMBOL_UP, "..");
        if (up)
        {
            catalogWrapRow(up);
            lv_obj_add_event_cb(up, catalogUpCb, LV_EVENT_CLICKED, nullptr);
        }
    }
    for (size_t i = 0; i < app->catItems_.size(); ++i)
    {
        const KiwixCatalogItem& it = app->catItems_[i];
        char label[140];
        if (it.isDir) snprintf(label, sizeof label, "%s", it.name);
        else if (it.size[0]) snprintf(label, sizeof label, "%s  (%s)", it.name, it.size);
        else snprintf(label, sizeof label, "%s", it.name);
        lv_obj_t* row = lv_list_add_btn(app->list_, it.isDir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_DOWNLOAD, label);
        if (row)
        {
            // lv_list defaults to circular scroll on long names — wrap instead.
            catalogWrapRow(row);
            lv_obj_add_event_cb(row, catalogItemCb, LV_EVENT_CLICKED, (void*)(uintptr_t)(i + 1));
        }
    }
    char msg[48];
    snprintf(msg, sizeof msg, TR("%u entries"), (unsigned)app->catRows_);
    app->setStatus(msg);
}

void KiwixApp::showCatalogueAsync(void*)
{
    s_catNavQueued = false;
    if (s_active) s_active->showCatalogue(s_catNavDir);
}

void KiwixApp::requestCatalogue(const char* dir)
{
    snprintf(s_catNavDir, sizeof s_catNavDir, "%s", dir ? dir : "");
    if (s_catNavQueued) return;
    s_catNavQueued = true;
    if (lv_async_call(showCatalogueAsync, nullptr) != LV_RES_OK)
    {
        s_catNavQueued = false;
        if (s_active) s_active->showCatalogue(s_catNavDir);
    }
}

void KiwixApp::catalogueCb(lv_event_t* e)
{
    (void)e;
    requestCatalogue("");
}

void KiwixApp::catalogUpCb(lv_event_t* e)
{
    (void)e;
    if (!s_active) return;
    if (s_active->catDir_.empty())
    {
        s_active->showArchives();
        return;
    }
    std::string up = s_active->catDir_;
    while (!up.empty() && up.back() == '/') up.pop_back();
    const size_t slash = up.rfind('/');
    up.resize(slash == std::string::npos ? 0 : slash);
    requestCatalogue(up.c_str());
}

void KiwixApp::catalogItemCb(lv_event_t* e)
{
    if (!s_active) return;
    const size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx == 0 || idx > s_active->catItems_.size()) return;
    const KiwixCatalogItem& it = s_active->catItems_[idx - 1];

    if (it.isDir)
    {
        std::string child = s_active->catDir_;
        if (!child.empty()) child += '/';
        child += it.name;
        // Copy the name before any deferred wipe of catItems_.
        requestCatalogue(child.c_str());
        return;
    }

    char url[320];
    snprintf(url, sizeof url, "https://download.kiwix.org/zim/%s%s%s",
             s_active->catDir_.empty() ? "" : s_active->catDir_.c_str(),
             s_active->catDir_.empty() ? "" : "/", it.name);
    s_active->startDownload(url);
}

void KiwixApp::downloadCb(lv_event_t* e)
{
    (void)e;
    if (!s_active || !s_active->url_) return;
    lv_obj_clear_flag(s_active->url_, LV_OBJ_FLAG_HIDDEN);
    if (s_active->list_) lv_obj_add_flag(s_active->list_, LV_OBJ_FLAG_HIDDEN);
    if (!lv_textarea_get_text(s_active->url_)[0])
        lv_textarea_set_text(s_active->url_, kDefaultZimUrl);
    lv_obj_add_state(s_active->url_, LV_STATE_FOCUSED);
    appPageBindTextField(s_active->url_);   // hardware keys type into the URL
    s_active->setStatus(TR("Edit the URL, then press Enter"));
}

void KiwixApp::urlReadyCb(lv_event_t* e)
{
    (void)e;
    if (!s_active || !s_active->url_) return;
    char full[256];
    if (!urlComplete(lv_textarea_get_text(s_active->url_), full, sizeof full))
    {
        s_active->setStatus(TR("That URL is too long"));
        return;
    }
    if (strncmp(full, "http://", 7) != 0 && strncmp(full, "https://", 8) != 0)
    {
        s_active->setStatus(TR("Only http:// and https:// URLs are supported"));
        return;
    }
    lv_textarea_set_text(s_active->url_, full);
    s_active->startDownload(full);
}

void KiwixApp::urlCancelCb(lv_event_t* e)
{
    (void)e;
    if (!s_active) return;
    if (s_active->url_) lv_obj_add_flag(s_active->url_, LV_OBJ_FLAG_HIDDEN);
    if (s_active->list_) lv_obj_clear_flag(s_active->list_, LV_OBJ_FLAG_HIDDEN);
    appPageBindTextField(nullptr);
    s_active->setStatus(TR("Pick an archive"));
}

void KiwixApp::startDownload(const char* url)
{
    const std::string name = zimNameFromUrl(url);
    if (name.empty())
    {
        setStatus(TR("Enter a full .zim URL"));
        return;
    }
    if (httpDownloadBusy())
    {
        setStatus(TR("A download is already running"));
        return;
    }
    SD.mkdir(kDownloadDir);
    char dest[160];
    snprintf(dest, sizeof dest, "%s/%s", kDownloadDir, name.c_str());
    if (!httpDownloadStart(url, dest, 0))
    {
        setStatus(TR("Cannot start the download"));
        return;
    }
    if (!dlTimer_) dlTimer_ = lv_timer_create(downloadTickCb, 400, nullptr);
    setStatus(TR("Starting the download\xE2\x80\xA6"));
}

void KiwixApp::downloadTickCb(lv_timer_t* t)
{
    (void)t;
    KiwixApp* app = s_active;
    if (!app) return;
    const HttpDownloadState& st = httpDownloadState();

    if (st.running)
    {
        char msg[128];
        if (st.total)
        {
            snprintf(msg, sizeof msg, "%s %u%% (%u/%u MB)", TR("Downloading"),
                     (unsigned)((st.bytes * 100) / st.total),
                     (unsigned)(st.bytes / (1024 * 1024)),
                     (unsigned)(st.total / (1024 * 1024)));
        }
        else
        {
            snprintf(msg, sizeof msg, "%s %u MB", TR("Downloading"),
                     (unsigned)(st.bytes / (1024 * 1024)));
        }
        app->setStatus(msg);
        return;
    }
    if (!st.done) return;

    const bool ok = st.ok;
    char err[96];
    snprintf(err, sizeof err, "%s", st.err);
    httpDownloadClearDone();

    if (app->dlTimer_)
    {
        lv_timer_del(app->dlTimer_);
        app->dlTimer_ = nullptr;
    }
    app->showArchives();
    app->setStatus(ok ? TR("Saved to the card") : (err[0] ? err : TR("Download failed")));
}
