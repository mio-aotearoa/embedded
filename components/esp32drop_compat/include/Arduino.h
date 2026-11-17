// Minimal Arduino compatibility layer for building ESP32Drop as an ESP-IDF
// component. ESP32Drop is an Arduino library whose ports only need millis()
// (and, defensively, Serial/delay helpers); everything else it uses is ESP-IDF.
//
// This is deliberately tiny: no String, no Print hierarchy, no Arduino core.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef ARDUINO
#define ARDUINO 100
#endif

uint32_t millis(void);
uint32_t micros(void);
void delay(uint32_t ms);
void delayMicroseconds(uint32_t us);

// ESP32Drop never writes to Serial (it stages diagnostics instead), but the
// port files include <Arduino.h> and mention it; provide a working sink so any
// diagnostic path still compiles and prints from the calling task.
class EspCompatSerial
{
  public:
    void begin(unsigned long = 115200) {}
    void flush(void) {}
    int available(void) { return 0; }
    int read(void) { return -1; }
    void print(const char *s);
    void println(const char *s);
    void println(void);
    int printf(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
    operator bool() const { return true; }
};

extern EspCompatSerial Serial;
