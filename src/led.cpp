// Single WS2812 status LED, state-machine colors (internal engineering notes NeoPixel contract).
#include "led.h"
#include "pins.h"
#include <Adafruit_NeoPixel.h>
#include <math.h>

static Adafruit_NeoPixel strip(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
static LedState s_state = LedState::OFF;
static bool s_enabled = true;
static uint8_t s_brightness = 30;  // low default to limit battery draw
static uint16_t s_pulse_ms = 500;  // default pulse half-period
static int8_t s_fade_step = -1;    // quantized fade level last rendered

// (low, high) RGB endpoints per fading state; the fade interpolates between them.
struct RGB { uint8_t r, g, b; };
static void pair_for(LedState s, RGB &lo, RGB &hi) {
    switch (s) {
    case LedState::BOOT:      lo = {0, 0, 30};   hi = {0, 0, 120};   break;
    case LedState::SAFE:      lo = {8, 8, 8};      hi = {80, 80, 80};    break;  // floor = one level above off; peak 80 neutral white
    case LedState::PROXIMITY: lo = {0, 30, 30};  hi = {0, 120, 120}; break;
    case LedState::ACTIVE:    lo = {60, 0, 0};   hi = {220, 0, 0};   break;
    case LedState::FATAL:     lo = {40, 0, 0};   hi = {220, 0, 0};   break;
    default:                  lo = {0, 0, 0};    hi = {0, 0, 0};     break;
    }
}

static bool needs_pulse() { return s_state == LedState::BOOT || s_state == LedState::FATAL || s_state == LedState::PROXIMITY || s_state == LedState::SAFE || s_state == LedState::ACTIVE; }

// Scale one channel by the user's brightness in float, round once. The strip
// runs at full resolution (setBrightness(255)): the old integer global scaler
// (v*br/255, truncating) collapsed the dim end of the fade into ~15 visible
// levels — that was the "steps". Float math + one final round keeps 8-bit
// smoothness; real current draw is unchanged (it follows the RGB value).
static uint8_t br8(uint8_t v) {
    return (uint8_t)((v * (float)s_brightness / 255.0f) + 0.5f);
}

static void render(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_enabled) return;
    strip.setPixelColor(0, strip.Color(br8(r), br8(g), br8(b)));
    strip.show();
}

namespace led {

// Solid (non-fading) states render once, here.
static void solid_render(LedState s) {
    switch (s) {
    case LedState::DEGRADED:  render(120, 120, 0);  break;
    case LedState::ARMED:     render(200, 90, 0);   break;
    default:                  render(0, 0, 0);      break;
    }
}

void init(bool enabled, uint8_t brightness) {
    s_enabled = enabled;
    if (brightness > 0) s_brightness = brightness;
    s_fade_step = -1;
    strip.begin();
    strip.setBrightness(255);  // full resolution; per-channel br8() applies user brightness
    s_state = enabled ? LedState::BOOT : LedState::OFF;
    if (needs_pulse()) render(0, 0, 0); else solid_render(s_state);
}

void set_enabled(bool on) {
    s_enabled = on;
    if (!on) { strip.setPixelColor(0, 0); strip.show(); }
}

void set_brightness(uint8_t v) {
    s_brightness = v ? v : 1;
    // Pulsing states are owned by fade() — re-rendering here (the old
    // render(0,0,0)) fought the fade every loop iteration and flickered in
    // SETTINGS, which calls set_brightness() on every pass.
    if (!needs_pulse()) solid_render(s_state);
}

bool enabled() { return s_enabled; }

void set(LedState s) {
    s_state = s;
    s_fade_step = -1;
    s_pulse_ms = (s == LedState::SAFE) ? 1500 : (s == LedState::FATAL) ? 200 : 500;  // SAFE breathes slow (~3 s full cycle); FATAL blinks fast; PROXIMITY callers override with the RSSI rate
    if (needs_pulse()) render(0, 0, 0); else solid_render(s_state);
}

bool pulsing() { return needs_pulse() && s_enabled; }

// Direct color (boot splash parade). Bypasses the state machine; the next
// set() call restores it. render() already applies user brightness.
void color(uint8_t r, uint8_t g, uint8_t b) {
    s_state = LedState::OFF;
    s_fade_step = -1;
    render(r, g, b);
}

// Continuous sine fade: quantize the sine into 64 steps so we only write to
// the strip when the visible level actually changes (~30 ms cadence).
// Linear interpolation (no gamma): gamma-2 pushed the fade into the WS2812
// dim-knee, where the eye resolves every step and the R/G/B dies separate —
// that made the staircase and the faint colored tails worse.
void fade(uint32_t now) {
    if (!needs_pulse()) return;
    uint32_t period = 2u * s_pulse_ms;
    uint32_t phase = now % period;
    if (phase >= s_pulse_ms) phase = period - phase;           // triangle 0..half..0
    // triangle -> smooth: shape = 0.5 - 0.5*cos(pi * phase/half)
    float t = (float)phase / (float)s_pulse_ms;
    float shape = 0.5f - 0.5f * cosf(3.14159265f * t);
    int8_t step = (int8_t)(shape * 63.0f);
    if (step == s_fade_step) return;
    s_fade_step = step;
    RGB lo, hi;
    pair_for(s_state, lo, hi);
    float f = (float)step / 63.0f;
    render((uint8_t)(lo.r + (hi.r - lo.r) * f),
           (uint8_t)(lo.g + (hi.g - lo.g) * f),
           (uint8_t)(lo.b + (hi.b - lo.b) * f));
}

void set_pulse_ms(uint16_t ms) { s_pulse_ms = ms < 80 ? 80 : ms; }  // ponytail: 80 ms floor keeps the fade cadence sane

uint16_t pulse_ms() { return s_pulse_ms; }

}
