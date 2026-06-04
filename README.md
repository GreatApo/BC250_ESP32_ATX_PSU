# ESP32-BC250-LOP_PSU-PowerON-Xbox

This project turns the **ESP32_Relay X2** board into a smart PC power controller. It allows you to wake your computer (specifically AMD BC-250 systems running Linux/Bazzite) using an Xbox gamepad, while maintaining the gamepad's direct Bluetooth connection to the PC itself. 

It also synchronously supplies 12V power to peripherals (such as a Commander Duo fan controller) exactly when the motherboard wakes up.

## 🌟 Key Features
* **Passive BLE Scanning:** The ESP32 doesn't "hijack" the gamepad's connection. It simply listens to BLE broadcasts, waits for a known controller to turn on, and triggers the PC power relay. You can launch Forza Motorsport right away — the gamepad connects directly to your OS!
* **Sniper Pairing Mode:** The pairing mode ignores neighboring devices. It only pairs with gamepads brought point-blank to the antenna (closer than 30 cm, utilizing a -45 dBm RSSI threshold).
* **Hardware Blacklist:** Built-in protection against annoying smart TVs and rogue devices via MAC address filtering.
* **"Zombie-Wake" Protection:** After shutting down the PC via the OS menu, the ESP32 goes "deaf" for 60 seconds. This gives the gamepad time to power off naturally and prevents cyclic boot loops.
* **LED & Power Sync:** The physical button's LED and the 12V peripheral relay work in strict sync with the motherboard's actual power state.

## 🛠 Hardware Requirements
* **ESP32_Relay X2 (303E32DC210)** dual-relay board
* Motherboard (AMD BC-250 powered by LOP_PSU)
* Physical momentary push button with an LED indicator
* Xbox Series X/S Gamepad (must support the BLE protocol)

## 📌 Pinout & Wiring
| ESP32 Pin | Connection | Description |

| `GPIO 17` | Left Relay | Motherboard PWR_SW pins (PC Power Toggle) |
| `GPIO 16` | Right Relay | 12V line power toggle (Commander Duo / Peripherals) |
| `GPIO 23` | Orange Wire | Physical case button |
| `GPIO 19` | Green Wire | Button LED positive terminal |
| `GPIO 4`  | Red Wire | PC state monitor (e.g., from TPMS1 9-pin connector) |
| `GND`     | Black Wires | Common Ground (Button, LED, PC monitor) |

## 🎮 Button Controls
The physical button on your PC case handles three functions based on hold duration:
1. **Short press (under 5 sec):** Standard PC power toggle (duplicates the gamepad wake function).
2. **Hold for 5 seconds:** Enters **Pairing Mode**. The LED will blink rapidly. Bring your Xbox gamepad point-blank to the ESP32 and turn it on to save its MAC address.
3. **Hold for 10 seconds:** **Factory Reset**. Completely clears the saved gamepad memory. The LED will give 3 long blinks to confirm.

## 🚀 Installation & Flashing
1. Open the `.ino` sketch in the Arduino IDE.
2. Ensure you have the standard ESP32 core libraries installed.
3. *(Optional)* Add the MAC addresses of any rogue neighborhood devices to the `blacklistedMACs` array in the code.
4. To flash the board, place a physical jumper between the `IO0` and `GND` pins, connect via USB-TTL, select a baud rate of `115200`, and click "Upload". 
5. Remove the jumper and reboot the board after flashing.
