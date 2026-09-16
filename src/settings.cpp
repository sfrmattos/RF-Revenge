// Small NVS-persisted user settings.
#include "settings.h"
#include <Preferences.h>

static Preferences prefs;

namespace settings {

void begin() {
    if (!prefs.begin("rfclown", false)) {
        // NVS not available; defaults will be used. Not fatal.
    }
}

Data load() {
    Data d;
    d.led_enabled = prefs.getBool("led_en", true);
    d.led_brightness = (uint8_t)prefs.getUChar("led_br", 40);
    d.oled_contrast = (uint8_t)prefs.getUChar("oled_ct", 200);
    d.tx_pa_max = prefs.getBool("tx_pa", true);
    return d;
}

bool save(const Data& d) {
    return prefs.putBool("led_en", d.led_enabled) == 1 &&
           prefs.putUChar("led_br", d.led_brightness) == 1 &&
           prefs.putUChar("oled_ct", d.oled_contrast) == 1 &&
           prefs.putBool("tx_pa", d.tx_pa_max) == 1;
}

}
