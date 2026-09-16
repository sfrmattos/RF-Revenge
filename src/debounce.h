#pragma once
// Pure button-debounce logic: no Arduino, no hardware. Shared by the
// buttons module (device) and the host unit tests.
//
// Per button: the raw level must stay stable for SETTLE_MS before the
// debounced state accepts it (bounce tolerance). A press held for
// LONG_MS (after settle) emits one LONG event; a release following a
// consumed LONG does not emit a spurious RELEASE.

#include <cstdint>

enum class DebBtn : uint8_t { LEFT = 0, RIGHT = 1, SELECT = 2 };
enum class DebEvent : uint8_t { NONE = 0, PRESS, RELEASE, LONG };

struct DebOut {
    DebBtn btn = DebBtn::LEFT;   // valid when ev != NONE
    DebEvent ev = DebEvent::NONE;
};

struct Debounce {
    static constexpr uint8_t N = 3;
    static constexpr uint32_t SETTLE_MS = 30;
    static constexpr uint32_t LONG_MS = 600;

    bool stable[N] = {false, false, false};
    bool raw_last[N] = {false, false, false};
    uint32_t since_change[N] = {0, 0, 0};
    bool long_fired[N] = {false, false, false};

    // Feed current raw "pressed" levels (polarity already adjusted) and time.
    // Returns at most one event per call; a LONG event takes priority.
    DebOut update(uint32_t now_ms, const bool raw[N]) {
        DebOut out;
        for (uint8_t i = 0; i < N; ++i) {
            bool p = raw[i];
            if (p != raw_last[i]) {
                raw_last[i] = p;
                since_change[i] = now_ms;
            } else if (now_ms - since_change[i] >= SETTLE_MS && p != stable[i]) {
                stable[i] = p;
                if (p) {
                    long_fired[i] = false;
                    if (out.ev == DebEvent::NONE) { out.btn = (DebBtn)i; out.ev = DebEvent::PRESS; }
                } else {
                    bool was_long = long_fired[i];
                    long_fired[i] = false;
                    if (!was_long && out.ev == DebEvent::NONE) {
                        out.btn = (DebBtn)i; out.ev = DebEvent::RELEASE;
                    }
                }
            }
            if (p && stable[i] && !long_fired[i] &&
                now_ms - since_change[i] >= SETTLE_MS + LONG_MS) {
                long_fired[i] = true;
                out.btn = (DebBtn)i;       // long press always wins
                out.ev = DebEvent::LONG;
            }
        }
        return out;
    }

    bool pressed(DebBtn b) const { return stable[(uint8_t)b]; }
    void reset() {
        for (uint8_t i = 0; i < N; ++i) {
            stable[i] = raw_last[i] = long_fired[i] = false;
            since_change[i] = 0;
        }
    }
};
