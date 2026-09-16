#pragma once
// Minimal Wi-Fi scan wrapper for the proximity meter (internal engineering notes phase 5).
// Station-mode passive scan only — never connects, never transmits a payload.
//
// ponytail: BLE is intentionally NOT built here. BLE random addresses and
// intermittent advertisements break stable target locking on this hardware
// (internal engineering notes lists that limitation); building it blind, without a device to
// validate the lock, would violate the evidence rule. Wi-Fi (stable BSSID)
// first, per the phase ordering.
#include <Arduino.h>
#include <WiFi.h>

namespace wifi_ble {
struct Net {
    char bssid[18];   // "AA:BB:CC:DD:EE:FF"
    char ssid[18];    // label (empty = hidden)
    int rssi;         // raw dBm
    int chan;
};
const int MAX_NETS = 32;   // driver can report more; mask (uint32_t) fits 32
struct ScanResult {
    int count;
    Net nets[MAX_NETS];   // sorted strongest-first
};

bool init();                   // false = Wi-Fi stack unusable
// chan == 0: full 13-channel discovery scan (~2-3 s).
// chan > 0: single-channel passive re-read of a known target (~0.2 s) —
//           drives the locked meter's refresh rate.
void start_scan(int chan);
bool scanning();               // a scan is in flight
bool settle();                 // true on the tick the last scan completed
const ScanResult *result();    // valid after settle() has fired
void stop();                   // radio off — the safe default outside the meter
}
