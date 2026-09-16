#pragma once
// Pure proximity-meter logic, no hardware: shared by the device build and
// the host unit tests.
//
// Trend policy (internal engineering notes: "trend changes usefully when moving, without
// rapid UI oscillation while stationary"):
//   - EMA smooths raw RSSI (alpha).
//   - The TREND is the sign of the EMA change over a short window
//     (kHist-1 samples, ~1.2 s at the 400 ms locked rescan), gated by a
//     deadband. A per-sample deadband (0.8 dB, pre-0.3.3) fired on
//     multipath fading and showed STRONGER/WEAKER while stationary; the
//     windowed delta cancels fast fading. A sustained move shows
//     STRONGER/WEAKER through the EMA settling transient, then STABLE.
//
// ponytail: window length + deadband are tuned for the 400 ms locked
// rescan; change that period and retune both.
#include <cstdint>

struct RssiSmoothen {
    float ema = 0.0f;
    int8_t trend = 0;        // 0=STABLE 1=STRONGER -1=WEAKER
    float alpha = 0.5f;      // per-sample EMA weight (fast: ~1 s to settle)
    float deadband_dB = 2.0f; // threshold on the WINDOWED delta (see below)
    static const int kHist = 4;  // window = kHist-1 samples (~1.2 s at 400 ms locked rescan)
    float hist_[kHist];
    int hpos = 0, hfill = 0;
    bool primed = false;

    // Feed one raw scan reading; returns the current trend.
    //
    // Trend policy: the trend is the sign of (ema - ema kHist-1 samples ago),
    // gated by deadband_dB. A per-sample deadband (0.8 dB, pre-0.3.3) reacted
    // to multipath fading (1-2 dB wiggles, common on weak/floor-separated
    // links) and showed STRONGER/WEAKER while the reader was stationary.
    // Comparing over a ~1.2 s window cancels fast fading; the trend only
    // moves for a real, sustained level change, and latches briefly through
    // the EMA settling transient before returning to STABLE.
    //
    // ponytail: kHist assumes the 400 ms locked rescan; if that period
    // changes, retune kHist (target window ~1-2 s) and deadband_dB.
    int8_t update(float rssi_dBm) {
        if (!primed) {
            ema = rssi_dBm;
            primed = true;
        } else {
            ema += alpha * (rssi_dBm - ema);
        }
        hist_[hpos % kHist] = ema;
        hpos++;
        if (hfill < kHist) hfill++;
        float ref = (hfill >= kHist) ? hist_[(hpos - kHist) % kHist] : hist_[0];
        float d = ema - ref;
        if (d >  deadband_dB)      trend = 1;
        else if (d < -deadband_dB) trend = -1;
        else                       trend = 0;
        return trend;
    }

    void reset() { primed = false; trend = 0; ema = 0.0f; hpos = 0; hfill = 0; }
};

// 0..15 signal bars for a -100..-40 dBm window (15 segments for the meter).
inline int rssi_bars(float rssi_dBm) {
    if (rssi_dBm < -100.0f) return 0;
    if (rssi_dBm > -40.0f)  return 15;
    int v = (int)((rssi_dBm + 100.0f) * 15.0f / 60.0f);
    return v < 0 ? 0 : (v > 15 ? 15 : v);
}

inline const char *rssi_trend_str(int8_t t) {
    return t > 0 ? "STRONGER" : (t < 0 ? "WEAKER" : "STABLE");
}

// LED pulse half-period (ms) from smoothed RSSI: fast = strong, slow = weak.
// The -100..-40 dBm window matches rssi_bars(). UX range is inference —
// the owner's visual test calibrates these two constants (widened to
// 2500..150 in 0.3.3: the 1500..200 range was too subtle at a glance).
inline uint16_t led_pulse_ms(float rssi_dBm) {
    float clamped = rssi_dBm < -100.0f ? -100.0f : (rssi_dBm > -40.0f ? -40.0f : rssi_dBm);
    float frac = (clamped + 100.0f) / 60.0f;      // 0.0 weak .. 1.0 strong
    float ms = 2500.0f - frac * 2350.0f;          // 2500 .. 150
    return (uint16_t)ms;
}
