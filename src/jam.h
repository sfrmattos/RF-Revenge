#pragma once
// nRF24 jamming module (internal engineering notes phase 8). Compile-gated: JAM_ENABLE.
//
// Technique: a continuous DC carrier (CONT_WAVE) on each of the three nRF24
// radios, each hopping through a disjoint one-third of the band
// (Scheduler::split_channels, round-robin). Untargeted, band-based: it
// degrades 2.4 GHz systems sharing the frequency; it does not transmit
// frames, decode protocols, or target devices.
//
// The pure channel-split logic lives in scheduler.h (host-tested). The
// device-side carrier control uses the PUBLIC RF24 API startConstCarrier() /
// stopConstCarrier() (RF24 1.6.2, verified in the pinned lib).

#include <cstdint>
#include "scheduler.h"

// ---------------------------------------------------------------------------
// Pure band math (host-testable, no hardware).
// ---------------------------------------------------------------------------
namespace jam {

// Band profiles (band labels only — internal engineering notes: protocol claims require
// owned-device validation). Target catalog: the user manual
enum class Profile { BLE, WIFI, ZIGBEE, DCE, HID, RC, FPV, ALL, MAP };
static const size_t kProfileCount = 9;

// nRF24 channel list for a profile, ascending: [lo, hi] inclusive.
// f = 2400 + ch. Full 2.4 GHz ISM = 2402-2480 MHz -> ch 2-80 (79 channels).
// Wi-Fi ch1-13 = 2412-2467 -> ch 12-72. Zigbee ch11-26 = 2405-2480 -> ch 5-80.
// DCE = 2423-2447 -> ch 23-47.
inline size_t profile_channels(Profile p, uint8_t out[80]) {
    int lo = 2, hi = 80;
    switch (p) {
        case Profile::WIFI:   lo = 12; hi = 72; break;
        case Profile::ZIGBEE: lo = 5;  hi = 80; break;
        case Profile::DCE:    lo = 23; hi = 47; break;
        default: break;   // BLE, HID, RC, FPV, ALL, MAP: full band (MAP refines to the band menu)
    }
    for (int c = lo; c <= hi; ++c) out[c - lo] = (uint8_t)c;
    return (size_t)(hi - lo + 1);
}

// Per-radio hop period: fast-FHSS victims (Wi-Fi narrow channels, HID/RC
// dongles, everything) get short dwell; fixed/slow-hop victims (BLE, Zigbee,
// DCE, analog video — fixed channel) get slow dwell to maximize time on the
// victim's channel.
inline uint32_t profile_hop_ms(Profile p) {
    switch (p) {
        case Profile::WIFI: case Profile::HID: case Profile::RC: case Profile::ALL:
            return 120;
        default: return 400;
    }
}

// RPD activity histogram for the MAP mode: radio A scans the band
// (signal >= -64 dBm), filling a decaying histogram; the victim's hopping
// subset is the set of channels above the noise floor. A full-band uniform
// hopper is NOT jam-able this way (the subset is the whole band); the OLED
// histogram is the diagnostic that shows which case you have.
//
// RPD limitation (measured): it does NOT latch OFDM Wi-Fi (GFSK energy
// detector), and the nRF24 RSSI register is dead on this board (constant).
// So the MAP also seeds the histogram from the ESP32's own Wi-Fi scan
// (wifi_ble) — a sensitive 802.11 receiver that sees the room's APs.
struct TrackHist {
    uint16_t v[81] = {0};   // index = nRF24 channel (2..80 used)
    void reset() { for (auto &x : v) x = 0; }
    // Called once per full scan pass: forgets ~12% of accumulated energy so
    // dead channels cool off within a couple of passes.
    void decay() { for (auto &x : v) x = (uint16_t)(x * 88 / 100); }
    // Top-k channels by energy within [lo, hi]; ties broken by lower channel.
    // Channels with energy below `min` are ignored (RPD noise floor).
    // out[k] = channel.
    size_t top_k(uint8_t out[], size_t k, uint8_t lo, uint8_t hi, uint16_t min = 3) const {
        size_t n = 0;
        bool used[81] = {false};
        for (size_t t = 0; t < k && n < k; ++t) {
            int best = -1; uint16_t bestv = min - 1;
            for (int c = lo; c <= hi; ++c)
                if (!used[c] && v[c] > bestv) { bestv = v[c]; best = c; }
            if (best < 0) break;
            out[n++] = (uint8_t)best;
            used[best] = true;
        }
        return n;
    }
    // MAP mode: pick up to `k` channels above the noise floor (the victim's
    // hopping subset). Returns how many were found; out[i] = channel,
    // descending by energy. Fewer than `k` = the rest are filled by the
    // caller's fallback (full band).
    size_t select_channels(uint8_t out[], size_t k, uint8_t lo, uint8_t hi,
                           uint16_t min = 3) const {
        uint8_t tmp[80];
        size_t n = top_k(tmp, k, lo, hi, min);
        for (size_t i = 0; i < n; ++i) out[i] = tmp[i];
        return n;
    }
};

// Wi-Fi channel (1-14) -> nRF24 channel: 2407 + 5*ch == 2400 + nrf.
// (ch1 -> 12, ch3 -> 22, ch11 -> 62, ch14 -> 77)
inline int wifi_to_nrf(int wifi_ch) { return 5 * wifi_ch + 7; }

}  // namespace jam

// ---------------------------------------------------------------------------
// Device UI state (JAM_ENABLE only; guarded out of host unit tests).
// ---------------------------------------------------------------------------
#if defined(ARDUINO)
#include <RF24.h>   // rf24_pa_dbm_e (PA level for the carrier)
#include <esp_timer.h>
#include "soc/gpio_reg.h"   // GPIO_OUT_W1TC_REG — direct register write in the ISR
#include "pins.h"
#include "wifi_ble.h"       // MAP: passive Wi-Fi scan seeds the subset (RPD can't see OFDM)
enum class JamState { IDLE, BAND, DUR, ARMING, ACTIVE, DONE };

// ---------------------------------------------------------------------------
// Jam TX safety watchdog — a HARDWARE timer, independent of the main loop.
//
// Why it exists: the nRF24 CONT_WAVE is autonomous hardware. Once CE is high
// it transmits with NO MCU involvement. If the main loop hangs (observed: the
// OLED I2C transfer wedges on a marginal battery, freezing the UI while the
// radios keep jamming — 2x the set duration, no button response), the software
// timer and buttons that are supposed to stop the carrier are dead with it.
// The only thing that can cut the TX is something that does not live in the
// main loop. This esp_timer ISR runs regardless; if the loop stops feeding its
// heartbeat while a carrier is up, the ISR drives all three CE pins low
// directly (a plain GPIO write — safe in ISR, no SPI, which may be wedged).
//
// Scope: jam only. De-auth is burst-based (no autonomous carrier) and stops if
// the loop hangs, so it does not need this.
// ---------------------------------------------------------------------------
static volatile int32_t jam_wdt_hb_us = 0;       // heartbeat, esp_timer clock (truncated; wrap-aware diff)
static volatile bool     jam_wdt_armed  = false; // true only while a carrier is up
static volatile bool     jam_wdt_fired  = false; // set if the watchdog cut the radios
static const uint32_t kJamWdtStaleUs = 3000000;  // 3 s: >> a normal frame, << the 30 s cap

static void IRAM_ATTR jam_wdt_isr(void *arg) {
    (void)arg;
    if (!jam_wdt_armed) return;
    // Wrap-aware: esp_timer time truncated to 32 bits (wraps every ~71 min);
    // the int32 diff is correct for any real gap < 34 min — ours is 3 s.
    int32_t now = (int32_t)esp_timer_get_time();
    if (now - jam_wdt_hb_us > (int32_t)kJamWdtStaleUs) {
        // Main loop hung with a carrier on — cut the TX directly at the pins.
        // Direct GPIO register write (write-1-to-clear), IRAM_ATTR: this must
        // fire even if flash is blocked, so no library call (digitalWrite is
        // not IRAM_ATTR). CE low = carrier off on the nRF24.
        WRITE_PERI_REG(GPIO_OUT_W1TC_REG,
                       (1u << RADIO_A_CE) | (1u << RADIO_B_CE) | (1u << RADIO_C_CE));
        jam_wdt_armed = false;
        jam_wdt_fired = true;
    }
}
static esp_timer_handle_t jam_wdt = nullptr;
static bool jam_wdt_init() {
    if (jam_wdt) return true;
    const esp_timer_create_args_t cfg = { .callback = jam_wdt_isr, .name = "jam_wdt" };
    if (esp_timer_create(&cfg, &jam_wdt) != ESP_OK ||
        esp_timer_start_periodic(jam_wdt, 500000) != ESP_OK) {
        if (jam_wdt) { esp_timer_delete(jam_wdt); jam_wdt = nullptr; }
        Serial.println("JAM WDT FAIL: TX LOCKED OUT");
        return false;
    }
    Serial.println("JAM WDT ready (3 s TX watchdog, direct-CE cut)");
    return true;
}
static void jam_wdt_arm()   { jam_wdt_hb_us = (int32_t)esp_timer_get_time(); jam_wdt_armed = true; }
static void jam_wdt_feed()  { jam_wdt_hb_us = (int32_t)esp_timer_get_time(); }
static void jam_wdt_disarm(){ jam_wdt_armed = false; }

static JamState jam_state = JamState::IDLE;
static int  jam_profile = 0;      // 0=BLE 1=WI-FI 2=ZIGBEE 3=DCE 4=HID 5=RC 6=FPV 7=ALL 8=MAP
static uint32_t jam_cap_ms = 30000;   // 30 s .. 5 min, 30 s steps
static const uint32_t kJamArmMs = 3000;
static uint32_t jam_arm_until = 0;
static Scheduler jam_sched;
static SessionTimer jam_timer;
static bool jam_carrier_up[3] = {false, false, false};
static uint8_t jam_carrier_ch[3] = {0, 0, 0};

// MAP mode: a passive mapping phase (radio A scans, B/C stay OFF — no TX
// while mapping) fills the histogram with the victim's hopping subset, then
// all 3 radios jam only the channels above the noise floor (fallback: full
// band if nothing was detected). The mapping window is FREE time: the attack
// cap runs in full after it, not counted against it.
static const uint32_t kMapMs = 20000;   // 20 s: ~12 full passes at 20 ms/ch
// Energy floor for the subset: a channel must be RPD-detected in this many
// scan passes to count as "strong enough" to jam. Higher = less sensitive
// (fewer channels in the subset). 3 = ~25% of passes; 8 = ~62% of passes.
// ponytail: fixed floor — per-band or NVS-tunable if the default misfits.
static const uint16_t kMapMin = 8;
static uint32_t map_start_ms = 0;
static bool     map_started = false;   // carrier started after the mapping window
static uint8_t  map_chans[80] = {0};
static size_t   map_n = 0;
static bool     map_fallback = false;   // true = nothing above floor -> full band
// Wi-Fi seed: the RPD can't see OFDM (measured), so the ESP32's own passive
// Wi-Fi scan seeds the histogram with the room's AP channels. They show up
// in the histogram (the "what's being detected" view) and join the subset.
static int      map_wifi_n = 0;         // distinct AP channels seeded (in band)
static bool     map_wifi_up = false;    // Wi-Fi radio up for the mapping scan
// Shared scanner state (radio A, RX mode, RPD): the decaying histogram of
// band activity, the current probe channel, and the scan throttle.
static jam::TrackHist track_hist;
static uint8_t  track_scan_ch = 2;     // next channel for radio A to probe
static uint32_t track_scan_next_ms = 0;
static bool     track_have_scanner = false;  // radio A present at arm time
// Band submenu: the scanner only probes this band (fewer channels -> more
// dwell per channel -> better RPD detection).
static int track_band = 0;   // 0=ALL 1=WI-FI 2=ZIGBEE 3=VIDEO
static const uint8_t kTrackLo[4] = {2, 12, 5, 23};
static const uint8_t kTrackHi[4] = {80, 72, 80, 47};
static uint32_t jam_final_ms = 0;   // frozen elapsed at stop (DONE screen)
static const char *jam_track_band_name() {
    static const char *n[4] = {"ALL", "WI-FI", "ZIGBEE", "VIDEO"};
    return n[track_band & 3];
}

static const char *jam_profile_name() {
    static const char *names[jam::kProfileCount] =
        {"BLE", "WI-FI", "ZIGBEE", "DCE", "HID", "RC", "FPV", "ALL", "GFSK MAP & KILL"};
    return names[jam_profile % (int)jam::kProfileCount];
}

// Device-side carrier control (RF24 API; see radios::carrier_*).
// PA is the global TX setting (Settings > PA): MAX = 7 dBm, HIGH = 3 dBm on
// the L01+. HIGH = less current (weak-battery friendly), MAX = more range.
static bool jam_carrier_start(int i) {
    if (i < 0 || i > 2) return false;
    if (!radios::start_session(i)) return false;   // powerUp (PWR_UP=1)
    if (!radios::carrier_on(i, S.tx_pa_max ? RF24_PA_MAX : RF24_PA_HIGH, jam_sched.channel(i))) return false;
    jam_carrier_up[i] = true;
    jam_carrier_ch[i] = jam_sched.channel(i);
    return true;
}

static void jam_carrier_hop(int i, uint8_t ch) {
    if (!jam_carrier_up[i] || ch == jam_carrier_ch[i]) return;
    radios::carrier_hop(i, ch);   // setChannel while CONT_WAVE is on
    jam_carrier_ch[i] = ch;
}

static void jam_carrier_stop(int i) {
    if (i < 0 || i > 2) return;
    radios::carrier_off(i);       // stopConstCarrier + powerDown (CE low)
    jam_carrier_up[i] = false;
}

static void jam_enter() {
    jam_state = JamState::IDLE;
    jam_profile = 0;
    jam_cap_ms = 30000;
    radios::power_down_all();
    jam_wdt_init();
    jam_wdt_disarm();
    jam_wdt_fired = false;
    led::set(LedState::SAFE);
}

static void jam_exit() {
    radios::power_down_all();
    ui = UIState::MENU;
    led::set(LedState::SAFE);
    paint_menu();
}

static void jam_paint_idle(uint32_t now) {
    display::clear();
    display::center(8, "2.4G JAM");
    char b[24];
    snprintf(b, sizeof(b), "PROFILE: %s", jam_profile_name());
    display::line(22, b, false);
    snprintf(b, sizeof(b), "PA %s / 3 RADIOS", S.tx_pa_max ? "MAX" : "HIGH");
    display::line(32, b, false);
    snprintf(b, sizeof(b), "DUR %lus", (unsigned long)(jam_cap_ms / 1000));
    display::line(42, b, false);
    display::center(54, "L/R PROF  S ARM");
    display::flush();
}

static void jam_paint_dur(uint32_t now) {
    display::clear();
    display::center(8, "ATTACK DURATION");
    char b[24];
    snprintf(b, sizeof(b), "PROFILE %s", jam_profile_name());
    display::line(22, b, false);
    snprintf(b, sizeof(b), "DUR %lus", (unsigned long)(jam_cap_ms / 1000));
    display::center(34, b);
    display::center(54, "L/R TIME  S START");
    display::flush();
}

static uint32_t jam_last_rem_s = 0xFFFFFFFF;
static uint32_t jam_last_arm_s = 0xFFFFFFFF;   // ARMING countdown throttle (1/s)
static uint8_t jam_last_ch[3] = {0, 0, 0};

static void jam_paint_active(uint32_t now) {
    uint32_t rem = jam_timer.remaining(now) / 1000;
    // 4-step pulse (1 Hz per bar, staggered) for the "sweeping" feel.
    // Repaint only when something on screen changes — a full 1024-byte I2C
    // flush at loop rate is the wedge-risk bug (observed freeze).
    uint8_t phase = (uint8_t)(now / 250);
    static uint8_t last_phase = 0xFF;
    bool changed = (rem != jam_last_rem_s) || (phase != last_phase);
    if (!changed) {
        for (int i = 0; i < 3; ++i)
            if (jam_sched.has_channel(i) && jam_sched.channel(i) != jam_last_ch[i]) {
                changed = true;
                break;
            }
    }
    if (!changed) return;
    jam_last_rem_s = rem;
    last_phase = phase;
    for (int i = 0; i < 3; ++i)
        jam_last_ch[i] = jam_sched.has_channel(i) ? jam_sched.channel(i) : 0;

    char b[24];
    display::clear();
    display::center(8, jam_profile_name());
    // 3-bars (mock 3.1): one block per radio, pulsing, label = radio + channel.
    static const int kBarH[4] = {14, 22, 14, 6};   // 4-step pulse, bh=28
    for (int i = 0; i < 3; ++i) {
        const int bx = 8 + i * 40, by = 16, bw = 30, bh = 28;
        display::glyph(bx, by, bw, 1);
        display::glyph(bx, by + bh - 1, bw, 1);
        display::glyph(bx, by, 1, bh);
        display::glyph(bx + bw - 1, by, 1, bh);
        if (jam_sched.has_channel(i)) {
            int fh = kBarH[(phase + i) & 3];
            display::glyph(bx + 2, by + bh - 2 - fh, bw - 4, fh);
        }
        snprintf(b, sizeof(b), "%c %d", "ABC"[i], jam_last_ch[i]);
        display::line(48, b, false, bx);
    }
    snprintf(b, sizeof(b), "LEFT %lus  HOLD STOP", (unsigned long)rem);
    display::center(56, b);
    display::flush();
}

// Histogram: 25 buckets x ~3 channels (ch 2-80), 4 px tall, y 28..32.
// Shows the RPD activity of the band (MAP screen).
static void jam_paint_hist() {
    for (int bk = 0; bk < 25; ++bk) {
        int lo = 2 + bk * 3, hi = lo + 2;   // 3 ch per bucket (75 ch) + 2 extra
        if (bk == 24) hi = 80;
        uint16_t sum = 0;
        for (int c = lo; c <= hi && c <= 80; ++c) sum += track_hist.v[c];
        uint8_t h = (uint8_t)(sum > 40 ? 4 : sum / 10);   // 0..4 px, saturate
        if (h) display::glyph(bk * 5, 28 + (4 - h), 3, h);
    }
}

// MAP screen: passive mapping countdown + histogram while the subset is
// being learned; after the mapping window, the same screen shows the
// discovered subset size and the 3 radios' channels.
static void jam_paint_map(uint32_t now) {
    // Throttle on the value actually shown: the mapping countdown while
    // mapping (the attack timer is deferred, so it reads 0 then), the cap
    // countdown once jamming.
    uint32_t shown = (now < map_start_ms + kMapMs)
                         ? (map_start_ms + kMapMs - now) / 1000
                         : jam_timer.remaining(now) / 1000;
    static uint32_t last_shown_s = 0xFFFFFFFF;
    if (shown == last_shown_s) return;   // ~1/s repaint (throttle, like ACTIVE)
    last_shown_s = shown;

    char b[24];
    display::clear();
    display::center(8, "MAP ACTIVE");
    if (now < map_start_ms + kMapMs) {
        // Mapping phase: no TX, radio A learning the hopping subset. The
        // attack cap starts AFTER the map (timer deferred by kMapMs), so
        // show the two phases separately instead of a merged countdown.
        uint32_t map_rem = (map_start_ms + kMapMs - now) / 1000;
        snprintf(b, sizeof(b), "MAPPING %lus  A=SCAN", (unsigned long)map_rem);
        display::center(20, b);
        snprintf(b, sizeof(b), "THEN JAM %lus", (unsigned long)(jam_cap_ms / 1000));
        display::center(44, b);
    } else {
        // "SUBSET 79 CH" was a lie: 79 is the FULL-BAND FALLBACK value when
        // nothing was detected above the floor. Show the truth instead.
        if (map_fallback)
            display::center(20, "NO SIG - FULL BAND");
        else {
            snprintf(b, sizeof(b), "SUBSET %d CH", (int)map_n);
            display::center(20, b);
        }
        snprintf(b, sizeof(b), "JAM %d %d %d",
                 jam_carrier_up[0] ? jam_carrier_ch[0] : 0,
                 jam_carrier_up[1] ? jam_carrier_ch[1] : 0,
                 jam_carrier_up[2] ? jam_carrier_ch[2] : 0);
        display::center(36, b);
        snprintf(b, sizeof(b), "LEFT %lus", (unsigned long)(jam_timer.remaining(now) / 1000));
        display::center(44, b);
    }
    jam_paint_hist();
    display::center(54, "HOLD SELECT STOP");
    display::flush();
}

static void jam_stop(const char *reason) {
    for (int i = 0; i < 3; ++i) jam_carrier_stop(i);
    radios::power_down_all();
    wifi_ble::stop();   // MAP mapping scan is passive, but radio off = safe default
    jam_wdt_disarm();
    if (jam_wdt_fired) {
        // The watchdog already cut the radios (the loop was hung). Log it so
        // the freeze is diagnosable: the carrier was alive, the loop was not.
        Serial.println("JAM WDT: watchdog cut the carrier (loop hang)");
    }
    // Freeze the elapsed value here: elapsed(now) keeps growing after the
    // session ends (now advances), and the DONE screen repaints every loop.
    jam_final_ms = jam_timer.elapsed(millis());
    Serial.printf("JAM STOP reason=%s elapsed=%lu ms\n", reason,
                  (unsigned long)jam_final_ms);
    jam_state = JamState::DONE;
    led::set(LedState::SAFE);
}

static void jam_tick(uint32_t now, const BtnEv &ev) {
    switch (jam_state) {
    case JamState::IDLE:
        if (ev.valid() && ev.btn == Btn::LEFT  && ev.ev == BtnEvent::PRESS)
            jam_profile = (jam_profile + (int)jam::kProfileCount - 1) % (int)jam::kProfileCount;
        else if (ev.valid() && ev.btn == Btn::RIGHT && ev.ev == BtnEvent::PRESS)
            jam_profile = (jam_profile + 1) % (int)jam::kProfileCount;
        else if (ev.valid() && ev.btn == Btn::SELECT && ev.ev == BtnEvent::PRESS) {
            // MAP picks its scan band first; the other profiles go straight
            // to the duration.
            jam_state = (jam_profile == (int)jam::Profile::MAP)
                            ? JamState::BAND : JamState::DUR;
        }
        else if (ev.valid() && ev.ev == BtnEvent::LONG) {
            jam_exit();
            break;
        }
        jam_paint_idle(now);
        break;

    case JamState::BAND:
        if (ev.valid() && ev.btn == Btn::LEFT  && ev.ev == BtnEvent::PRESS)
            track_band = (track_band + 3) % 4;
        else if (ev.valid() && ev.btn == Btn::RIGHT && ev.ev == BtnEvent::PRESS)
            track_band = (track_band + 1) % 4;
        else if (ev.valid() && ev.btn == Btn::SELECT && ev.ev == BtnEvent::PRESS) {
            if (diag.ra.present || diag.rb.present || diag.rc.present)
                jam_state = JamState::DUR;
            else
                Serial.println("JAM ARM REFUSED: no radio present");
        }
        else if (ev.valid() && ev.ev == BtnEvent::LONG) {
            jam_state = JamState::IDLE;
            break;
        }
        {
            // Throttle: static screen — only repaint when the band changes
            // (a 1024-byte flush per loop iteration is the wedge-risk bug).
            static int last_tb = -1;
            if (track_band == last_tb) break;
            last_tb = track_band;
            char b[24];
            display::clear();
            display::center(8, "SCAN BAND");
            display::center(26, jam_track_band_name());
            snprintf(b, sizeof(b), "CH %d-%d", kTrackLo[track_band], kTrackHi[track_band]);
            display::center(38, b);
            display::center(54, "L/R BAND - S OK");
            display::flush();
        }
        break;

    case JamState::DUR:
        if (ev.valid() && ev.btn == Btn::LEFT  && ev.ev == BtnEvent::PRESS) {
            jam_cap_ms -= 30000;
            if (jam_cap_ms < 30000) jam_cap_ms = 300000;   // 30 s .. 5 min wrap
        } else if (ev.valid() && ev.btn == Btn::RIGHT && ev.ev == BtnEvent::PRESS) {
            jam_cap_ms += 30000;
            if (jam_cap_ms > 300000) jam_cap_ms = 30000;
        } else if (ev.valid() && ev.btn == Btn::SELECT && ev.ev == BtnEvent::PRESS) {
            if (diag.ra.present || diag.rb.present || diag.rc.present) {
                jam_state = JamState::ARMING;
                jam_arm_until = now + kJamArmMs;
                jam_last_arm_s = 0xFFFFFFFF;   // force the first ARMING repaint
                led::set(LedState::ARMED);
                Serial.printf("JAM ARM profile=%s cap=%lu ms\n",
                              jam_profile_name(), (unsigned long)jam_cap_ms);
            } else {
                Serial.println("JAM ARM REFUSED: no radio present");
            }
        }
        else if (ev.valid() && ev.ev == BtnEvent::LONG) {
            jam_state = JamState::IDLE;
            break;
        }
        jam_paint_dur(now);
        break;

    case JamState::ARMING: {
        uint32_t left = (now < jam_arm_until) ? (jam_arm_until - now) / 1000 : 0;
        if (ev.valid() && ev.ev == BtnEvent::LONG) {
            jam_state = JamState::IDLE;
            led::set(LedState::SAFE);
            break;
        }
        if (left == 0) {
            uint8_t chans[80];
            size_t n = jam::profile_channels((jam::Profile)jam_profile, chans);
            jam_sched.init(chans, n, 3, jam::profile_hop_ms((jam::Profile)jam_profile));
            if (!jam_wdt_init()) {
                radios::power_down_all();
                jam_state = JamState::DONE;
                led::set(LedState::DEGRADED);
                Serial.println("JAM START BLOCKED: watchdog unavailable");
                break;
            }
            int up = 0;
            if (jam_profile == (int)jam::Profile::MAP) {
                // Passive mapping: NO TX for the first kMapMs. Radio A
                // scans the band in RX mode and fills the histogram with the
                // victim's hopping subset; B/C stay powered down. After the
                // window, all 3 radios jam only the discovered channels.
                track_hist.reset();
                track_scan_ch = kTrackLo[track_band];
                track_scan_next_ms = now;
                track_have_scanner = diag.ra.present;
                map_start_ms = now;
                map_n = 0;
                map_fallback = false;
                map_wifi_n = 0;
                map_started = false;
                map_wifi_up = false;
                if (track_have_scanner && radios::start_rx(0))
                    jam_carrier_up[0] = false;   // A listens; B/C off
                // The RPD can't latch OFDM Wi-Fi (measured: hot=0/79 in a
                // room full of APs). The ESP32's own passive Wi-Fi scan
                // seeds the histogram with the AP channels — but the radio
                // is brought up in ACTIVE (Phase 1), NOT here: the Wi-Fi
                // STA init is a current spike and must not land on the
                // ARMING countdown (the battery-freeze point).
            } else {
                for (int i = 0; i < 3; ++i)
                    if (jam_sched.has_channel(i) && jam_carrier_start(i)) ++up;
            }
            // MAP: no carrier up yet (mapping phase) — that is expected, not
            // a failure. Only a non-MAP profile with 0 radios is a failure.
            if (up == 0 && jam_profile != (int)jam::Profile::MAP) {
                Serial.println("JAM START FAILED: no radio accepted the carrier");
                jam_state = JamState::DONE;
                led::set(LedState::SAFE);
                break;
            }
            // MAP: the mapping window is FREE time — start the timer
            // kMapMs into the future so the attack cap runs in full after
            // the map, not counted against it. Other profiles start now.
            if (jam_profile == (int)jam::Profile::MAP)
                jam_timer.start(now + kMapMs, jam_cap_ms);
            else
                jam_timer.start(now, jam_cap_ms);
            jam_state = JamState::ACTIVE;
            jam_last_rem_s = 0xFFFFFFFF;   // force the first ACTIVE repaint
            jam_wdt_arm();            // safety net: if this loop hangs, the ISR cuts the carrier
            led::set(LedState::ACTIVE);
            Serial.printf("JAM START profile=%s radios=%d cap=%lu ms\n",
                          jam_profile_name(), up, (unsigned long)jam_cap_ms);
            break;
        }
        // Throttle like ACTIVE: the countdown changes 1/s — a full 1024-byte
        // I2C flush per loop iteration is needless load (and a wedge risk on
        // a marginal battery), the same bug fixed in ACTIVE.
        // Throttle on the bar fill (changes ~4×/s as it drains) — a full
        // 1024-byte I2C flush per loop iteration is the wedge-risk bug, but
        // 4 Hz is fine (the mock drains smoothly).
        uint32_t rem_ms = (now < jam_arm_until) ? (jam_arm_until - now) : 0;
        int fill = (int)((120 - 2) * rem_ms / kJamArmMs);
        static int last_fill = -1;
        if (fill != last_fill) {
            last_fill = fill;
            char b[24];
            display::clear();
            display::center(8, "ARMING ...");
            display::center(22, jam_profile_name());
            // Draining bar (mock 3.3): the 3 s ARMING window as a bar that
            // empties — more visceral than the number alone.
            {
                const int bx = 4, by = 32, bw = 120, bh = 10;
                display::glyph(bx, by, bw, 1);
                display::glyph(bx, by + bh - 1, bw, 1);
                display::glyph(bx, by, 1, bh);
                display::glyph(bx + bw - 1, by, 1, bh);
                if (fill > 0) display::glyph(bx + 1, by + 1, fill, bh - 2);
            }
            snprintf(b, sizeof(b), "START IN %lu s", (unsigned long)(rem_ms / 1000));
            display::center(48, b);
            display::center(58, "HOLD SELECT CANCEL");
            display::flush();
        }
        break;
    }

    case JamState::ACTIVE:
        if (ev.valid() && ev.ev == BtnEvent::LONG) {
            jam_stop("manual-stop");
            break;
        }
        jam_wdt_feed();   // the loop is alive — keep the watchdog quiet
        if (jam_wdt_fired) {
            // The loop was blocked long enough that the watchdog already cut
            // the carrier. The loop has recovered: stop the session (radios
            // are already off) instead of hopping into the void.
            jam_stop("wdt-recovery");
            break;
        }
        if (jam_profile == (int)jam::Profile::MAP) {
            if (!map_started) {
                // Phase 1 (first kMapMs): passive mapping — radio A scans,
                // NO TX at all (B/C powered down), the histogram learns the
                // victim's hopping subset.
                // Wi-Fi seed: bring the ESP32's passive scan up NOW (the
                // mapping phase has no carrier, so the Wi-Fi STA current
                // spike is not on top of the 3-radio TX load). It is
                // powered back down when Phase 2 starts, before the jam.
                if (!map_wifi_up) {
                    if (wifi_ble::init()) {
                        wifi_ble::start_scan(0);
                        map_wifi_up = true;
                    }
                }
                if (track_have_scanner && now >= track_scan_next_ms) {
                    if (radios::scan_channel(0, track_scan_ch))
                        track_hist.v[track_scan_ch]++;
                    track_scan_ch++;
                    if (track_scan_ch > kTrackHi[track_band]) {
                        track_scan_ch = kTrackLo[track_band];
                        track_hist.decay();
                    }
                    track_scan_next_ms = now + 10;   // ~0.8 s/pass
                }
                // Wi-Fi seed: the RPD can't see OFDM — the ESP32's passive
                // scan finds the room's APs. Pin their (band-filtered)
                // channels just above the floor so they join the subset and
                // show up in the histogram. Rescan every pass (~4 s).
                if (wifi_ble::settle()) {
                    bool seen[81] = {false};
                    int seeded = 0;
                    const wifi_ble::ScanResult *sr = wifi_ble::result();
                    for (int i = 0; i < sr->count; ++i) {
                        int c = jam::wifi_to_nrf(sr->nets[i].chan);
                        if (c < kTrackLo[track_band] || c > kTrackHi[track_band]) continue;
                        // Pin well above the floor: the per-pass decay
                        // (x0.88) erodes the seed between rescans — 10
                        // decayed to 7 (< floor 8) in ~20 s. 20 survives
                        // 3 decays (13.6) and stays above the floor.
                        if (track_hist.v[c] < 20) track_hist.v[c] = 20;
                        if (!seen[c]) { seen[c] = true; ++seeded; }
                    }
                    map_wifi_n = seeded;
                    wifi_ble::start_scan(0);
                }
                if (now >= map_start_ms + kMapMs) {
                    // Phase 2: jam ONLY the discovered subset with all 3
                    // radios. Fallback: full band (nothing above noise floor)
                    // or blind hop (no scanner radio).
                    if (map_wifi_up) {
                        wifi_ble::stop();   // Wi-Fi seed done: radio OFF before
                        map_wifi_up = false; // the 3-carrier TX load (battery)
                        Serial.println("JAM MAP: wifi seed done, STA OFF before TX");
                    }
                    size_t n = track_hist.select_channels(map_chans, 40,
                                                          kTrackLo[track_band],
                                                          kTrackHi[track_band],
                                                          kMapMin);
                    map_fallback = (n == 0);
                    if (n == 0) {
                        n = jam::profile_channels((jam::Profile)jam_profile, map_chans);
                    }
                    map_n = n;
                    if (track_have_scanner) jam_carrier_stop(0);   // A joins the jam
                    jam_sched.init(map_chans, n, 3, 120);
                    for (int i = 0; i < 3; ++i)
                        if (jam_sched.has_channel(i)) jam_carrier_start(i);
                    map_started = true;
                    Serial.printf("JAM MAP: subset=%d ch (band %d-%d) floor=%d wifi_seed=%d\n",
                                  (int)map_n, kTrackLo[track_band], kTrackHi[track_band],
                                  (int)kMapMin, (int)map_wifi_n);
                    // Diagnostic: what did the scanner actually see? The
                    // subset above is the RESULT (with full-band fallback);
                    // this is the RAW histogram — 0 total = RPD saw nothing,
                    // a few hot channels = a real emitter, everything ~equal
                    // = noise floor. (temp diag — remove once the RPD
                    // behavior is understood)
                    {
                        int total = 0, above = 0;
                        for (int c = kTrackLo[track_band]; c <= kTrackHi[track_band]; ++c) {
                            total += track_hist.v[c];
                            if (track_hist.v[c] > kMapMin - 1) ++above;
                        }
                        Serial.printf("MAP DIAG: total=%d above_floor=%d top:", total, above);
                        bool used[81] = {false};
                        for (int t = 0; t < 10; ++t) {
                            int best = -1; uint16_t bv = 0;
                            for (int c = kTrackLo[track_band]; c <= kTrackHi[track_band]; ++c)
                                if (!used[c] && track_hist.v[c] > bv) { bv = track_hist.v[c]; best = c; }
                            if (best < 0 || bv == 0) break;
                            used[best] = true;
                            Serial.printf(" %d=%u", best, (unsigned)bv);
                        }
                        Serial.println();
                    }
                }
            } else {
                jam_sched.tick(now);
                for (int i = 0; i < 3; ++i)
                    if (jam_sched.has_channel(i)) jam_carrier_hop(i, jam_sched.channel(i));
            }
        } else {
            jam_sched.tick(now);
            for (int i = 0; i < 3; ++i)
                if (jam_sched.has_channel(i)) jam_carrier_hop(i, jam_sched.channel(i));
        }
        if (jam_timer.should_stop(now)) {
            jam_stop("timeout");
            break;
        }
        if (jam_profile == (int)jam::Profile::MAP) jam_paint_map(now);
        else jam_paint_active(now);
        break;

    case JamState::DONE:
        // R = repeat the same attack (profile + cap + band), L = back to the
        // duration screen (the screen before arming), S = also back (legacy).
        if (ev.valid() && ev.btn == Btn::RIGHT && ev.ev == BtnEvent::PRESS) {
            jam_state = JamState::ARMING;
            jam_arm_until = now + kJamArmMs;
            jam_last_arm_s = 0xFFFFFFFF;
            led::set(LedState::ARMED);
            Serial.printf("JAM REPEAT profile=%s cap=%lu ms\n",
                          jam_profile_name(), (unsigned long)jam_cap_ms);
        }
        else if (ev.valid() && (ev.btn == Btn::LEFT || ev.btn == Btn::SELECT)
                 && ev.ev == BtnEvent::PRESS) {
            // Back to the duration screen (previous step), not the main menu.
            // Radios are already down (jam_stop) and the LED is IDLE.
            jam_state = JamState::DUR;
        } else {
            char b[24];
            display::clear();
            display::center(8, "SESSION STOPPED");
            if (jam_wdt_fired) {
                display::center(26, "WATCHDOG CUT TX");
                display::center(36, "LOOP WAS HUNG");
            } else {
                snprintf(b, sizeof(b), "ELAPSED %lus", (unsigned long)(jam_final_ms / 1000));
                display::center(26, b);
            }
            display::center(44, "RADIO POWERED DOWN");
            display::center(54, "L BACK - R REPEAT");
            display::flush();
        }
        break;
    }
}
#endif  // defined(ARDUINO)
