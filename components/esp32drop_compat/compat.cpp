#include "Arduino.h"

#include <stdarg.h>

uint32_t millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

uint32_t micros(void)
{
    return (uint32_t)esp_timer_get_time();
}

void delay(uint32_t ms)
{
    if (ms == 0) {
        return;
    }
    TickType_t ticks = pdMS_TO_TICKS(ms);
    if (ticks == 0) {
        ticks = 1;
    }
    vTaskDelay(ticks);
}

void delayMicroseconds(uint32_t us)
{
    if (us == 0) {
        return;
    }
    // esp_rom_delay_us is cycle-calibrated and safe in tasks and ISRs.
    extern void esp_rom_delay_us(uint32_t us);
    esp_rom_delay_us(us);
}

EspCompatSerial Serial;

void EspCompatSerial::print(const char *s)
{
    if (s) {
        fputs(s, stdout);
    }
}

void EspCompatSerial::println(const char *s)
{
    if (s) {
        fputs(s, stdout);
    }
    fputc('\n', stdout);
}

void EspCompatSerial::println(void)
{
    fputc('\n', stdout);
}

int EspCompatSerial::printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}
