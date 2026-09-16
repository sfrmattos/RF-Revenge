#pragma once
// Debounced three-button input with edge events and long-press detection.
// Call update() every loop iteration — non-blocking.
#include <Arduino.h>

enum class Btn : uint8_t { LEFT = 0, RIGHT = 1, SELECT = 2 };
enum class BtnEvent : uint8_t { NONE = 0, PRESS, RELEASE, LONG };

struct BtnEv {
    Btn btn;
    BtnEvent ev;
    bool valid() const { return ev != BtnEvent::NONE; }
};

namespace buttons {
void init();
void update(uint32_t now_ms);
BtnEv poll();                 // returns and clears the pending event
bool is_down(Btn b);          // debounced state (for diagnostics)
}
