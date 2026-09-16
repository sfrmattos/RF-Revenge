// Host unit tests for the pure logic: scheduler (channel subsets + one
// channel at a time per radio + hop timing), session timer bounds, and
// button debounce (settle, long-press, no spurious release after long).
//
// Run:  pio test -e host-tests
// (or standalone: g++ -std=c++17 -I src test_host.cpp src/scheduler.h ...)

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include "scheduler.h"
#include "debounce.h"
#include "proximity.h"
#include "deauth.h"
#include "jam.h"

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { ++g_fail; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static void sizes_ok(const Scheduler *s) {
    CHECK(s->sizes[0] == 2);
    CHECK(s->sizes[1] == 2);
    CHECK(s->sizes[2] == 2);
}

static void test_split_disjoint_equal() {
    uint8_t ch[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t subsets[3][80];
    size_t sizes[3];
    split_channels(ch, 8, 3, subsets, sizes);
    // 8 channels / 3 radios: sizes 3,3,2
    CHECK(sizes[0] == 3);
    CHECK(sizes[1] == 3);
    CHECK(sizes[2] == 2);
    // disjoint: no channel appears in more than one subset
    for (int a = 0; a < 3; ++a)
        for (size_t ia = 0; ia < sizes[a]; ++ia)
            for (int b = a + 1; b < 3; ++b)
                for (size_t ib = 0; ib < sizes[b]; ++ib)
                    CHECK(subsets[a][ia] != subsets[b][ib]);
    // round-robin order: ch[i] in subset (i%3)
    CHECK(subsets[0][0] == 1);
    CHECK(subsets[1][0] == 2);
    CHECK(subsets[2][0] == 3);
    CHECK(subsets[0][1] == 4);
}

static void test_scheduler_one_channel_at_a_time() {
    uint8_t ch[6] = {2, 20, 40, 60, 76, 80};
    Scheduler s;
    s.init(ch, 6, 3, 100);  // 2 channels per radio, hop every 100 ms
    CHECK(s.has_channel(0));
    CHECK(s.has_channel(1));
    CHECK(s.has_channel(2));
    sizes_ok(&s);
    for (uint32_t t = 0; t <= 1000; t += 10) {
        s.tick(t);
        // each radio always holds exactly one valid channel from its subset
        for (int i = 0; i < 3; ++i) {
            bool found = false;
            for (size_t k = 0; k < s.sizes[i]; ++k)
                if (s.subsets[i][k] == s.channel(i)) found = true;
            CHECK(found);
        }
    }
    // all channels of a subset are revisited over time. Use a FRESH scheduler
    // (the one above already advanced past this window). Mark by INDEX into
    // the subset (cur[i] is that index), then assert every index was hit.
    Scheduler s2;
    s2.init(ch, 6, 3, 100);
    bool seen[3][16] = {};
    for (uint32_t t = 0; t < 500; t += 10) {
        s2.tick(t);
        for (int i = 0; i < 3; ++i) seen[i][s2.cur[i]] = true;
    }
    for (int i = 0; i < 3; ++i)
        for (size_t k = 0; k < s2.sizes[i]; ++k)
            CHECK(seen[i][k]);
}

static void test_single_radio_full_list() {
    uint8_t ch[6] = {2, 20, 40, 60, 76, 80};
    Scheduler s;
    s.init(ch, 6, 1, 50);   // one radio takes ALL six channels
    CHECK(s.sizes[0] == 6);
    CHECK(!s.has_channel(1));
    CHECK(!s.has_channel(2));
}

static void test_session_timer_bounds() {
    SessionTimer t;
    t.start(1000, 60000);
    CHECK(t.active(1000));
    CHECK(t.active(60999));
    CHECK(!t.active(61000));
    CHECK(t.expired(61000));
    CHECK(!t.should_stop(1001));
    CHECK(t.should_stop(61000));
    CHECK(t.remaining(1000) == 60000);
    CHECK(t.remaining(60000) == 1000);   // deadline 61000 - now 60000
    CHECK(t.remaining(61000) == 0);
    CHECK(t.elapsed(61000) == 60000);
    // manual stop flag wins even while active
    t.stop_requested = true;
    CHECK(t.should_stop(2000));
}

static void test_debounce_settle_and_edges() {
    Debounce d;
    bool raw[3] = {false, false, false};
    uint32_t t = 0;
    // no event while idle
    for (int i = 0; i < 10; ++i) { CHECK(d.update(t += 20, raw).ev == DebEvent::NONE); }
    // Press LEFT with a 20 ms bounce; both the press AND the release must
    // clear the 30 ms settle before they are accepted.
    raw[0] = true;
    DebOut o = d.update(t += 20, raw);   // raw changed -> since_change reset
    CHECK(o.ev == DebEvent::NONE);
    raw[0] = false;                      // bounce
    o = d.update(t += 20, raw);
    CHECK(o.ev == DebEvent::NONE);
    raw[0] = true;                       // stable press begins
    o = d.update(t += 20, raw);          // call0 (raw changed)
    CHECK(o.ev == DebEvent::NONE);
    o = d.update(t += 20, raw);          // call1
    CHECK(o.ev == DebEvent::NONE);
    o = d.update(t += 20, raw);          // call2 = PRESS (3rd call after change)
    CHECK(o.ev == DebEvent::PRESS);
    CHECK(d.pressed(DebBtn::LEFT));
    // hold, then release (release also needs a settle window)
    o = d.update(t += 500, raw);
    CHECK(o.ev == DebEvent::NONE);
    raw[0] = false;
    o = d.update(t += 20, raw);          // call0 (raw changed)
    CHECK(o.ev == DebEvent::NONE);
    o = d.update(t += 20, raw);          // call1
    CHECK(o.ev == DebEvent::NONE);
    o = d.update(t += 20, raw);          // call2 = RELEASE (3rd call after change)
    CHECK(o.ev == DebEvent::RELEASE);
    CHECK(!d.pressed(DebBtn::LEFT));
}

static void test_debounce_long_press_select() {
    Debounce d;
    bool raw[3] = {false, false, false};
    uint32_t t = 0;
    raw[2] = true;  // press SELECT
    d.update(t += 20, raw);            // call0 (raw changed)
    DebOut o = d.update(t += 20, raw); // call1
    CHECK(o.ev == DebEvent::NONE);
    o = d.update(t += 20, raw);        // call2 = PRESS (3rd call after change)
    CHECK(o.ev == DebEvent::PRESS);
    // hold until long threshold (settle 30 + long 600)
    DebEvent last = DebEvent::NONE;
    for (int i = 0; i < 40; ++i) {
        o = d.update(t += 20, raw);
        if (o.ev != DebEvent::NONE) last = o.ev;
    }
    CHECK(last == DebEvent::LONG);     // LONG fired within the hold
    // release after long: no spurious RELEASE for the consumed press
    raw[2] = false;
    DebEvent post = DebEvent::NONE;
    for (int i = 0; i < 10; ++i) {
        o = d.update(t += 20, raw);
        if (o.ev != DebEvent::NONE) post = o.ev;
    }
    CHECK(post == DebEvent::NONE);     // long press consumed the press cycle
}

static void test_proximity_trend_and_stability() {
    RssiSmoothen sm;
    // stationary: constant signal => STABLE, no flips
    for (int i = 0; i < 20; ++i) CHECK(sm.update(-60.0f) == 0);
    // small wiggles inside the per-sample deadband => still STABLE
    float wig[6] = {-60, -59.5, -60.5, -59.8, -60.2, -60};
    for (float w : wig) CHECK(sm.update(w) == 0);

    // sustained move closer: -60 -> -50 => STRONGER while it moves,
    // then STABLE once it arrives at the new level (no oscillation).
    int saw_stronger = 0, saw_stable_after = 0;
    for (int i = 0; i < 12; ++i) {
        int8_t t = sm.update(-50.0f);
        if (t == 1) saw_stronger = 1;
        if (i >= 6 && t == 0) saw_stable_after = 1;
    }
    CHECK(saw_stronger);
    CHECK(saw_stable_after);
    for (int i = 0; i < 8; ++i) CHECK(sm.update(-50.0f) == 0);   // settled

    // sustained move away: -50 -> -70 => WEAKER while it moves, then STABLE.
    int saw_weaker = 0, saw_stable2 = 0;
    for (int i = 0; i < 12; ++i) {
        int8_t t = sm.update(-70.0f);
        if (t == -1) saw_weaker = 1;
        if (i >= 6 && t == 0) saw_stable2 = 1;
    }
    CHECK(saw_weaker);
    CHECK(saw_stable2);
    for (int i = 0; i < 8; ++i) CHECK(sm.update(-70.0f) == 0);   // settled

    // bars: 15 segments, monotonic in the window + clamped
    CHECK(rssi_bars(-110.0f) == 0);
    CHECK(rssi_bars(-100.0f) == 0);
    CHECK(rssi_bars(-40.0f)  == 15);
    CHECK(rssi_bars(-30.0f)  == 15);
    CHECK(rssi_bars(-70.0f)  > rssi_bars(-85.0f));
    CHECK(rssi_bars(-70.0f)  < rssi_bars(-55.0f));
    // ~2 dBm per segment: -85 is ~22% into the -100..-40 window => ~3 bars
    CHECK(rssi_bars(-85.0f) >= 2 && rssi_bars(-85.0f) <= 4);
    // reset returns to unprimed STABLE
    sm.reset();
    CHECK(!sm.primed);
    CHECK(sm.update(-55.0f) == 0);   // first sample is just priming

    // FADING STATIONARY: ±1.5 dB multipath wiggles around a constant mean.
    // Pre-0.3.3 (per-sample 0.8 dB deadband) this flickered STRONGER/WEAKER;
    // the windowed delta must stay STABLE. User-reported on 0.3.2: a
    // floor-separated AP showed trend flips while standing still.
    RssiSmoothen fade;
    for (int i = 0; i < 30; ++i)
        fade.update(-75.0f);                       // prime + settle
    for (int i = 0; i < 30; ++i) {
        float w = -75.0f + ((i % 4) < 2 ? 1.5f : -1.5f);
        CHECK(fade.update(w) == 0);                // never moves the trend
    }

    // LED pulse period: strong = fast, weak = slow, clamped to the
    // -100..-40 window (matches rssi_bars)
    CHECK(led_pulse_ms(-30.0f)  == 150);
    CHECK(led_pulse_ms(-40.0f)  == 150);
    CHECK(led_pulse_ms(-110.0f) == 2500);
    CHECK(led_pulse_ms(-100.0f) == 2500);
    CHECK(led_pulse_ms(-70.0f)  < led_pulse_ms(-85.0f));  // stronger -> faster
    CHECK(led_pulse_ms(-70.0f)  >= 150 && led_pulse_ms(-70.0f) <= 2500);
}

static void test_deauth_frame_and_mac() {
    uint8_t dst[6]  = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    uint8_t bssid[6] = {0xAA,0xBB,0xCC,0xDE,0xEF,0x11};
    uint8_t f[32];
    int len = deauth::build_deauth_frame(f, dst, bssid);
    CHECK(len == 26);
    // fc wire 0xC0 0x00: FC 0x00C0 = ver0, mgmt, subtype 12 = DEAUTH
    // (IEEE 802.11 table + aircrack-ng DEAUTH_REQ "\xC0\x00...")
    CHECK(f[0] == 0xC0 && f[1] == 0x00);      // mgmt / deauth
    CHECK(f[4] == 0xFF && f[9] == 0xFF);       // addr1 = broadcast
    CHECK(f[10] == 0xAA && f[15] == 0x11);     // addr2 = AP BSSID (SA)
    CHECK(f[16] == 0xAA && f[21] == 0x11);     // addr3 = AP BSSID
    CHECK(f[24] == 0x06 && f[25] == 0x00);     // reason 6

    // disassoc frame: fc 0x00A0 = ver0 mgmt subtype 10 (IEEE table), 24 bytes
    uint8_t g[32];
    int gln = deauth::build_disassoc_frame(g, dst, bssid);
    CHECK(gln == 24);
    CHECK(g[0] == 0xA0 && g[1] == 0x00);       // mgmt / disassoc
    CHECK(g[4] == 0xFF && g[9] == 0xFF);       // addr1 = broadcast
    CHECK(g[10] == 0xAA && g[15] == 0x11);     // addr2 = AP BSSID (SA)
    CHECK(g[16] == 0xAA && g[21] == 0x11);     // addr3 = AP BSSID
    CHECK(g[22] == 0x01 && g[23] == 0x00);     // reason 1

    uint8_t m[6];
    CHECK(deauth::parse_mac("AA:BB:CC:DE:EF:11", m));
    CHECK(m[0] == 0xAA && m[5] == 0x11);
    CHECK(!deauth::parse_mac("AA:BB:CC:DE:EF", m));       // short
    CHECK(!deauth::parse_mac("ZZ:BB:CC:DE:EF:11", m));    // non-hex
    CHECK(!deauth::parse_mac("AA BB:CC:DE:EF:11", m));    // bad sep
    CHECK(!deauth::parse_mac("AA:BB:CC:DE:EF:11:22", m)); // too long
}

static void test_jam_band_profiles() {
    uint8_t ch[80];
    // Full band: 2402-2480 MHz -> nRF24 ch 2-80 (79 channels, ascending)
    size_t n = jam::profile_channels(jam::Profile::BLE, ch);
    CHECK(n == 79);
    CHECK(ch[0] == 2 && ch[78] == 80);
    for (size_t i = 1; i < n; ++i) CHECK(ch[i] == ch[i - 1] + 1);
    // Wi-Fi band: ch1-13 (2412-2467 MHz) -> nRF24 ch 12-72 (61 channels)
    n = jam::profile_channels(jam::Profile::WIFI, ch);
    CHECK(n == 61);
    CHECK(ch[0] == 12 && ch[60] == 72);
    // Zigbee ch11-26 (2405-2480 MHz) -> nRF24 ch 5-80 (76 channels)
    n = jam::profile_channels(jam::Profile::ZIGBEE, ch);
    CHECK(n == 76);
    CHECK(ch[0] == 5 && ch[75] == 80);
    // DCE band (2423-2447 MHz) -> nRF24 ch 23-47 (25 channels)
    n = jam::profile_channels(jam::Profile::DCE, ch);
    CHECK(n == 25);
    CHECK(ch[0] == 23 && ch[24] == 47);
    // HID/RC/FPV/ALL = full band
    CHECK(jam::profile_channels(jam::Profile::HID, ch) == 79);
    CHECK(jam::profile_channels(jam::Profile::RC, ch) == 79);
    CHECK(jam::profile_channels(jam::Profile::FPV, ch) == 79);
    CHECK(jam::profile_channels(jam::Profile::ALL, ch) == 79);
    // Every profile: three-radio split disjoint, covers each channel exactly once
    for (size_t pi = 0; pi < jam::kProfileCount; ++pi) {
        jam::Profile p = (jam::Profile)pi;
        size_t m = jam::profile_channels(p, ch);
        uint8_t subsets[3][80];
        size_t sizes[3];
        split_channels(ch, m, 3, subsets, sizes);
        CHECK(sizes[0] == m / 3 + (m % 3 > 0));
        bool seen[81] = {false};
        for (int a = 0; a < 3; ++a)
            for (size_t ia = 0; ia < sizes[a]; ++ia) {
                uint8_t c = subsets[a][ia];
                CHECK(c >= 2 && c <= 80);
                CHECK(!seen[c]);   // disjoint across radios
                seen[c] = true;
            }
        size_t covered = 0;
        for (int c = 2; c <= 80; ++c) covered += seen[c] ? 1 : 0;
        CHECK(covered == m);       // every channel assigned to exactly one radio
    }
    // Hop periods: fast-FHSS victims (120 ms) vs fixed/slow-hop (400 ms)
    CHECK(jam::profile_hop_ms(jam::Profile::WIFI) == 120);
    CHECK(jam::profile_hop_ms(jam::Profile::HID) == 120);
    CHECK(jam::profile_hop_ms(jam::Profile::RC) == 120);
    CHECK(jam::profile_hop_ms(jam::Profile::ALL) == 120);
    CHECK(jam::profile_hop_ms(jam::Profile::BLE) == 400);
    CHECK(jam::profile_hop_ms(jam::Profile::ZIGBEE) == 400);
    CHECK(jam::profile_hop_ms(jam::Profile::DCE) == 400);
    CHECK(jam::profile_hop_ms(jam::Profile::FPV) == 400);
    // TRACK histogram: decay cools dead channels; top_k returns distinct
    // hottest channels above the noise floor.
    jam::TrackHist h;
    h.v[40] = 10; h.v[50] = 8; h.v[60] = 2;   // 60 is below the min=3 floor
    uint8_t top[2];
    size_t k = h.top_k(top, 2, 2, 80, 3);
    CHECK(k == 2 && top[0] == 40 && top[1] == 50);
    // Decay: a hot channel cools below the floor in a few passes; a channel
    // fed every pass stays hot.
    for (int i = 0; i < 10; ++i) { h.decay(); h.v[50] += 2; }
    k = h.top_k(top, 2, 2, 80, 3);
    CHECK(k == 1 && top[0] == 50);   // 40 decayed out, 50 kept alive
    // Band window: channels outside [lo,hi] are excluded (TRACK band menu).
    h.v[10] = 50; h.v[15] = 40;      // 10 is below the wifi window (12-72)
    k = h.top_k(top, 2, 12, 72, 3);
    CHECK(k == 2 && top[0] == 15 && top[1] == 50);
    // MAP: select_channels picks the victim's hopping subset (all channels
    // above the noise floor, descending by energy); empty histogram = 0
    // (the caller falls back to the full band).
    // wifi_to_nrf: 2407 + 5*ch == 2400 + nrf (MAP Wi-Fi seed)
    CHECK(jam::wifi_to_nrf(1) == 12);
    CHECK(jam::wifi_to_nrf(3) == 22);
    CHECK(jam::wifi_to_nrf(11) == 62);
    CHECK(jam::wifi_to_nrf(14) == 77);

    jam::TrackHist m;
    m.v[20] = 30; m.v[25] = 25; m.v[30] = 4; m.v[35] = 2;   // 35 below floor
    uint8_t sub[40];
    size_t subn = m.select_channels(sub, 40, 2, 80);
    CHECK(subn == 3 && sub[0] == 20 && sub[1] == 25 && sub[2] == 30);
    // Higher energy floor (kMapMin) = less sensitive: only the strong
    // channels survive; a channel detected in ~25% of passes is dropped.
    size_t subn2 = m.select_channels(sub, 40, 2, 80, 8);
    CHECK(subn2 == 2 && sub[0] == 20 && sub[1] == 25);
    jam::TrackHist empty;
    CHECK(empty.select_channels(sub, 40, 2, 80) == 0);
}

int main() {
    test_split_disjoint_equal();
    test_scheduler_one_channel_at_a_time();
    test_single_radio_full_list();
    test_session_timer_bounds();
    test_debounce_settle_and_edges();
    test_debounce_long_press_select();
    test_proximity_trend_and_stability();
    test_deauth_frame_and_mac();
    test_jam_band_profiles();
    if (g_fail) { printf("HOST TESTS FAILED: %d\n", g_fail); return 1; }
    printf("HOST TESTS OK\n");
    return 0;
}
