// Minimal SSD1306 128x64 display module (U8g2, HW I2C).
#include "display.h"
#include "pins.h"
#include "rf_revenge_128x64.h"
#include <U8g2lib.h>
#include <Wire.h>

static U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_I2C_ADDR);

namespace display {

static bool s_present = false;

bool init() {
    Wire.begin(PIN_SDA, PIN_SCL);
    // U8g2 init returns 1 on success (it scans the I2C bus).
    int ok = u8g2.begin();
    s_present = (ok != 0);
    if (!s_present) return false;
    u8g2.setContrast(200);
    u8g2.setFont(u8g2_font_5x8_tf);
    u8g2.clearBuffer();
    u8g2.sendBuffer();
    return true;
}

bool present() { return s_present; }  // result of the init-time bus scan

void clear() { u8g2.clearBuffer(); }

void line(uint8_t y, const char *s, bool center, uint8_t x) {
    if (center) x = 0;  // centered text ignores x
    int w = u8g2.getStrWidth(s);
    if (!center) {
        if (x + w > 128) x = 0;
    } else {
        x = (128 - w) / 2;
        if (x < 0) x = 0;
    }
    u8g2.setCursor(x, y);
    u8g2.print(s);
}

void center(uint8_t y, const char *s) { line(y, s, true); }

void splash(const char *name, const char *device, const char *ver) {
    if (!s_present) return;
    // Brand splash: Blackbeard flag + wordmark. Shown ~4s at boot before
    // the BIOS/diagnostics screen. Bitmap is 128x64 1bpp (drawXBMP).
    u8g2.clearBuffer();
    u8g2.drawXBMP(0, 0, 128, 64, rf_revenge_128x64);
    u8g2.setFont(u8g2_font_4x6_tr);
    // Wordmark lines (right-aligned to x=124), positioned to clear the
    // diagonal spear (which occupies x55-70 at y18-33).
    u8g2.setFontPosBaseline();
    u8g2.setCursor(59, 13); u8g2.print(name);      // "RF REVENGE"
    u8g2.setCursor(64, 19); u8g2.print(device);    // "RF-Clown V2"
    u8g2.setCursor(70, 26); u8g2.print(ver);       // APP_VERSION
    // Baseline mode is global state — restore top mode or every later
    // small-y text draws 6px too high and clips at the top of the panel.
    u8g2.setFontPosTop();
    u8g2.sendBuffer();
}

void glyph(uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
    if (x + w > 128) w = 128 - x;
    if (y + h > 64)  h = 64 - y;
    if (w && h) u8g2.drawBox(x, y, w, h);
}

void flush() { u8g2.sendBuffer(); }

void set_contrast(uint8_t v) { u8g2.setContrast(v); }

void set_brightness(uint8_t v) {
    // SSD1306 has no PWM dimming; contrast is the only brightness knob.
    // ponytail: contrast 0 can make the screen unreadable on some modules;
    // floor at 40 if that is observed on the real panel.
    u8g2.setContrast(v);
}

}
