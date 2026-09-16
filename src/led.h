#pragma once
// Single WS2812 status LED, state-machine colors (internal engineering notes NeoPixel contract).
#include <Arduino.h>

enum class LedState {
    BOOT,        // blue pulse — "powering up / probing" only
    SAFE,        // solid cold white — 3 radios down, safe to handle
    PROXIMITY,   // cyan pulse (rate = signal strength when a target is locked)
    DEGRADED,    // yellow (missing radio / degraded hardware)
    ARMED,       // orange (lab session armed, warning)
    ACTIVE,      // solid red (lab session active)
    FATAL,       // red pulse
    OFF
};

namespace led {
void init(bool enabled, uint8_t brightness);   // brightness 0..255, low to save battery
void set(LedState s);
bool pulsing();            // true when the current state fades (BOOT/PROXIMITY/SAFE/ACTIVE/FATAL)
void fade(uint32_t now);   // continuous sine fade; call every ~30 ms while pulsing
void set_pulse_ms(uint16_t ms);  // pulse half-period for the current state
uint16_t pulse_ms();
void set_enabled(bool on);
void set_brightness(uint8_t v);   // 0..255
void color(uint8_t r, uint8_t g, uint8_t b);  // direct color (boot parade); brightness-scaled
bool enabled();
}
