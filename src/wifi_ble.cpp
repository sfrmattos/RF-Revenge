#include "wifi_ble.h"
#include <algorithm>

namespace wifi_ble {
static ScanResult g_result;
static bool g_scanning = false;

bool init() {
    WiFi.mode(WIFI_STA);   // station mode, not connected: passive scan only
    WiFi.disconnect();
    return true;           // per-scan failures surface through scanComplete()
}

void start_scan(int chan) {
    // passive, 300 ms per channel: a typical AP beacon interval is ~102 ms,
    // so a 300 ms window catches 2-3 beacons; 100 ms caught 0-1 (the source
    // of phantom TARGET LOST on a single-channel re-read).
    WiFi.scanNetworks(true, false, true, 300, (uint8_t)chan);  // async, passive
    g_scanning = true;
}

bool scanning() { return g_scanning; }

// Call every loop. Returns true on the tick the last scan completed, after
// copying (and sorting) the result. Read before scanDelete: it frees the list.
bool settle() {
    if (!g_scanning) return false;
    int n = WiFi.scanComplete();   // >=0 done, -1 running, -2 failed
    if (n < 0) return false;
    g_scanning = false;

    int count = n < MAX_NETS ? n : MAX_NETS;
    for (int i = 0; i < count; ++i) {
        String ssid;
        uint8_t enc;
        int32_t rssi, chan;
        uint8_t *b;
        if (!WiFi.getNetworkInfo(i, ssid, enc, rssi, b, chan)) continue;
        snprintf(g_result.nets[i].bssid, sizeof(g_result.nets[i].bssid),
                 "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
        ssid.toCharArray(g_result.nets[i].ssid, sizeof(g_result.nets[i].ssid));
        g_result.nets[i].rssi = (int)rssi;
        g_result.nets[i].chan = (int)chan;
    }
    for (int i = 0; i < count; ++i)               // bubble strongest-first
        for (int j = i + 1; j < count; ++j)
            if (g_result.nets[j].rssi > g_result.nets[i].rssi)
                std::swap(g_result.nets[i], g_result.nets[j]);
    g_result.count = count;
    WiFi.scanDelete();
    return true;
}

const ScanResult *result() { return &g_result; }

void stop() {
    if (WiFi.getMode() != WIFI_OFF)
        WiFi.disconnect();   // disconnect() logs ESP_ERR_WIFI_NOT_INIT when already off
    WiFi.mode(WIFI_OFF);
    g_scanning = false;
}

}
