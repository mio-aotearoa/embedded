// SPDX-License-Identifier: MIT
#include "TFT_eSPI.h"
#include "psram_alloc.h"

#include <math.h>
#include <stdio.h>

void* TFT_eSprite::createSprite(int16_t w, int16_t h)
{
    deleteSprite();
    if (w <= 0 || h <= 0) return nullptr;
    const size_t bytes = (size_t)w * (size_t)h * sizeof(uint16_t);
    _buf = (uint16_t*)psramPreferAlloc(bytes);
    if (!_buf) return nullptr;
    _w = w;
    _h = h;
    _owned = true;
    _created = true;
    memset(_buf, 0, bytes);
    return _buf;
}

void TFT_eSprite::deleteSprite()
{
    if (_owned && _buf) psramPreferFree(_buf);
    _buf = nullptr;
    _w = _h = 0;
    _owned = false;
    _created = false;
}

void TFT_eSprite::pushToSprite(TFT_eSprite* dst, int32_t x, int32_t y)
{
    if (!dst || !_created || !dst->_created || !_buf || !dst->_buf) return;
    for (int32_t row = 0; row < _h; ++row)
    {
        const int32_t dy = y + row;
        if (dy < 0 || dy >= dst->_h) continue;
        for (int32_t col = 0; col < _w; ++col)
        {
            const int32_t dx = x + col;
            if (dx < 0 || dx >= dst->_w) continue;
            dst->_buf[(size_t)dy * (size_t)dst->_w + (size_t)dx] =
                _buf[(size_t)row * (size_t)_w + (size_t)col];
        }
    }
}

void TFT_eSprite::fillSprite(uint16_t color)
{
    if (!_buf) return;
    const size_t n = (size_t)_w * (size_t)_h;
    for (size_t i = 0; i < n; ++i) _buf[i] = color;
}

void TFT_eSprite::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color)
{
    if (!_buf || w <= 0 || h <= 0) return;
    int32_t x1 = x, y1 = y, x2 = x + w, y2 = y + h;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > _w) x2 = _w;
    if (y2 > _h) y2 = _h;
    for (int32_t yy = y1; yy < y2; ++yy)
        for (int32_t xx = x1; xx < x2; ++xx) put(xx, yy, color);
}

void TFT_eSprite::drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color)
{
    if (w <= 0 || h <= 0) return;
    drawFastHLine(x, y, w, color);
    drawFastHLine(x, y + h - 1, w, color);
    drawLine(x, y, x, y + h - 1, color);
    drawLine(x + w - 1, y, x + w - 1, y + h - 1, color);
}

void TFT_eSprite::drawFastHLine(int32_t x, int32_t y, int32_t w, uint16_t color)
{
    if (!_buf || w <= 0 || y < 0 || y >= _h) return;
    int32_t x1 = x, x2 = x + w;
    if (x1 < 0) x1 = 0;
    if (x2 > _w) x2 = _w;
    uint16_t* row = _buf + (size_t)y * (size_t)_w;
    for (int32_t xx = x1; xx < x2; ++xx) row[xx] = color;
}

void TFT_eSprite::drawPixel(int32_t x, int32_t y, uint16_t color) { put(x, y, color); }

void TFT_eSprite::drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t color)
{
    int32_t dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int32_t dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    for (;;)
    {
        put(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        const int32_t e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

void TFT_eSprite::fillTriangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2,
                               int32_t y2, uint16_t color)
{
    // Sort by y
    if (y0 > y1)
    {
        int32_t t = y0;
        y0 = y1;
        y1 = t;
        t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y1 > y2)
    {
        int32_t t = y1;
        y1 = y2;
        y2 = t;
        t = x1;
        x1 = x2;
        x2 = t;
    }
    if (y0 > y1)
    {
        int32_t t = y0;
        y0 = y1;
        y1 = t;
        t = x0;
        x0 = x1;
        x1 = t;
    }
    auto edge = [](int32_t xa, int32_t ya, int32_t xb, int32_t yb, int32_t y) -> float {
        if (ya == yb) return (float)xa;
        return (float)xa + (float)(xb - xa) * (float)(y - ya) / (float)(yb - ya);
    };
    for (int32_t y = y0; y <= y2; ++y)
    {
        float xa, xb;
        if (y < y1)
        {
            xa = edge(x0, y0, x2, y2, y);
            xb = edge(x0, y0, x1, y1, y);
        }
        else
        {
            xa = edge(x0, y0, x2, y2, y);
            xb = edge(x1, y1, x2, y2, y);
        }
        if (xa > xb)
        {
            float t = xa;
            xa = xb;
            xb = t;
        }
        drawFastHLine((int32_t)xa, y, (int32_t)(xb - xa + 1), color);
    }
}

void TFT_eSprite::fillCircle(int32_t x0, int32_t y0, int32_t r, uint16_t color)
{
    if (r < 0) return;
    for (int32_t y = -r; y <= r; ++y)
    {
        const int32_t dx = (int32_t)sqrtf((float)(r * r - y * y));
        drawFastHLine(x0 - dx, y0 + y, 2 * dx + 1, color);
    }
}

void TFT_eSprite::drawCircle(int32_t x0, int32_t y0, int32_t r, uint16_t color)
{
    int32_t x = -r, y = 0, err = 2 - 2 * r;
    do
    {
        put(x0 - x, y0 + y, color);
        put(x0 - y, y0 - x, color);
        put(x0 + x, y0 - y, color);
        put(x0 + y, y0 + x, color);
        r = err;
        if (r <= y) err += ++y * 2 + 1;
        if (r > x || err > y) err += ++x * 2 + 1;
    } while (x < 0);
}

void TFT_eSprite::fillEllipse(int32_t x0, int32_t y0, int32_t rx, int32_t ry, uint16_t color)
{
    if (rx <= 0 || ry <= 0) return;
    for (int32_t y = -ry; y <= ry; ++y)
    {
        const float t = 1.0f - (float)(y * y) / (float)(ry * ry);
        if (t < 0) continue;
        const int32_t dx = (int32_t)(rx * sqrtf(t));
        drawFastHLine(x0 - dx, y0 + y, 2 * dx + 1, color);
    }
}

// Tiny 5x7 digits/letters for the HUD (ASCII 32..90).
static const uint8_t kFont5x7[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00},  // space
    {0x00, 0x00, 0x5F, 0x00, 0x00},  // !
    {0x00, 0x07, 0x00, 0x07, 0x00},  // "
    {0x14, 0x7F, 0x14, 0x7F, 0x14},  // #
    {0x24, 0x2A, 0x7F, 0x2A, 0x12},  // $
    {0x23, 0x13, 0x08, 0x64, 0x62},  // %
    {0x36, 0x49, 0x55, 0x22, 0x50},  // &
    {0x00, 0x05, 0x03, 0x00, 0x00},  // '
    {0x00, 0x1C, 0x22, 0x41, 0x00},  // (
    {0x00, 0x41, 0x22, 0x1C, 0x00},  // )
    {0x08, 0x2A, 0x1C, 0x2A, 0x08},  // *
    {0x08, 0x08, 0x3E, 0x08, 0x08},  // +
    {0x00, 0x50, 0x30, 0x00, 0x00},  // ,
    {0x08, 0x08, 0x08, 0x08, 0x08},  // -
    {0x00, 0x60, 0x60, 0x00, 0x00},  // .
    {0x20, 0x10, 0x08, 0x04, 0x02},  // /
    {0x3E, 0x51, 0x49, 0x45, 0x3E},  // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00},  // 1
    {0x42, 0x61, 0x51, 0x49, 0x46},  // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31},  // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10},  // 4
    {0x27, 0x45, 0x45, 0x45, 0x39},  // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30},  // 6
    {0x01, 0x71, 0x09, 0x05, 0x03},  // 7
    {0x36, 0x49, 0x49, 0x49, 0x36},  // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E},  // 9
};

// Sparse uppercase used by the start screen / HUD labels.
static const uint8_t kFontAZ[][5] = {
    {0x7E, 0x11, 0x11, 0x11, 0x7E},  // A
    {0x7F, 0x49, 0x49, 0x49, 0x36},  // B
    {0x3E, 0x41, 0x41, 0x41, 0x22},  // C
    {0x7F, 0x41, 0x41, 0x22, 0x1C},  // D
    {0x7F, 0x49, 0x49, 0x49, 0x41},  // E
    {0x7F, 0x09, 0x09, 0x09, 0x01},  // F
    {0x3E, 0x41, 0x49, 0x49, 0x7A},  // G
    {0x7F, 0x08, 0x08, 0x08, 0x7F},  // H
    {0x00, 0x41, 0x7F, 0x41, 0x00},  // I
    {0x20, 0x40, 0x41, 0x3F, 0x01},  // J
    {0x7F, 0x08, 0x14, 0x22, 0x41},  // K
    {0x7F, 0x40, 0x40, 0x40, 0x40},  // L
    {0x7F, 0x02, 0x0C, 0x02, 0x7F},  // M
    {0x7F, 0x04, 0x08, 0x10, 0x7F},  // N
    {0x3E, 0x41, 0x41, 0x41, 0x3E},  // O
    {0x7F, 0x09, 0x09, 0x09, 0x06},  // P
    {0x3E, 0x41, 0x51, 0x21, 0x5E},  // Q
    {0x7F, 0x09, 0x19, 0x29, 0x46},  // R
    {0x46, 0x49, 0x49, 0x49, 0x31},  // S
    {0x01, 0x01, 0x7F, 0x01, 0x01},  // T
    {0x3F, 0x40, 0x40, 0x40, 0x3F},  // U
    {0x1F, 0x20, 0x40, 0x20, 0x1F},  // V
    {0x3F, 0x40, 0x38, 0x40, 0x3F},  // W
    {0x63, 0x14, 0x08, 0x14, 0x63},  // X
    {0x07, 0x08, 0x70, 0x08, 0x07},  // Y
    {0x61, 0x51, 0x49, 0x45, 0x43},  // Z
};

static const uint8_t* glyphBits(char ch)
{
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    if (ch >= 32 && ch <= 57) return kFont5x7[ch - 32];
    if (ch >= 'A' && ch <= 'Z') return kFontAZ[ch - 'A'];
    return kFont5x7[0];
}

void TFT_eSprite::glyph(char ch)
{
    const uint8_t* g = glyphBits(ch);
    const int s = _ts;
    for (int col = 0; col < 5; ++col)
    {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; ++row)
        {
            const bool on = (bits >> row) & 1;
            for (int dy = 0; dy < s; ++dy)
                for (int dx = 0; dx < s; ++dx)
                {
                    const int32_t px = _cx + col * s + dx;
                    const int32_t py = _cy + row * s + dy;
                    if (on) put(px, py, _tc);
                    else if (_fillBg) put(px, py, _tbg);
                }
        }
    }
    _cx = (int16_t)(_cx + 6 * s);
}

void TFT_eSprite::print(const char* s)
{
    if (!s) return;
    for (; *s; ++s)
    {
        if (*s == '\n')
        {
            _cx = 0;
            _cy = (int16_t)(_cy + 8 * _ts);
            continue;
        }
        glyph(*s);
    }
}

void TFT_eSprite::print(int n)
{
    char buf[16];
    snprintf(buf, sizeof buf, "%d", n);
    print(buf);
}

void TFT_eSprite::print(float n)
{
    char buf[16];
    snprintf(buf, sizeof buf, "%.1f", (double)n);
    print(buf);
}
