// nRF24 driver on nRF24/RF24 (reference driver for this board).
#include "radios.h"
#include "pins.h"
#include <SPI.h>
#include <RF24.h>

static const uint8_t csn[3] = {RADIO_A_CSN, RADIO_B_CSN, RADIO_C_CSN};
static const uint8_t ce[3] = {RADIO_A_CE, RADIO_B_CE, RADIO_C_CE};

// read_register() is protected in RF24 — expose just the RSSI register
// (0x0F) for the graded-signal diagnostic.
struct Rf24Ext : RF24 {
    using RF24::RF24;
    uint8_t rssi_raw() { return read_register(0x0F) & 0x3F; }
};

static Rf24Ext* r[3];
static bool active[3] = {false, false, false};

namespace radios {

void init() {
    for (int i = 0; i < 3; ++i) {
        pinMode(csn[i], OUTPUT);
        pinMode(ce[i], OUTPUT);
        digitalWrite(ce[i], LOW);      // safety: no radio active at boot
        digitalWrite(csn[i], HIGH);    // deselected
        r[i] = new Rf24Ext(ce[i], csn[i]);
        r[i]->begin();                 // known-good on this board (default SPI)
    }
}

Health probe(int i) {
    Health h{false, false};
    if (i < 0 || i > 2) return h;
    h.spi_ok = r[i]->isChipConnected();  // SPI chip ID read-back
    h.present = h.spi_ok && r[i]->isValid();
    r[i]->powerDown();                  // never leave a probed radio enabled
    active[i] = false;
    return h;
}

bool power_down_all() {
    for (int i = 0; i < 3; ++i) {
        r[i]->powerDown();
        active[i] = false;
    }
    return true;
}

bool start_session(int i) {
    if (i < 0 || i > 2) return false;
    r[i]->powerUp();             // CE high -> RX standby (no TX in this POC)
    active[i] = true;
    return true;
}

bool start_rx(int i) {
    if (i < 0 || i > 2) return false;
    r[i]->startListening();   // PWR_UP + PRIM_RX + CE high — true RX mode
    active[i] = true;
    return true;
}

bool stop_session() {
    for (int i = 0; i < 3; ++i) {
        r[i]->powerDown();
        active[i] = false;
    }
    return true;
}

void set_channel(int i, uint8_t ch) {
    if (i < 0 || i > 2 || !active[i]) return;
    r[i]->setChannel(ch);
}

bool carrier_on(int i, uint8_t pa_level, uint8_t ch) {
    if (i < 0 || i > 2 || !active[i]) return false;
    r[i]->setChannel(ch);
    r[i]->startConstCarrier((rf24_pa_dbm_e)pa_level, ch);
    return true;
}

void carrier_hop(int i, uint8_t ch) {
    if (i < 0 || i > 2 || !active[i]) return;
    r[i]->setChannel(ch);   // CONT_WAVE stays on; PLL re-locks to the new channel
}

void carrier_off(int i) {
    if (i < 0 || i > 2) return;
    r[i]->stopConstCarrier();
    r[i]->powerDown();      // CE low — the universal safe state
    active[i] = false;
}

bool scan_channel(int i, uint8_t ch) {
    if (i < 0 || i > 2 || !active[i]) return false;
    // RPD is only valid in RX mode (start_rx) and LATCHES once high until
    // the RX buffer is flushed — so: channel, clear the latch, settle, read.
    // The settle window is the "catch window": a fast-hopping, low-duty
    // victim (e.g. a Logitech mouse: ~1 ms packets, ~10 ms hop, 1/K per
    // channel) is only in the window a fraction of the time. 2 ms caught
    // almost nothing (observed: empty histogram, MAP fell to full-band
    // fallback, TRACK stuck on the 2/3 seed). 20 ms catches >=1 packet per
    // pass for a K<=~40 subset. Longer = better detection but a slower full
    // pass (79 ch x 20 ms ~= 1.6 s).
    r[i]->setChannel(ch);
    r[i]->flush_rx();
    delayMicroseconds(20000);
    return r[i]->testRPD(); // RPD: signal >= -64 dBm on this channel
}

int raw_rssi(int i, uint8_t ch) {
    if (i < 0 || i > 2 || !active[i]) return 0;
    r[i]->setChannel(ch);
    r[i]->flush_rx();
    delayMicroseconds(20000);
    // RSSI register 0x0F: raw 6-bit, inverted (64 = no signal, lower =
    // stronger). Read directly — NOT gated on the RPD bit, so it reveals
    // graded signal strength the binary RPD would miss (e.g. OFDM Wi-Fi,
    // which the GFSK RPD may not latch as a "packet").
    return r[i]->rssi_raw();
}

}
