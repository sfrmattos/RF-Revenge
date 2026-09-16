#pragma once
// Minimal SSD1306 128x64 display module (U8g2, HW I2C).
#include <Arduino.h>

namespace display {
bool init();                       // I2C init; false if no SSD1306 at the address
bool present();                    // last init/scan result
void clear();                      // blank buffer, no flush
void line(uint8_t y, const char *s, bool center, uint8_t x = 0);
void center(uint8_t y, const char *s);
void splash(const char *name, const char *device, const char *ver);  // brand splash (flag + wordmark)
void glyph(uint8_t x, uint8_t y, uint8_t w, uint8_t h);  // filled block (meter bar)
void flush();
void set_contrast(uint8_t v);      // 0..255
void set_brightness(uint8_t v);    // 0..255 (SSD1306 has no dimming; contrast is the knob)
}
