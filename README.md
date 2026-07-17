# BC250 Power Controller (ESP32 ATX PSU)

![BC250 ESP ATX PSU WebUI](img/webUI_PC.JPG "BC250 ESP ATX PSU WebUI")

An ESP32-based power controller (with 2 relays) for a BC-250 motherboard running and controlling a standard ATX power supply. It lets a saved BLE or Bluetooth gamepad, a physical button, or a local web page start the system. It also coordinates the ATX `PS_ON` signal with the motherboard power-button input and monitors the PC's real power state for safer startup and shutdown handling.

> [!WARNING]
> This project switches PSU and motherboard control signals. Incorrect wiring can damage the ESP32, motherboard, or PSU. Disconnect mains power before changing wiring, verify every connection with a multimeter, and never feed 5 V or 12 V into an ESP32 GPIO. ATX supplies can retain hazardous energy after being unplugged.

## Features

- Controls an ATX PSU on/off by grounding its green `PS_ON` wire through a relay.
- Pulses the BC-250 motherboard power-button input through a second relay.
- Provides physical-button power-on and normal-shutdown controls.
- Wakes the BC-250 when a registered controller is detected over BLE or Bluetooth Classic.
- Stores up to five controller MAC addresses in ESP32 non-volatile storage.
- Provides web-based power controls, live state, controller management, RSSI filtering, and a rolling event log.
- Saves router Wi-Fi settings from the web portal and applies them after a guarded controller restart.
- Discovers nearby controllers from a built-in web interface.
- Supports manual MAC-address registration when a controller is not discoverable.
- Connects to a configured 2.4 GHz Wi-Fi network and falls back to its own setup access point if the connection fails.
- Keeps the fallback AP active after a failed or dropped router connection; another router attempt is made only when settings are saved again or the ESP32 boots.

## How it works

The ESP32 remains powered from the ATX supply's always-on `+5VSB` rail. While the PC is off, it periodically scans for Bluetooth advertisements and inquiries from registered controllers.

When a known controller is detected, or power-on is requested from the button or web page, the controller:

1. Activates the PSU relay, connecting ATX `PS_ON` to ground.
2. Waits 1 second for the PSU rails to settle.
3. Activates the motherboard power-button relay for 500 ms.
4. Waits up to 15 seconds for the PC monitor input to go high.
5. Keeps `PS_ON` asserted while the PC is running.
6. Releases both relays if startup is not confirmed.

For a normal shutdown, it pulses the motherboard power-button input and leaves the PSU enabled while the operating system shuts down. Once the monitor input remains low and shutdown is confirmed, it releases `PS_ON`. Controller-triggered wake is ignored for 60 seconds after shutdown to avoid accidental wake loops.

## Hardware requirements

- An original ESP32 with Wi-Fi, BLE, and Bluetooth Classic support.
- A two-channel, ESP32-compatible relay board; the sketch is arranged for an active-high ESP32 Relay X2-style board.
- BC-250 motherboard and its power-button header/pads.
- Standard ATX PSU with accessible `+5VSB`, `PS_ON`, and ground connections.
- Momentary normally-open push button (PC Case button).
- LED with a suitable current-limiting resistor, unless the button assembly already includes one.
- Hook-up wire, insulated terminals/connectors, and suitable enclosure.
- Bluetooth controller whose address can be observed through BLE or Bluetooth Classic discovery (can also use [BluetoothViewer](https://www.nirsoft.net/utils/bluetooth_viewer.html)).

ESP32-C3 boards do not support Bluetooth Classic. Other ESP32 variants may also require changes to the Bluetooth APIs or pin assignments used by this sketch.

## Pin assignment & Wiring

![ESP32 to BC250 and ATX PSU connection](img/ESP32_to_BC250_ATX_PSU.JPG "ESP32 to BC250 and ATX PSU connection")

| ESP32 pin | Sketch name |  | Connects to
| --- | --- | --- | --- |
| `GPIO 17` | `RELAY_PWR_PIN` | `BC25_PW` | Relay contact across the BC-250 power-button pins |
| `GPIO 16` | `RELAY_PSU_PIN` | `PS_ON` | Relay contact between ATX green `PS_ON` and PSU ground |
| `GPIO 19` | `BUTTON_PIN` | `Case_Button` | Physical button to ground |
| `GPIO 23` | `LED_PIN` | — | Status/button LED through a suitable resistor |
| `GPIO 4` | `PC_MONITOR_PIN` | `TPMS1_pin_9` | 3.3 V PC-on indication |
| `GND` | — | — | Button, LED, monitor-signal, and relay-board logic ground |

The labels “left relay” and “right relay” in the sketch refer to the author's relay board layout. Trust the GPIO numbers and verify your own board's relay mapping rather than relying only on physical position.

### ESP32 standby power

The ESP32 and relay logic must remain powered when the main ATX rails are off:

| ATX connection | Controller connection |
| --- | --- |
| Purple `+5VSB` | Relay-board/ESP32 VCC or 5V supply input appropriate for the board |
| Black ground | Relay-board/ESP32 `GND` |

Check the exact relay board documentation before applying power. A terminal labelled `5V` may be an output on some integrated ESP32 relay boards rather than the intended supply input.

### ATX PSU relay

Use the normally-open contacts of the relay controlled by `GPIO 16`:

- Relay `COM` to the ATX green `PS_ON` wire.
- Relay `NO` to an ATX black ground wire (`GND`).
- Leave `NC` unused.

### Motherboard power-button relay

Use the normally-open contacts of the relay controlled by `GPIO 17`:

- Relay `COM` to one BC-250 power-button contact.
- Relay `NO` to the BC-250 TPMS1 pin 17 (`GND`).
- Leave `NC` unused.

### Physical button and LED

- Connect a momentary button between `GPIO 19` and `GND`.
- If you hav an external LED, Connect it to `GPIO 23` using the correct polarity and a current-limiting resistor.

### PC-state monitor

- Connect the BC250 3.3V “PC on” signal to `GPIO 4`.

## Configure the sketch

Open [`BC250_ESP32_ATX_PSU.ino`](BC250_ESP32_ATX_PSU.ino) and review the settings near the top before compiling.

### Wi-Fi and web portal

These values are the first-boot defaults. Saved values from the web portal take priority:

```cpp
const char* DEFAULT_WIFI_SSID = "YOUR_2_4_GHZ_WIFI";
const char* DEFAULT_WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* WEB_HOSTNAME = "bc250-controller";
const char* WIFI_AP_PASSWORD = "CHOOSE_AN_8_PLUS_CHARACTER_PASSWORD";
```

- The ESP32 Wi-Fi radio uses 2.4 GHz; a 5 GHz-only network will not work.
- Set `DEFAULT_WIFI_SSID` to an empty string to start with the local setup AP. Router credentials can then be entered in the portal.
- Set `WIFI_AP_PASSWORD` to an empty string for an open AP, or use at least eight characters for a protected AP.
- Router SSID and password changes made in the portal are stored in the ESP32 `Preferences` namespace `wifi_cfg` and survive restarts and power loss.
- The portal never returns the saved password. A blank password field preserves it; select **Open network** to erase it deliberately.
- The fallback AP name and password remain compile-time settings.

The web server is plain HTTP and has no application-level login. Anyone who can reach it can operate the power controls and modify the controller list. Use it only on a trusted network and protect the fallback AP with a strong password.

### Pins and relay polarity

Change the five pin constants if your hardware is wired differently. The supplied configuration expects active-high relay inputs:

```cpp
const int RELAY_ON = HIGH;
const int RELAY_OFF = LOW;
```

Some relay modules are active-low. Confirm the idle state before connecting the relay contacts to the PC or PSU, then reverse these constants if required. Both relays must be released at boot.

### Timing and scan settings

The main adjustable values are:

| Setting | Default | Purpose |
| --- | ---: | --- |
| `WAKE_COOLDOWN_MS` | 15 s | Suppresses repeated wake requests after detection |
| `SHUTDOWN_COOLDOWN_MS` | 60 s | Blocks controller wake immediately after shutdown |
| `WEB_SCAN_DURATION_MS` | 15 s | Length of a controller scan started in the portal |
| `POWER_OFF_HOLD_MS` | 3 s | Physical-button hold required for shutdown |
| `PSU_SETTLE_BEFORE_PWR_SW_MS` | 1 s | Delay between `PS_ON` and motherboard button pulse |
| `POWER_BUTTON_PRESS_MS` | 500 ms | Motherboard button pulse length |
| `STARTUP_CONFIRM_TIMEOUT_MS` | 15 s | Maximum time to detect a successful startup |
| `DEFAULT_WEB_SCAN_RSSI_THRESHOLD` | -55 dBm | Initial proximity filter for discovery |

## Compile and upload

If the ESP32 board has no onboard USB-to-serial interface, an Arduino Nano can be used as the programming adapter. See [Using an Arduino Nano as a USB-to-TTL adapter to program the ESP32](Arduino%20Nano%20as%20USB-to-TTL%20adapter%20to%20program%20ESP32.md) for the tested wiring, Arduino IDE procedure, bootloader-button sequence, and 3.3 V/5 V serial-level warning.

1. Open `BC250_ESP32_ATX_PSU.ino` in Arduino IDE
2. Select `board` > `Select other board and port` > `ESP32 Dev Module` and click `yes` in the notification to install
3. Select `Tools` > `Partition Scheme` > `No OTA (2MP APP/2MP SPIFFS)` (required due to the bigger program size)
3. Click the `verify` button (check icon) to test-compile the code
4. Go to `Tools` > `Port` and see what ports are shown up
5. Connect the ESP32 to your PC (i.e. ESP32 to the Arduino and then through USB)
6. Go to `Tools` > `Port` and select the new port that appeared
7. Click `upload` and wait for the compile to finish
8. When you see `Connecting` in the log, on ESP32 hold the `IOO` button, press the `EN` button once and then release the `IOO` button
9. It should start programming the ESP32

## First-time setup

1. Power the ESP32 from a current-limited USB supply and test the button, LED, monitor input, and relay outputs without the PSU/motherboard control contacts attached.
2. Confirm relay polarity: both relays must be off immediately after reset when the PC monitor input is low.
3. Power the controller from ATX `+5VSB` and ground, still with the control contacts disconnected.
4. Watch Serial Monitor at `115200` baud for the assigned LAN address.
5. If router Wi-Fi fails after about 20 seconds, join the access point named by `WEB_HOSTNAME` (default: `bc250-controller`) and open `http://192.168.4.1/`.
6. Enter the 2.4 GHz router SSID and password under **Wi-Fi & device**, then select **Save Wi-Fi**.
7. While the PC state is off, select **Restart controller** to apply the saved settings. Reconnect through the new LAN address shown in Serial Monitor.
8. Register a controller using the portal.
9. Switch off and disconnect mains power, then wire the two relay contact pairs and PC monitor input.
10. Recheck continuity, isolation, GPIO voltage, and relay idle states before reconnecting mains.
11. Test web or physical-button power-on first, then test controller wake.
12. Start a normal OS shutdown and confirm that `PS_ON` remains active until the monitor signal goes low.

## Web interface

<img src="img/webUI_mobile_AP.png" alt="BC250 ESP ATX PSU WebUI" width="300"/>

When connected to router Wi-Fi, open the IP address shown in Serial Monitor. Depending on your router and client, `http://bc250-controller.local/` may also work, but the sketch does not explicitly start an mDNS responder, so the numeric IP is the reliable option.

If router Wi-Fi is unavailable, join the fallback AP and browse to the AP address printed in the log, normally `http://192.168.4.1/`.

The portal provides:

- Current PC state and internal power-operation state.
- **Power on** and **Power off** controls.
- The five saved-controller slots with removal controls.
- A 15-second BLE/Bluetooth Classic scan for new devices.
- Adjustable discovery RSSI threshold from -100 to -20 dBm.
- Manual controller registration in `aa:bb:cc:dd:ee:ff` format.
- Persistent 2.4 GHz router SSID/password configuration.
- A guarded ESP32 restart button, enabled only while the PC is off and the power state machine is idle.
- The latest 40 in-memory log lines.

The page refreshes approximately every 1.5 seconds. Logs are held only in RAM and are cleared when the ESP32 restarts.

## Register a controller

### Scan and pair

1. Put the controller into its Bluetooth pairing/discoverable mode and keep it close to the ESP32.
2. Open the web portal and select **Scan**.
3. Wait for the controller to appear in the discovered list.
4. Select **Pair** next to the correct MAC address.
5. Shut the PC down, wait for the 60-second shutdown cooldown, and activate the controller.

The RSSI filter reduces the chance of registering a neighbor's device. A less-negative value is stricter: `-45 dBm` generally means very close, `-70 dBm` may cover a room, and `-90 dBm` can include devices through walls. Actual readings vary by antenna, enclosure, and environment.

### Add a MAC address manually

Enter a known Bluetooth MAC address in the portal using colon-separated hexadecimal notation. Manual registration is useful when a controller does not advertise long enough to appear in a scan.

Controller addresses are stored in the ESP32 `Preferences` namespace `xbox_cfg` and survive power loss and firmware restarts. Adding a sixth unique controller replaces a slot using the sketch's rotating slot pointer. Controllers can be removed individually from the web page; there is currently no physical-button pairing or factory-reset gesture.

Some devices use private or rotating BLE addresses. Those devices may not wake reliably by a saved MAC address. Xbox controller behavior can also vary with controller model and firmware.

## Physical button and LED

| Input | Result |
| --- | --- |
| Short press while PC is off | Starts the ATX power-on sequence |
| Hold for at least 3 seconds while PC is on | Pulses the motherboard button for a normal shutdown |

The sketch performs two short LED blinks when accepting a short press or the long-press threshold. The LED toggles every 250 ms continuously; it is therefore best treated as an activity/heartbeat indicator rather than a direct PC-power indicator.

## Default behavior and safeguards

- PC-monitor changes must remain stable for 100 ms before they are accepted.
- Controller wake scanning runs only while the PC is off and at least one controller is saved.
- Normal controller wake is blocked for 60 seconds after the PC turns off.
- If PC-on confirmation is missing after 15 seconds, both relays are released.

## HTTP API

The web UI uses a small unauthenticated HTTP API. It can also be called by trusted local automation:

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/api/status` | PC, Wi-Fi, scan, and power-state status |
| `GET` | `/api/controllers` | Saved controller list |
| `GET` | `/api/found` | Current discovery results |
| `GET` | `/api/logs` | Rolling runtime log |
| `POST` | `/api/power/on` | Request power-on |
| `POST` | `/api/power/off` | Request normal shutdown |
| `POST` | `/api/scan/start` | Start controller discovery |
| `POST` | `/api/pair?mac=aa%3Abb%3Acc%3Add%3Aee%3Aff` | Save a controller from current results |
| `POST` | `/api/manual-add?mac=aa%3Abb%3Acc%3Add%3Aee%3Aff` | Save a controller manually |
| `POST` | `/api/remove?slot=0` | Remove a saved slot; slots are zero-based |
| `POST` | `/api/rssi?value=-55` | Save discovery RSSI threshold |
| `POST` | `/api/wifi/save?ssid=NAME&password=SECRET&clearPassword=0` | Save router Wi-Fi settings and make one connection attempt; blank password preserves the current one |
| `POST` | `/api/restart` | Restart the ESP32; rejected while the PC is on or power control is busy |

There is no authentication, TLS, CSRF protection, or network access control in the firmware. Do not expose port 80 to the internet.

## Troubleshooting

### The sketch does not compile

- Confirm an original ESP32 target is selected, not an AVR board or ESP32-C3.
- Use the BLE headers supplied with the Espressif ESP32 Arduino core; installing a second, conflicting BLE library can cause ambiguous or missing APIs.
- If `esp_gap_bt_api.h` or Classic Bluetooth symbols are unavailable, the selected ESP32 variant/core does not provide the required Classic Bluetooth stack.

### The web page is unavailable

- Open Serial Monitor at `115200` baud and use the printed numeric IP address.
- Confirm the configured network is 2.4 GHz and the SSID/password are correct.
- After the 20-second connection timeout, join the fallback AP and browse to `192.168.4.1`.
- Ensure the client is on the same LAN and that client/AP isolation is disabled.
- Do not rely on the `.local` hostname; this sketch does not initialize mDNS.

### A controller is not found

- Put it into Bluetooth pairing mode and move it close to the ESP32.
- Lower the RSSI threshold to a more negative value, such as `-70` or `-85 dBm`.
- Wait for the full 15-second scan because BLE and Classic inquiries share the radio.
- Read the controller MAC address elsewhere and add it manually.
- Check whether the device uses a rotating private address.

### A saved controller does not wake the PC

- Wait at least 60 seconds after shutdown.
- Confirm the saved MAC matches the address the controller currently advertises.
- Check the live log for “Known controller detected”.
- Verify that the PC monitor input is low while the PC is off; wake scanning is suppressed when it reads high.
- Confirm the PSU relay grounds `PS_ON`, then verify the motherboard relay closes after the 1-second delay.

## Limitations

- Controller compatibility depends on discoverable Bluetooth behavior and stable device addresses.
- The web UI and API are intended only for a trusted local network.
- The portal can save router credentials, but it is not a captive portal and cannot scan for nearby Wi-Fi networks.
- Changing the fallback AP name/password or hostname still requires editing and reflashing the firmware.
- Saved controllers are managed through the web page; there is no physical factory-reset control in this version.
