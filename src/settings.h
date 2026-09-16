#pragma once
// Small NVS-persisted user settings (internal engineering notes: LED enable/brightness,
// display contrast). Nothing else is persisted.
#include <Arduino.h>

namespace settings {
struct Data {
    bool led_enabled = true;
    uint8_t led_brightness = 40;   // low default for battery; grid 10..100 step 10
    uint8_t oled_contrast = 200;   // 0..255
    bool tx_pa_max = true;         // global TX PA: nRF24 7 vs 3 dBm, Wi-Fi 20.75 vs 18 dBm
};

void begin();
Data load();
bool save(const Data& d);
}
