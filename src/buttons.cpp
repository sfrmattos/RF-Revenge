// Debounced three-button input with edge events and long-press detection.
#include "buttons.h"
#include "pins.h"
#include "debounce.h"
#include <Arduino.h>

static Debounce deb;
static const uint8_t pins[3] = {PIN_BTN_LEFT, PIN_BTN_RIGHT, PIN_BTN_SELECT};
static BtnEv pending;

namespace buttons {

void init() {
    for (uint8_t i = 0; i < 3; ++i) pinMode(pins[i], INPUT_PULLUP);
    deb.reset();
    pending = BtnEv{Btn::LEFT, BtnEvent::NONE};
}

void update(uint32_t now_ms) {
    bool raw[3];
    for (uint8_t i = 0; i < 3; ++i)
        raw[i] = (digitalRead(pins[i]) == BUTTON_PRESSED_LEVEL);
    DebOut o = deb.update(now_ms, raw);
    if (o.ev != DebEvent::NONE && !pending.valid()) {
        // ponytail: one event per tick; two simultaneous distinct presses in
        // the same 30 ms window are not a real-world case (first wins).
        pending = BtnEv{
            static_cast<Btn>(static_cast<uint8_t>(o.btn)),
            static_cast<BtnEvent>(static_cast<uint8_t>(o.ev))};
    }
}

BtnEv poll() {
    BtnEv e = pending;
    pending = BtnEv{Btn::LEFT, BtnEvent::NONE};
    return e;
}

bool is_down(Btn b) { return deb.pressed(static_cast<DebBtn>(static_cast<uint8_t>(b))); }

}
