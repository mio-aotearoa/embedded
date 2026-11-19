// SPDX-License-Identifier: MIT
// Minimal TFT_eSPI / TFT_eSprite stand-in for the OutRun port: draws into an
// RGB565 buffer (usually the LVGL canvas), not a real SPI panel.
#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TFT_BLACK       0x0000
#define TFT_NAVY        0x000F
#define TFT_DARKGREEN   0x03E0
#define TFT_DARKCYAN    0x03EF
#define TFT_MAROON      0x7800
#define TFT_PURPLE      0x780F
#define TFT_OLIVE       0x7BE0
#define TFT_LIGHTGREY   0xC618
#define TFT_DARKGREY    0x7BEF
#define TFT_BLUE        0x001F
#define TFT_GREEN       0x07E0
#define TFT_CYAN        0x07FF
#define TFT_RED         0xF800
#define TFT_MAGENTA     0xF81F
#define TFT_YELLOW      0xFFE0
#define TFT_WHITE       0xFFFF
#define TFT_ORANGE      0xFD20
#define TFT_GREENYELLOW 0xAFE5
#define TFT_PINK        0xF81F

#define PSRAM_ENABLE 1

class TFT_eSPI
{
  public:
    TFT_eSPI(int16_t = 320, int16_t = 240) {}
    void begin() {}
    void setRotation(uint8_t) {}
    void fillScreen(uint16_t) {}
    uint16_t color565(uint8_t r, uint8_t g, uint8_t b)
    {
        return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
};

class TFT_eSprite
{
  public:
    explicit TFT_eSprite(TFT_eSPI* = nullptr) {}
    ~TFT_eSprite() { deleteSprite(); }

    // Prefer an external buffer (LVGL canvas). createSprite allocates PSRAM if
    // no external buffer was bound.
    void setBuffer(uint16_t* buf, int16_t w, int16_t h)
    {
        deleteSprite();
        _buf = buf;
        _w = w;
        _h = h;
        _owned = false;
        _created = buf != nullptr;
    }

    void* createSprite(int16_t w, int16_t h);
    void deleteSprite();

    void pushSprite(int32_t, int32_t) {}  // display owned by the AppPage canvas
    void pushToSprite(TFT_eSprite* dst, int32_t x, int32_t y);

    void fillSprite(uint16_t color);
    void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color);
    void drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color);
    void drawFastHLine(int32_t x, int32_t y, int32_t w, uint16_t color);
    void drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t color);
    void drawPixel(int32_t x, int32_t y, uint16_t color);
    void fillTriangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                      uint16_t color);
    void fillCircle(int32_t x, int32_t y, int32_t r, uint16_t color);
    void drawCircle(int32_t x, int32_t y, int32_t r, uint16_t color);
    void fillEllipse(int32_t x, int32_t y, int32_t rx, int32_t ry, uint16_t color);

    void setTextColor(uint16_t c)
    {
        _tc = c;
        _tbg = c;
        _fillBg = false;
    }
    void setTextColor(uint16_t fg, uint16_t bg)
    {
        _tc = fg;
        _tbg = bg;
        _fillBg = true;
    }
    void setTextSize(uint8_t s) { _ts = s < 1 ? 1 : s; }
    void setCursor(int16_t x, int16_t y)
    {
        _cx = x;
        _cy = y;
    }

    void print(const char* s);
    void print(const String& s) { print(s.c_str()); }
    void print(int n);
    void print(float n);

    void setColorDepth(int8_t) {}
    void setAttribute(uint8_t, uint8_t) {}

    uint16_t* buffer() const { return _buf; }
    int16_t width() const { return _w; }
    int16_t height() const { return _h; }
    bool created() const { return _created; }

  private:
    void put(int32_t x, int32_t y, uint16_t c)
    {
        if (!_buf || x < 0 || y < 0 || x >= _w || y >= _h) return;
        _buf[(size_t)y * (size_t)_w + (size_t)x] = c;
    }
    void glyph(char ch);

    uint16_t* _buf = nullptr;
    int16_t _w = 0, _h = 0;
    bool _created = false;
    bool _owned = false;
    int16_t _cx = 0, _cy = 0;
    uint16_t _tc = TFT_WHITE, _tbg = TFT_BLACK;
    uint8_t _ts = 1;
    bool _fillBg = false;
};
