#pragma once
// RF Revenge board pin map (documented hardware assumption, internal engineering notes).
// No runtime pin discovery: the target board is fixed for v0.1.

// --- Display --- (ESP32 default I2C: SDA=21, SCL=22; reference firmware
// uses U8g2 HW-I2C defaults without calling Wire.begin, same mapping)
#define PIN_SDA 21
#define PIN_SCL 22
#define OLED_I2C_ADDR 0x3C  // ponytail: single-const flip if this board's SSD1306 sits at 0x3D

// --- LED ---
#define PIN_NEOPIXEL 14     // single WS2812, GRB 800kHz, bit-banged

// --- Application buttons (V2 reference board: L=27 R=25 S=26) ---
#define PIN_BTN_LEFT 27
#define PIN_BTN_RIGHT 25
#define PIN_BTN_SELECT 26
// Buttons assumed wired to GND with internal pull-ups (idle HIGH, pressed LOW).
// If Diagnostics shows a button FAIL while it is physically pressable, flip this.
#define BUTTON_PRESSED_LEVEL LOW

// --- nRF24 radios, shared default SPI (MISO=12 MOSI=13 SCK=14) ---
// SCK=14 shares the physical line with the bit-banged NeoPixel. This is the
// known-good wiring of the reference board (original firmware runs both on
// 14); do not "fix" it without a real hardware problem.
#define RADIO_A_CE 5
#define RADIO_A_CSN 17
#define RADIO_B_CE 16
#define RADIO_B_CSN 4
#define RADIO_C_CE 15
#define RADIO_C_CSN 2
