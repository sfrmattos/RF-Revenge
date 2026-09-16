# RF Revenge user manual

## Scope, origin, and credit

RF Revenge is independent firmware for the ESP32-based RF-Clown V2 device. I bought an RF-Clown 2.0 to explore RF gadgets. I considered the hardware good and wanted different firmware for my personal requirements. I wrote the firmware from scratch for the documented device, using original project material only as a hardware and pinout reference. After my own testing, I decided to share it with the community.

The hardware documentation credit belongs to [CiferTech's RF-Clown project](https://github.com/cifertech/RF-Clown). That project documents its BLE/Bluetooth-oriented project and hardware, Arduino and precompiled-binary flashing flow, and mode-indicating LED. RF Revenge documents separate implementation decisions: a PlatformIO build, host checks, explicit-port setup and flash, diagnostics, NVS-backed settings, passive Wi-Fi proximity, management-frame test controls, 2.4 GHz JAM controls, timed arming, bounded sessions, long-hold stop or cancel, external-radios-safe boot, and a JAM watchdog cutout. These differences are not a judgment of the original firmware's quality.

## Quick setup

Connect the supported device by USB and work from the repository root. Prepare dependencies, validate or apply the guarded local patch when necessary, build, and run the host checks:

```sh
./tools/setup.sh
```

This command does not flash a device by default. To flash, first identify and verify the device port, then supply that exact port explicitly:

```sh
./tools/setup.sh --flash /dev/ttyUSB0
```

The setup path prepares dependencies, performs its guarded patch handling as needed, builds the firmware, and runs host checks. For patch details, see [Patch reproduction](PATCH_REPRODUCTION.md).

## Hardware and build scope

The documented hardware is an ESP32-based RF-Clown V2 with an SSD1306-class OLED, one status LED, three buttons, and up to three nRF24 radios. Safe boot powers down external radios. The firmware main menu contains `2.4G Wi-Fi Proximity`, `Wi-Fi De-Auth`, `2.4G Jam`, `Settings`, `About`, and `Diagnostics`; De-Auth and Jam are compile-gated and enabled in the current documented build.

A completed setup, patch, build, host check, or flash establishes only that those local software steps completed. It does not establish RF behavior, permission, compatibility with other dependency versions, or suitability in every environment.

## Controls

| Control | Action |
|---|---|
| Left / Right | Navigate menus or change the visible value. |
| Select press | Enter, confirm, mark, or start, according to the active screen. |
| Select hold | Go back, cancel, save Settings, or stop an active session, according to the active screen. |

Use the on-screen labels as the immediate authority for the selected screen. When a control presents an arm warning or countdown, review it before proceeding; holding Select remains the stop or cancel path for an active session.

## Features and intended use

### 2.4G Wi-Fi Proximity

This is passive Wi-Fi discovery. Browse visible networks with Left and Right, then press Select to lock the selected BSSID; hold Select to leave the view. The display applies exponential RSSI smoothing and shows a 15-bar visual with a rising, falling, or stable trend. Treat the display as a proximity-oriented observation only: it is not a distance measurement and does not prove that a selected BSSID identifies a particular person or device.

### Diagnostics

Diagnostics reports the OLED, I2C, status LED, and detected nRF24-radio health. Use it after boot, after hardware changes, and before relying on a feature that uses an external radio. A diagnostic result reports what the firmware detects; it is not a guarantee of behavior outside the device.

### Settings

Hold Select on the Settings screen to save the LED enabled state, LED brightness, and TX-power selection in ESP32 NVS. The interface can signal an error if the save fails. These are the settings currently documented as persistent; do not assume other display values are stored.

### Wi-Fi De-Auth

`Wi-Fi De-Auth` is a Wi-Fi management-frame test control enabled by the current build. It derives selection data from a Wi-Fi scan, shows the selected channel and BSSID, then requires an explicit local arm warning and countdown before the selected session begins. Choose a duration from 30 seconds through 5 minutes. Hold Select to cancel during arming or stop an active session.

Use this control only after you have established permission for the equipment, network, site, spectrum, and activity. It does not promise that a client will disconnect or that any particular result will occur.

### 2.4 GHz JAM

The user interface names this control `2.4 GHz JAM`. Up to three nRF24 radios can operate through it. Before a session, the interface presents an explicit local arm warning and countdown; select a duration from 30 seconds through 5 minutes, and hold Select to cancel or stop.

The JAM path has a watchdog that cuts carrier/radio control when the main-loop heartbeat becomes stale. A session cannot start if that watchdog is unavailable. These controls bound the firmware session and provide a stop path; the controls do not establish or predict a physical RF effect.

### GFSK MAP & KILL

`GFSK MAP & KILL` is a MAP profile used before the normal JAM session transition. It first asks for a band, then opens a mapping window. During mapping, radio A scans while radios B and C are off. The map samples nRF24 RPD activity; when the Wi-Fi band is selected, it also passively seeds the histogram with an ESP32 Wi-Fi scan because RPD does not latch OFDM.

The interface displays map, histogram, and diagnostic status, then chooses a bounded subset of channels above a noise floor. Mapping completes before any normal JAM session transition, and that later transition remains subject to the explicit arm and safety controls described above. I found this channel-selection workflow personally useful and surprising in controlled testing. That observation is personal and limited; it makes no claim about performance, effectiveness, devices, protocols, or conditions on air.

## Safety operating flow

1. Establish permission for the equipment, spectrum, site, and planned activity before using a transmission-related control.
2. Check the device, power source, antennas, intended activity, and a safe recovery or stop method.
3. Boot the device, then use Diagnostics to review the OLED, I2C, LED, and detected radio status.
4. For observation, use Wi-Fi Proximity and interpret RSSI trends as observations rather than distance or identity evidence.
5. For a permitted management-frame or JAM test, confirm the selected data, read the local warning, choose the bounded duration, and remain able to hold Select to cancel or stop.
6. Stop immediately if operation differs from expectation. Keep testing isolated from third-party equipment and shared spectrum.

## What the checks prove and do not prove

The setup script's build and host checks are useful software checks. Those checks do not prove RF behavior, regulatory status, permission, compatibility with all dependency versions, or safety in every environment. Likewise, an OLED display, a diagnostic health report, an arm countdown, or a completed bounded session should not be read as a claim about effects on other equipment, networks, protocols, or the surrounding spectrum.

## Troubleshooting and limitations

- **Setup or build does not complete:** Run `./tools/setup.sh` from the repository root and consult [Patch reproduction](PATCH_REPRODUCTION.md) for the guarded patch process.
- **Flash is not intended:** Use the setup command without `--flash`; flashing is opt-in.
- **Flash target is uncertain:** Do not guess a port. Identify the USB device first, then pass its exact `/dev/...` path to `--flash`.
- **A feature is missing from the menu:** The De-Auth and Jam entries are compile-gated. This manual describes the current `rfclown_alt` build with both gates enabled.
- **A radio-related feature appears unavailable:** Review Diagnostics and the hardware connection. Safe boot powers down external radios, and the firmware only reports detected radio health.
- **A save signals an error:** The documented persisted settings are LED enabled state, LED brightness, and TX-power selection. Retry the save and treat an error indication as a failed save rather than assuming the value was retained.
- **A JAM session will not start:** The watchdog must be available; the firmware blocks starting when it is not. Do not bypass that condition.

See the [README](../README.md) for the project overview and [MIT License](../LICENSE) for licensing.
