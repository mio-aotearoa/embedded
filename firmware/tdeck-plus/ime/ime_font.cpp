// SPDX-License-Identifier: GPL-3.0-or-later
#include "ime_font.h"

#include "ime_paths.h"

#include <Arduino.h>
#include <SD.h>

#include <string.h>

namespace
{
lv_fs_drv_t s_drv;
bool s_drvReady = false;
lv_font_t *s_font = nullptr;
bool s_tried = false;

void *fsOpen(lv_fs_drv_t *, const char *path, lv_fs_mode_t mode)
{
    if (!path) return nullptr;
    // lv_font_load passes "S:/ime/cjk14.bin" — strip the drive letter.
    if (path[0] && path[1] == ':') path += 2;
    File f = SD.open(path, (mode & LV_FS_MODE_WR) ? FILE_WRITE : FILE_READ);
    if (!f) return nullptr;
    File *heap = new File(f);
    return heap;
}

lv_fs_res_t fsClose(lv_fs_drv_t *, void *file_p)
{
    if (!file_p) return LV_FS_RES_OK;
    File *f = (File *)file_p;
    f->close();
    delete f;
    return LV_FS_RES_OK;
}

lv_fs_res_t fsRead(lv_fs_drv_t *, void *file_p, void *buf, uint32_t btr, uint32_t *br)
{
    File *f = (File *)file_p;
    if (!f || !buf) return LV_FS_RES_INV_PARAM;
    int n = f->read((uint8_t *)buf, btr);
    if (n < 0) return LV_FS_RES_FS_ERR;
    if (br) *br = (uint32_t)n;
    return LV_FS_RES_OK;
}

lv_fs_res_t fsSeek(lv_fs_drv_t *, void *file_p, uint32_t pos, lv_fs_whence_t whence)
{
    File *f = (File *)file_p;
    if (!f) return LV_FS_RES_INV_PARAM;
    if (whence == LV_FS_SEEK_SET)
    {
        if (!f->seek(pos)) return LV_FS_RES_FS_ERR;
    }
    else if (whence == LV_FS_SEEK_CUR)
    {
        if (!f->seek(f->position() + pos)) return LV_FS_RES_FS_ERR;
    }
    else if (whence == LV_FS_SEEK_END)
    {
        const uint32_t sz = f->size();
        if (!f->seek(sz >= pos ? sz - pos : 0)) return LV_FS_RES_FS_ERR;
    }
    return LV_FS_RES_OK;
}

lv_fs_res_t fsTell(lv_fs_drv_t *, void *file_p, uint32_t *pos_p)
{
    File *f = (File *)file_p;
    if (!f || !pos_p) return LV_FS_RES_INV_PARAM;
    *pos_p = f->position();
    return LV_FS_RES_OK;
}

void ensureDrv(void)
{
    if (s_drvReady) return;
    lv_fs_drv_init(&s_drv);
    s_drv.letter = 'S';
    s_drv.open_cb = fsOpen;
    s_drv.close_cb = fsClose;
    s_drv.read_cb = fsRead;
    s_drv.seek_cb = fsSeek;
    s_drv.tell_cb = fsTell;
    lv_fs_drv_register(&s_drv);
    s_drvReady = true;
}
}  // namespace

bool imeFontEnsure(void)
{
    if (s_font) return true;
    if (s_tried) return false;
    s_tried = true;
    if (!SD.exists(IME_CJK_FONT)) return false;
    ensureDrv();
    s_font = lv_font_load("S:" IME_CJK_FONT);
    return s_font != nullptr;
}

bool imeFontReady(void) { return s_font != nullptr; }

const lv_font_t *imeFont(void)
{
    imeFontEnsure();
    return s_font;
}

void imeFontAttachFallback(lv_font_t *base)
{
    if (!base) return;
    const lv_font_t *cjk = imeFont();
    if (!cjk) return;
    // Walk to the end of the existing fallback chain, then append.
    lv_font_t *p = base;
    while (p->fallback) p = (lv_font_t *)p->fallback;
    if (p == cjk) return;
    p->fallback = cjk;
}
