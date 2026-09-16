#pragma once
// nRF24 driver on the reference driver (nRF24/RF24), known-good on this board.
//
// Safety: every boot and every stop path drives CE low on all radios.
// In this POC build (LAB_TX_ENABLED=0) radios only enter RX standby;
// they are never switched to TX mode.
#include <Arduino.h>

namespace radios {
struct Health {
    bool spi_ok;    // SPI transaction answered
    bool present;   // chip ID matched (nRF24L01/P family)
};

void init();                    // pins, SPI, CE low for all
Health probe(int i);            // 0=A 1=B 2=C; always ends with CE low
bool power_down_all();          // CE low for all — the universal safe state
bool start_session(int i);      // power up radio i (RX standby)
bool start_rx(int i);           // true RX mode (PRIM_RX + CE high) — RPD scanner
bool stop_session();            // power down all, returns false if any refused
void set_channel(int i, uint8_t ch);  // hop within an active session
// Constant-carrier (jam, phase 8): DC carrier occupying one channel.
// carrier_on requires the radio already powered (start_session).
bool carrier_on(int i, uint8_t pa_level, uint8_t ch);
void carrier_hop(int i, uint8_t ch);  // re-lock the carrier on a new channel
void carrier_off(int i);       // stopConstCarrier + powerDown (CE low)
// RPD scan (TRACK mode): radio must be powered (start_session, RX standby).
// Sets the channel and returns the RPD bit (signal >= -64 dBm on channel).
bool scan_channel(int i, uint8_t ch);
// Raw RSSI register (0x0F) on channel ch after the 20 ms settle window.
// Returns the RAW 6-bit value: 64 (0x40) = no signal detected, LOWER =
// stronger (the register is inverted). NOT gated on the RPD bit — that is
// the point: it shows graded signal strength the binary RPD would miss.
// (Cheap nRF24 clones sometimes report a dead/stuck RSSI register — the
// sweep output is what tells us which case we have.)
int raw_rssi(int i, uint8_t ch);
}
