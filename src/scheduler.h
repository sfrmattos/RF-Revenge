#pragma once
// Pure logic, no hardware: shared by the device build and host unit tests.

#include <cstdint>
#include <cstddef>

// Split a channel list into `nr` approximately equal, disjoint subsets:
// round-robin, channel i -> subset (i % nr). Radios with an empty subset idle.
// Capacity: 80 channels per radio (full 2.4 GHz ISM band = 79 channels).
inline void split_channels(const uint8_t *channels, size_t n, int nr,
                           uint8_t subsets[3][80], size_t sizes[3]) {
    for (int i = 0; i < 3; ++i) sizes[i] = 0;
    for (size_t i = 0; i < n; ++i) {
        int r = (int)(i % (size_t)nr);
        if (sizes[r] < 80) subsets[r][sizes[r]++] = channels[i];
    }
}

// One-channel-at-a-time per radio: a radio only ever holds subsets[i][cur[i]].
// Each radio hops on its own tick period (round-robin subset).
struct Scheduler {
    uint8_t subsets[3][80];
    size_t sizes[3];
    int nr;
    uint32_t hop_ms;
    int cur[3];
    uint32_t next_tick[3];

    void init(const uint8_t *channels, size_t n, int nr_, uint32_t hop) {
        split_channels(channels, n, nr_, subsets, sizes);
        nr = nr_;
        hop_ms = hop;
        for (int i = 0; i < 3; ++i) {
            cur[i] = 0;
            next_tick[i] = (sizes[i] > 0) ? hop : 0;
        }
    }

    void tick(uint32_t now_ms) {
        for (int i = 0; i < nr; ++i) {
            if (sizes[i] == 0) continue;
            if (now_ms >= next_tick[i]) {
                cur[i] = (cur[i] + 1) % (int)sizes[i];
                next_tick[i] = now_ms + hop_ms;
            }
        }
    }

    bool has_channel(int i) const { return i >= 0 && i < 3 && sizes[i] > 0; }
    uint8_t channel(int i) const { return subsets[i][cur[i]]; }
};

// Bounded session timer.
struct SessionTimer {
    uint32_t start_ms = 0;
    uint32_t cap_ms = 0;
    uint32_t deadline_ms = 0;
    bool stop_requested = false;

    void start(uint32_t now_ms, uint32_t cap) {
        start_ms = now_ms;
        cap_ms = cap;
        deadline_ms = now_ms + cap;
        stop_requested = false;
    }

    bool active(uint32_t now_ms) const { return now_ms < deadline_ms; }
    bool expired(uint32_t now_ms) const { return now_ms >= deadline_ms; }
    bool should_stop(uint32_t now_ms) const { return stop_requested || expired(now_ms); }
    uint32_t remaining(uint32_t now_ms) const { return active(now_ms) ? (deadline_ms - now_ms) : 0; }
    uint32_t elapsed(uint32_t now_ms) const { return now_ms > start_ms ? (now_ms - start_ms) : 0; }
};
