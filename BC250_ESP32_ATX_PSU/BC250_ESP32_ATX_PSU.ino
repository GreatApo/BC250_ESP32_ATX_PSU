#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <Preferences.h>
#include <WiFi.h>
#include <string.h>
#include "esp_gap_bt_api.h"
#include "esp_mac.h"

// ================= PIN SETTINGS =================
const int RELAY_PWR_PIN = 17;     // (Left relay) BC-250 power button pin + TPMS1 pin 17 (GND)
const int RELAY_PSU_PIN = 16;     // (Right relay) ATX PS_ON green wire to PSU GND
const int BUTTON_PIN = 19;        // Case power button
const int LED_PIN = 23;           // LED
const int PC_MONITOR_PIN = 4;     // BC250 TPMS1 pin 9 (3V - PC Monitor)

const int RELAY_ON = HIGH;
const int RELAY_OFF = LOW;

// ================= WIFI / WEB UI SETTINGS =================
// These are first-boot defaults. Values saved from the webpage take priority.
const char* DEFAULT_WIFI_SSID = "";
const char* DEFAULT_WIFI_PASSWORD = "";
const char* WEB_HOSTNAME = "bc250-controller";
const char* WIFI_AP_PASSWORD = "12345678"; // Leave empty for an open setup AP, or use 8+ chars.
const char* WIFI_CONFIG_NAMESPACE = "wifi_cfg";

String wifiSsid = DEFAULT_WIFI_SSID;
String wifiPassword = DEFAULT_WIFI_PASSWORD;
String classicBtSpoofMac = "";

WiFiServer server(80);

const int MAX_LOG_LINES = 40;
String logLines[MAX_LOG_LINES];
int logStart = 0;
int logCount = 0;

// ================= TIMING SETTINGS =================
const unsigned long WAKE_COOLDOWN_MS = 15000;           // Ignore controllers briefly after wakes up from BLE
const unsigned long SHUTDOWN_COOLDOWN_MS = 60000;       // Ignore controllers briefly after shutdown
const unsigned long WEB_SCAN_DURATION_MS = 15000;       // Stop webpage BLE scans after this long

const unsigned long POWER_OFF_HOLD_MS = 3000;           // Hold physical button this long to turn the PC off

const unsigned long CONFIRM_BLINK_MS = 100;             // LED on/off duration for 2-blink confirmations
const unsigned long POWER_OFF_PREVIEW_BLINK_MS = 250;   // Blink while holding long enough for power-off

const unsigned long SHUTDOWN_CONFIRM_DELAY_MS = 4000;   // Keep PS_ON on briefly after the PC monitor reports OFF
const unsigned long PC_STABLE_DELAY_MS = 100;           // Require PC monitor pin to stay unchanged before accepting it
const unsigned long PSU_SETTLE_BEFORE_PWR_SW_MS = 1000; // Wait after asserting ATX PS_ON before pressing PWR_SW
const unsigned long POWER_BUTTON_PRESS_MS = 500;        // How long to hold the motherboard power button relay
const unsigned long STARTUP_CONFIRM_TIMEOUT_MS = 15000; // Give the PC this long to report ON after wake
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;    // Try station WiFi this long before starting the fallback AP
const unsigned long BLE_SCAN_INTERVAL_MS = 2500;        // Leave WiFi airtime between blocking BLE scans
const unsigned long CLASSIC_SCAN_INTERVAL_MS = 10000;   // Bluetooth Classic inquiry is longer and heavier than BLE scan
const uint8_t CLASSIC_INQUIRY_DURATION = 4;             // Inquiry duration in 1.28s units (4 = about 5.1s)
const int DEFAULT_WEB_SCAN_RSSI_THRESHOLD = -55;        // Filter out weak BLE signals during web scans (-45 = close, -70 = room, -90 = wall)

// ================= MEMORY FOR 5 GAMEPADS =================
const char* CONFIG_NAMESPACE = "xbox_cfg";
const int MAX_CONTROLLERS = 5;
const int MAX_FOUND_CONTROLLERS = 10;
String savedMACs[MAX_CONTROLLERS];
int currentSlot = 0;

struct FoundController {
    String mac;
    String name;
    int rssi;
};

FoundController foundControllers[MAX_FOUND_CONTROLLERS];
int foundControllerCount = 0;

Preferences preferences;
bool webScanActive = false;
BLEScan* bleScan = nullptr;
int webScanRssiThreshold = DEFAULT_WEB_SCAN_RSSI_THRESHOLD;
bool bleScanRunning = false;
bool shutdownPress = false;

unsigned long buttonPressStartTime = 0;
unsigned long lastWakeTime = 0; // Time PC was last woken up by a known controller
unsigned long lastBleScanTime = 0;
unsigned long lastClassicScanTime = 0;
bool classicDiscoveryRunning = false;
bool classicDiscoveryCancelRequested = false;
bool classicBtSpoofEnabled = false;
bool classicPageScanTargetInitialized = false;
bool classicPageScanTargetEnabled = false;

bool setupApRunning = false;
bool routerWifiWasConnected = false;
bool routerWifiRetrying = false;
unsigned long wifiRetryStartedAt = 0;
bool wifiConnectRequested = false;
bool restartRequested = false;
unsigned long restartRequestedAt = 0;

// --- LED BLINK TIMERS ---
unsigned long lastBlinkTime = 0;
bool ledBlinkState = LOW;

// --- SHUTDOWN PROTECTION COOLDOWN ---
unsigned long lastShutdownTime = 0; // Time when PC was last detected as OFF
bool lastPcState = false;

// --- WEB SCAN MAX DURATION ---
unsigned long webScanStartTime = 0;

// --- PC MONITOR DEBOUNCE ---
struct DebouncedInput {
    bool raw;
    bool stable;
    unsigned long changedAt;
};

DebouncedInput pcMonitor = { false, false, 0 };
bool stablePcOn = false;

// --- NON-BLOCKING ATX POWER STATE MACHINE ---
enum PowerState {
    POWER_IDLE,
    POWER_ON_WAIT_FOR_PSU,
    POWER_ON_PRESS_BUTTON,
    POWER_ON_CONFIRM,
    POWER_OFF_PRESS_BUTTON,
    POWER_OFF_WAIT_FOR_OFF
};

PowerState powerState = POWER_IDLE;
unsigned long powerStateStartedAt = 0;

void addLog(const String& message) {
    String line = String(millis()) + "ms - " + message;

    if (logCount < MAX_LOG_LINES) {
        logLines[(logStart + logCount) % MAX_LOG_LINES] = line;
        logCount++;
    } else {
        logLines[logStart] = line;
        logStart = (logStart + 1) % MAX_LOG_LINES;
    }

    Serial.println(line);
}

bool debounceInput(DebouncedInput &input, bool rawState) {
    // Track raw edge changes first; only promote them to "stable" after the
    // input has stopped changing for PC_STABLE_DELAY_MS.
    if (rawState != input.raw) {
        input.raw = rawState;
        input.changedAt = millis();
    }

    if (millis() - input.changedAt >= PC_STABLE_DELAY_MS) {
        input.stable = rawState;
    }

    return input.stable;
}

void updatePcState() {
    bool newPcState = debounceInput(pcMonitor, digitalRead(PC_MONITOR_PIN) == HIGH);

    if (newPcState != stablePcOn) {
        stablePcOn = newPcState;
        addLog(String("STATE - PC ") + (stablePcOn ? "ON" : "OFF"));
    }
}

bool buttonIsPressed() {
    // Check button 5 times in a row to avoid false positives from bouncing.
    for (int i = 0; i < 5; i++) {
        if (digitalRead(BUTTON_PIN) == HIGH) {
            return false;
        }
        delay(1);
    }
    return true;
}

void startAtxPowerOnSequence() {
    // Only one relay sequence should run at a time. This keeps repeated BLE
    // adverts or button presses from stacking extra power-button pulses.
    if (stablePcOn) {
        addLog("STATE - PC already on.");
        return;
    }

    if (powerState != POWER_IDLE) {
        addLog("STATE - Power operation already in progress (power-on aborted).");
        return;
    }

    addLog("STATE - Setting PSU ON");
    digitalWrite(RELAY_PSU_PIN, RELAY_ON);
    powerState = POWER_ON_WAIT_FOR_PSU;
    powerStateStartedAt = millis();
}

void startNormalShutdown() {
    if (!stablePcOn) {
        addLog("STATE - PC already off.");
        return;
    }

    if (powerState != POWER_IDLE) {
        addLog("STATE - Power operation already in progress (shutdown aborted).");
        return;
    }

    addLog("STATE - Pulsing motherboard power button for normal shutdown...");
    // Arm the controller wake lockout immediately. Waiting until the monitor
    // pin reports OFF leaves a window where controller adverts can restart the PC.
    lastShutdownTime = millis();
    if (lastShutdownTime == 0) lastShutdownTime = 1;
    digitalWrite(RELAY_PWR_PIN, RELAY_ON);
    powerState = POWER_OFF_PRESS_BUTTON;
    powerStateStartedAt = millis();
}

void handlePowerState() {
    unsigned long now = millis();

    switch (powerState) {
        case POWER_ON_WAIT_FOR_PSU:
            // Give the ATX PSU a moment after PS_ON before touching PWR_SW.
            if (now - powerStateStartedAt >= PSU_SETTLE_BEFORE_PWR_SW_MS) {
                digitalWrite(RELAY_PWR_PIN, RELAY_ON);
                powerState = POWER_ON_PRESS_BUTTON;
                powerStateStartedAt = now;
            }
            break;

        case POWER_ON_PRESS_BUTTON:
            // Release the motherboard power button relay after a short press.
            if (now - powerStateStartedAt >= POWER_BUTTON_PRESS_MS) {
                digitalWrite(RELAY_PWR_PIN, RELAY_OFF);
                powerState = POWER_ON_CONFIRM;
                powerStateStartedAt = now;
            }
            break;

        case POWER_ON_CONFIRM:
            // Once the monitor pin reports ON, leave PS_ON asserted and return
            // to idle. If it never comes up, fail safe by releasing both relays.
            if (stablePcOn) {
                addLog("STATE - PC power-on confirmed - PC is now running.");
                powerState = POWER_IDLE;
            } else if (now - powerStateStartedAt >= STARTUP_CONFIRM_TIMEOUT_MS) {
                addLog("STATE - WARNING: PC did not power on! Turning ATX PS_ON relay OFF.");
                digitalWrite(RELAY_PWR_PIN, RELAY_OFF);
                digitalWrite(RELAY_PSU_PIN, RELAY_OFF);
                powerState = POWER_IDLE;
            }
            break;

        case POWER_OFF_PRESS_BUTTON:
            // Normal shutdown is just a short motherboard power-button press.
            if (now - powerStateStartedAt >= POWER_BUTTON_PRESS_MS) {
                digitalWrite(RELAY_PWR_PIN, RELAY_OFF);
                powerState = POWER_OFF_WAIT_FOR_OFF;
                powerStateStartedAt = now;
            }
            break;

        case POWER_OFF_WAIT_FOR_OFF:
            // Keep PS_ON asserted while the OS shuts down, then release it after
            // the monitor pin has stayed off long enough to be believable.
            if (!stablePcOn) {
                if (now - powerStateStartedAt >= SHUTDOWN_CONFIRM_DELAY_MS) {
                    digitalWrite(RELAY_PSU_PIN, RELAY_OFF);
                    powerState = POWER_IDLE;
                }
            } else {
                powerStateStartedAt = now;
            }
            break;

        default:
            break;
    }
}

bool isKnownController(const String& deviceMAC) {
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
        if (savedMACs[i].length() == 0) continue; // Skip empty slots
        if (deviceMAC.equalsIgnoreCase(savedMACs[i])) return true;
    }
    return false;
}

bool hasSavedController() {
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
        if (savedMACs[i].length() > 0) return true;
    }
    return false;
}

void saveController(const String& deviceMAC) {
    if (isKnownController(deviceMAC)) return;

    int slotToUse = -1;
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
        if (savedMACs[i].length() == 0) {
            slotToUse = i;
            break;
        }
    }

    if (slotToUse < 0) {
        slotToUse = currentSlot;
    }

    // Save into a rotating slot so adding a sixth controller replaces the
    // oldest remembered slot rather than growing storage forever.
    savedMACs[slotToUse] = deviceMAC;

    preferences.begin(CONFIG_NAMESPACE, false);
    preferences.putString(("mac" + String(slotToUse)).c_str(), deviceMAC);

    currentSlot = (slotToUse + 1) % MAX_CONTROLLERS;
    preferences.putInt("slot", currentSlot);
    preferences.end();
}

bool removeController(int slot) {
    if (slot < 0 || slot >= MAX_CONTROLLERS) return false;
    if (savedMACs[slot].length() == 0) return false;

    savedMACs[slot] = "";
    preferences.begin(CONFIG_NAMESPACE, false);
    preferences.remove(("mac" + String(slot)).c_str());
    preferences.end();
    return true;
}

void saveRssiThreshold(int threshold) {
    if (threshold < -100) threshold = -100;
    if (threshold > -20) threshold = -20;

    webScanRssiThreshold = threshold;

    preferences.begin(CONFIG_NAMESPACE, false);
    preferences.putInt("rssi", webScanRssiThreshold);
    preferences.end();
}

void clearFoundControllers() {
    foundControllerCount = 0;
}

int foundControllerIndex(const String& deviceMAC) {
    for (int i = 0; i < foundControllerCount; i++) {
        if (deviceMAC.equalsIgnoreCase(foundControllers[i].mac)) return i;
    }
    return -1;
}

void rememberFoundController(const String& deviceMAC, const String& deviceName, int rssi) {
    if (isKnownController(deviceMAC)) return;

    int existingIndex = foundControllerIndex(deviceMAC);
    if (existingIndex >= 0) {
        if (deviceName.length() > 0) {
            foundControllers[existingIndex].name = deviceName;
        }
        foundControllers[existingIndex].rssi = rssi;
        return;
    }

    if (foundControllerCount >= MAX_FOUND_CONTROLLERS) return;

    foundControllers[foundControllerCount].mac = deviceMAC;
    foundControllers[foundControllerCount].name = deviceName;
    foundControllers[foundControllerCount].rssi = rssi;
    foundControllerCount++;

    String label = deviceName.length() > 0 ? deviceName : "Unknown";
    addLog(String("DISCOVERED CONTROLLER: ") + label + " / " + deviceMAC + " (" + String(rssi) + " dBm)");
}

bool isHexChar(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

String normalizeMac(String mac) {
    mac.trim();
    mac.toLowerCase();
    mac.replace("-", ":");
    return mac;
}

bool isValidMac(const String& mac) {
    if (mac.length() != 17) return false;

    for (int i = 0; i < 17; i++) {
        if ((i + 1) % 3 == 0) {
            if (mac.charAt(i) != ':') return false;
        } else if (!isHexChar(mac.charAt(i))) {
            return false;
        }
    }

    return true;
}

bool parseMacAddress(const String& mac, uint8_t address[6]) {
    unsigned int octets[6];
    if (sscanf(mac.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
               &octets[0], &octets[1], &octets[2],
               &octets[3], &octets[4], &octets[5]) != 6) {
        return false;
    }

    bool anyNonZero = false;
    for (int i = 0; i < 6; i++) {
        if (octets[i] > 0xff) return false;
        address[i] = (uint8_t)octets[i];
        if (address[i] != 0) anyNonZero = true;
    }

    // A Bluetooth device address must identify one device, not a multicast or
    // broadcast group. esp_iface_mac_addr_set() also validates this, but the
    // explicit check gives a useful startup message.
    return anyNonZero && (address[0] & 0x01) == 0;
}

void applyClassicBluetoothSpoof() {
    String configuredMac = normalizeMac(classicBtSpoofMac);
    if (configuredMac.length() == 0) {
        addLog("Classic Bluetooth - Using factory MAC address.");
        return;
    }

    uint8_t address[6];
    if (!isValidMac(configuredMac) || !parseMacAddress(configuredMac, address)) {
        addLog(String("Classic Bluetooth - WARNING: invalid spoof MAC: ") + configuredMac);
        return;
    }

    esp_err_t result = esp_iface_mac_addr_set(address, ESP_MAC_BT);
    if (result == ESP_OK) {
        classicBtSpoofEnabled = true;
        addLog(String("Classic Bluetooth - Spoof MAC set to ") + configuredMac);
    } else {
        addLog(String("Classic Bluetooth - WARNING: spoof MAC failed: ") + String((int)result));
    }
}

String jsonEscape(const String& value) {
    String escaped = "";
    for (unsigned int i = 0; i < value.length(); i++) {
        char c = value.charAt(i);
        if (c == '"' || c == '\\') {
            escaped += '\\';
            escaped += c;
        } else if (c == '\n') {
            escaped += "\\n";
        } else if (c == '\r') {
            escaped += "\\r";
        } else {
            escaped += c;
        }
    }
    return escaped;
}

String logsJson() {
    String json = "[";
    for (int i = 0; i < logCount; i++) {
        if (i > 0) json += ",";
        int index = (logStart + i) % MAX_LOG_LINES;
        json += "\"";
        json += jsonEscape(logLines[index]);
        json += "\"";
    }
    json += "]";
    return json;
}

void blinkConfirmation() {
    for (int i = 0; i < 2; i++) {
        digitalWrite(LED_PIN, HIGH);
        delay(CONFIRM_BLINK_MS);
        digitalWrite(LED_PIN, LOW);
        delay(CONFIRM_BLINK_MS);
    }
}

// ================= BLUETOOTH CLASSIC INQUIRY =================
String classicAddressToString(const esp_bd_addr_t address) {
    char buffer[18];
    snprintf(buffer, sizeof(buffer), "%02x:%02x:%02x:%02x:%02x:%02x",
             address[0], address[1], address[2], address[3], address[4], address[5]);
    return String(buffer);
}

void copyBtName(String& target, const uint8_t* value, int length) {
    if (value == nullptr || length <= 0) return;

    int copyLength = length;
    if (copyLength > 31) copyLength = 31;

    char name[32];
    memcpy(name, value, copyLength);
    name[copyLength] = '\0';
    target = String(name);
}

void parseClassicProperties(int numProps, esp_bt_gap_dev_prop_t* props, String& deviceName, int& rssi) {
    if (props == nullptr) return;

    for (int i = 0; i < numProps; i++) {
        esp_bt_gap_dev_prop_t prop = props[i];

        if (prop.type == ESP_BT_GAP_DEV_PROP_RSSI && prop.val != nullptr && prop.len > 0) {
            rssi = *((int8_t*)prop.val);
        } else if (prop.type == ESP_BT_GAP_DEV_PROP_BDNAME) {
            copyBtName(deviceName, (uint8_t*)prop.val, prop.len);
        } else if (prop.type == ESP_BT_GAP_DEV_PROP_EIR && prop.val != nullptr) {
            uint8_t nameLength = 0;
            uint8_t* name = esp_bt_gap_resolve_eir_data((uint8_t*)prop.val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &nameLength);
            if (name == nullptr) {
                name = esp_bt_gap_resolve_eir_data((uint8_t*)prop.val, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &nameLength);
            }
            if (name != nullptr && nameLength > 0) {
                copyBtName(deviceName, name, nameLength);
            }
        }
    }
}

void handleClassicDevice(const String& deviceMAC, const String& deviceName, int rssi) {
    unsigned long now = millis();

    if (webScanActive) {
        if (rssi != -127 && rssi < webScanRssiThreshold) return;

        String label = deviceName.length() > 0 ? deviceName : "Classic Bluetooth";
        rememberFoundController(deviceMAC, label, rssi);
        return;
    }

    if (powerState != POWER_IDLE) return;
    if (stablePcOn) return;
    if (!isKnownController(deviceMAC)) return;
    if (lastShutdownTime > 0 && now - lastShutdownTime < SHUTDOWN_COOLDOWN_MS) return;
    if (now - lastWakeTime < WAKE_COOLDOWN_MS) return;

    addLog("Classic Bluetooth - Known controller detected! Turning on ATX PSU and PC...");
    startAtxPowerOnSequence();
    lastWakeTime = now;
}

void classicGapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
    if (event == ESP_BT_GAP_DISC_RES_EVT) {
        String deviceMAC = classicAddressToString(param->disc_res.bda);
        String deviceName = "";
        int rssi = -127;

        parseClassicProperties(param->disc_res.num_prop, param->disc_res.prop, deviceName, rssi);
        handleClassicDevice(deviceMAC, deviceName, rssi);
    } else if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT) {
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            classicDiscoveryRunning = true;
            //addLog("Classic Bluetooth inquiry started.");
        } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            classicDiscoveryRunning = false;
            classicDiscoveryCancelRequested = false;
            //addLog("Classic Bluetooth inquiry finished.");
        }
    } else if (event == ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT &&
               classicBtSpoofEnabled &&
               param->acl_conn_cmpl_stat.stat == ESP_BT_STATUS_SUCCESS) {
        // A gamepad paired to the PC pages the PC adapter's address instead of
        // entering inquiry/discoverable mode. With that address spoofed, the
        // incoming ACL attempt exposes the gamepad address here even though
        // authentication will normally fail without the PC's link key.
        String deviceMAC = classicAddressToString(param->acl_conn_cmpl_stat.bda);
        handleClassicDevice(deviceMAC, "Paired Classic device", -127);
    }
}

void updateClassicPageScan() {
    if (!classicBtSpoofEnabled) return;

    // Do not answer under the PC adapter's address once the real PC adapter is
    // powered. Re-enable page scan only while the PC is off and the ESP32 is
    // responsible for noticing controllers looking for their paired host.
    bool shouldEnable = !stablePcOn;
    if (classicPageScanTargetInitialized && shouldEnable == classicPageScanTargetEnabled) return;

    classicPageScanTargetInitialized = true;
    classicPageScanTargetEnabled = shouldEnable;

    esp_err_t result = esp_bt_gap_set_scan_mode(
        shouldEnable ? ESP_BT_CONNECTABLE : ESP_BT_NON_CONNECTABLE,
        ESP_BT_NON_DISCOVERABLE
    );
    if (result != ESP_OK) {
        addLog(String("Classic Bluetooth - WARNING: page scan update failed: ") + String((int)result));
    }
}

void setupClassicBluetooth() {
    esp_err_t result = esp_bt_gap_register_callback(classicGapCallback);
    if (result != ESP_OK) {
        addLog(String("Classic Bluetooth - WARNING: callback failed: ") + String((int)result));
        return;
    }

    updateClassicPageScan();
    addLog("Classic Bluetooth - Inquiry ready.");
}

bool setupApHasClient() {
    return setupApRunning && WiFi.softAPgetStationNum() > 0;
}

void protectSetupApClient() {
    // WiFi and Bluetooth share the ESP32's 2.4 GHz radio. A Classic inquiry
    // lasts about five seconds and can make a setup-AP client appear to be
    // disconnected. Pause normal wake discovery while the portal is in use.
    // An explicit scan requested from the webpage is still allowed.
    if (!setupApHasClient() || webScanActive) return;

    if (classicDiscoveryRunning && !classicDiscoveryCancelRequested) {
        if (esp_bt_gap_cancel_discovery() == ESP_OK) {
            classicDiscoveryCancelRequested = true;
        }
    }
}

void scanClassicBluetooth() {
    unsigned long now = millis();

    if (buttonIsPressed()) return;

    if (webScanActive && now - webScanStartTime >= WEB_SCAN_DURATION_MS) {
        webScanActive = false;
        addLog("Web controller scan finished.");
    }

    if (!webScanActive && setupApHasClient()) return;

    if (!webScanActive) {
        if (stablePcOn) return;
        if (!hasSavedController()) return;
    }

    if (classicDiscoveryRunning) return;
    if (bleScanRunning) return;
    if (now - lastClassicScanTime < CLASSIC_SCAN_INTERVAL_MS) return;
    lastClassicScanTime = now;
    classicDiscoveryCancelRequested = false;

    esp_err_t result = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, CLASSIC_INQUIRY_DURATION, 0);
    if (result != ESP_OK) {
        classicDiscoveryRunning = false;
        addLog(String("Classic Bluetooth - WARNING: inquiry failed: ") + String((int)result));
    } else {
        // Mark immediately to avoid starting a BLE scan in the same loop cycle
        // before the async DISCOVERY_STARTED callback arrives.
        classicDiscoveryRunning = true;
    }
}

// ================= BLE CALLBACK =================
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
        String deviceMAC = advertisedDevice.getAddress().toString().c_str();
        unsigned long now = millis();

        if (webScanActive) {
            int currentRSSI = advertisedDevice.getRSSI();
            if (currentRSSI < webScanRssiThreshold) return;

            String deviceName = "";
            if (advertisedDevice.haveName()) {
                deviceName = advertisedDevice.getName().c_str();
            }

            rememberFoundController(deviceMAC, deviceName, currentRSSI);
            return;
        }

        // The BLE scanner sees many repeated advertisements. These gates keep
        // wake requests quiet unless this is a known controller and the PC can
        // safely be woken.
        if (powerState != POWER_IDLE) return;
        if (stablePcOn) return;
        if (!isKnownController(deviceMAC)) return;
        if (lastShutdownTime > 0 && now - lastShutdownTime < SHUTDOWN_COOLDOWN_MS) return;
        if (now - lastWakeTime < WAKE_COOLDOWN_MS) return;

        addLog("BLE - Known controller detected! Turning on ATX PSU and PC...");
        startAtxPowerOnSequence();
        lastWakeTime = now;
    }
};

void updateStatusLed(unsigned long now) {
    if (now - lastBlinkTime >= POWER_OFF_PREVIEW_BLINK_MS) {
        lastBlinkTime = now;
        ledBlinkState = !ledBlinkState;
        digitalWrite(LED_PIN, ledBlinkState);
    }
}

void handleShutdownTracking(unsigned long now) {
    // Check if the PC has just turned off
    if (!stablePcOn && lastPcState) {
        lastShutdownTime = now; // Start the 60s ignore timer for controller wake-ups
        if (lastShutdownTime == 0) lastShutdownTime = 1;
        addLog("STATE - PC turned OFF. Releasing ATX PSU and activating 60s gamepad ignore timer...");
        if (powerState == POWER_IDLE) {
            // If PC power state si already idle, turn off the PSU immediately.
            digitalWrite(RELAY_PSU_PIN, RELAY_OFF);
        }
    }

    lastPcState = stablePcOn;
}

void syncAtxPsuRelay() {
    // Outside an active transition, mirror ATX PS_ON to the stable PC state.
    if (powerState == POWER_IDLE) {
        digitalWrite(RELAY_PSU_PIN, stablePcOn ? RELAY_ON : RELAY_OFF);
    }
}

void handleButton(unsigned long now) {
    bool buttonPressed = buttonIsPressed();

    if (buttonPressed) {
        if (buttonPressStartTime == 0) {
            buttonPressStartTime = now;
            shutdownPress = false;
        }

        unsigned long pressDuration = now - buttonPressStartTime;

        if (pressDuration >= POWER_OFF_HOLD_MS && !shutdownPress) {
            if(powerState == POWER_IDLE){
                blinkConfirmation();
                addLog("BUTTON - LONG PRESS (" + String(pressDuration) + "ms) - Normal shutdown.");
                startNormalShutdown();
                shutdownPress = true;
            }else{
                addLog("BUTTON PRESSED (" + String(pressDuration) + "ms) - Power operation already in progress...");
            }
        }else{
            if (!shutdownPress) addLog("BUTTON PRESSED (" + String(pressDuration) + "ms) - No action.");
        }

    } else {
        if (buttonPressStartTime > 0) {
            unsigned long pressDuration = now - buttonPressStartTime;


            if (pressDuration < POWER_OFF_HOLD_MS) {
                // Short press detected
                blinkConfirmation();
                addLog("BUTTON RELEASED (" + String(pressDuration) + "ms) - Power on.");
                startAtxPowerOnSequence();
            }else{
                addLog("BUTTON RELEASED (" + String(pressDuration) + "ms) - No action.");
            }

            buttonPressStartTime = 0;
            shutdownPress = false;
        }
    }
}

void scanBle() {
    unsigned long now = millis();

    // Keep button holds responsive. BLE scan blocks the loop, so skip it while
    // the user is holding the button and needs live LED/mode feedback.
    if (buttonIsPressed()) return;
    if (classicDiscoveryRunning) return;
    if (bleScanRunning) return;

    if (webScanActive && now - webScanStartTime >= WEB_SCAN_DURATION_MS) {
        webScanActive = false;
        addLog("BLE - Forced scan finished.");
    }

    if (!webScanActive && setupApHasClient()) return;

    // Web scans must always scan so the user can discover new controllers.
    // Normal wake scanning is only useful while the PC is off and at least one
    // saved controller exists.
    if(!webScanActive){
        if (stablePcOn) return;
        if (!hasSavedController()) return;
    }

    if (now - lastBleScanTime < BLE_SCAN_INTERVAL_MS) return;
    lastBleScanTime = now;

    bleScanRunning = true;
    bleScan->start(1, false);
    bleScanRunning = false;
    bleScan->clearResults();
}

String powerStateName() {
    switch (powerState) {
        case POWER_IDLE: return "idle";
        case POWER_ON_WAIT_FOR_PSU: return "power_on_wait_for_psu";
        case POWER_ON_PRESS_BUTTON: return "power_on_press_button";
        case POWER_ON_CONFIRM: return "power_on_confirm";
        case POWER_OFF_PRESS_BUTTON: return "power_off_press_button";
        case POWER_OFF_WAIT_FOR_OFF: return "power_off_wait_for_off";
        default: return "unknown";
    }
}

String controllersJson() {
    String json = "[";
    bool first = true;
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
        if (savedMACs[i].length() == 0) continue;
        if (!first) json += ",";
        json += "{\"slot\":";
        json += i;
        json += ",\"mac\":\"";
        json += jsonEscape(savedMACs[i]);
        json += "\"}";
        first = false;
    }
    json += "]";
    return json;
}

String foundControllersJson() {
    String json = "[";
    bool first = true;
    for (int i = 0; i < foundControllerCount; i++) {
        if (isKnownController(foundControllers[i].mac)) continue;
        if (!first) json += ",";
        json += "{\"mac\":\"";
        json += jsonEscape(foundControllers[i].mac);
        json += "\",\"name\":\"";
        json += jsonEscape(foundControllers[i].name);
        json += "\",\"rssi\":";
        json += foundControllers[i].rssi;
        json += "}";
        first = false;
    }
    json += "]";
    return json;
}

String webIpAddress() {
    if (WiFi.status() == WL_CONNECTED) {
        return WiFi.localIP().toString();
    }
    return WiFi.softAPIP().toString();
}

String wifiStatusName(wl_status_t status) {
    switch (status) {
        case WL_CONNECTED: return "connected";
        case WL_NO_SSID_AVAIL: return "network not found";
        case WL_CONNECT_FAILED: return "connect failed";
        case WL_CONNECTION_LOST: return "connection lost";
        case WL_DISCONNECTED: return "disconnected";
        case WL_IDLE_STATUS: return "idle";
        default: return "status " + String((int)status);
    }
}

void loadWiFiSettings() {
    preferences.begin(WIFI_CONFIG_NAMESPACE, true);
    wifiSsid = preferences.getString("ssid", DEFAULT_WIFI_SSID);
    wifiPassword = preferences.getString("password", DEFAULT_WIFI_PASSWORD);
    preferences.end();
}

bool saveWiFiSettings(const String& ssid, const String& password, bool replacePassword) {
    if (ssid.length() > 32) return false;
    if (replacePassword && password.length() > 63) return false;
    if (replacePassword && password.length() > 0 && password.length() < 8) return false;

    wifiSsid = ssid;
    if (replacePassword) wifiPassword = password;

    preferences.begin(WIFI_CONFIG_NAMESPACE, false);
    preferences.putString("ssid", wifiSsid);
    if (replacePassword) preferences.putString("password", wifiPassword);
    preferences.end();
    return true;
}

String wifiConfigJson() {
    String json = "{\"ssid\":\"";
    json += jsonEscape(wifiSsid);
    json += "\",\"hasPassword\":";
    json += wifiPassword.length() > 0 ? "true" : "false";
    json += ",\"hostname\":\"";
    json += jsonEscape(WEB_HOSTNAME);
    json += "\",\"classicBtSpoofMac\":\"";
    json += jsonEscape(classicBtSpoofMac);
    json += "\"}";
    return json;
}

void startSetupAp() {
    if (setupApRunning) return;

    if (WiFi.status() == WL_CONNECTED) {
        WiFi.mode(WIFI_AP_STA);
    } else {
        WiFi.disconnect(false);
        WiFi.mode(WIFI_AP);
    }

    bool apStarted = false;
    if (WIFI_AP_PASSWORD[0] == '\0') {
        apStarted = WiFi.softAP(WEB_HOSTNAME);
    } else {
        apStarted = WiFi.softAP(WEB_HOSTNAME, WIFI_AP_PASSWORD);
    }

    if (apStarted) {
        setupApRunning = true;
        addLog(String("AP - Setup started: ") + WEB_HOSTNAME);
        addLog(String("AP - Webpage: http://") + WiFi.softAPIP().toString());
    } else {
        addLog("AP - WARNING: Setup failed to start.");
    }
}

void stopSetupAp() {
    if (!setupApRunning) return;

    WiFi.softAPdisconnect(true);
    setupApRunning = false;
    WiFi.mode(WIFI_STA);
    addLog("AP - Setup stopped after router WiFi connected.");
}

void sendResponse(WiFiClient& client, int code, const char* type, const String& body) {
    client.print("HTTP/1.1 ");
    client.print(code);
    client.println(code == 200 ? " OK" : " Error");
    client.print("Content-Type: ");
    client.println(type);
    client.println("Cache-Control: no-store");
    client.println("Connection: close");
    client.println();
    client.print(body);
}

void sendJson(WiFiClient& client, int code, const String& json) {
    sendResponse(client, code, "application/json; charset=utf-8", json);
}

String urlDecode(const String& value) {
    String decoded = "";
    for (unsigned int i = 0; i < value.length(); i++) {
        char c = value.charAt(i);
        if (c == '%' && i + 2 < value.length()) {
            char hex[3] = { value.charAt(i + 1), value.charAt(i + 2), 0 };
            decoded += char(strtol(hex, nullptr, 16));
            i += 2;
        } else if (c == '+') {
            decoded += ' ';
        } else {
            decoded += c;
        }
    }
    return decoded;
}

String queryParam(const String& query, const String& key) {
    int start = 0;
    while (start < query.length()) {
        int end = query.indexOf('&', start);
        if (end < 0) end = query.length();

        int equals = query.indexOf('=', start);
        if (equals > start && equals < end && query.substring(start, equals) == key) {
            return urlDecode(query.substring(equals + 1, end));
        }

        start = end + 1;
    }
    return "";
}

void handleWebRoot(WiFiClient& client) {
    static const char page[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <meta name="theme-color" content="#0a1018">
  <link rel="icon" href="data:image/svg+xml,<svg xmlns=%22http://www.w3.org/2000/svg%22 viewBox=%220 0 100 100%22><text y=%22.9em%22 font-size=%2290%22>🎮</text></svg>">
  <title>BC250 Control Panel</title>
  <style>
    :root {
      color-scheme: dark;
      font-family: Inter, ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      background: #080d14;
      color: #e8eef6;
      --bg: #080d14;
      --surface: #101823;
      --surface-2: #151f2c;
      --line: #243244;
      --muted: #8695a8;
      --blue: #4c91ff;
      --blue-2: #2776ee;
      --green: #45d48a;
      --red: #ff6470;
      --amber: #f4b84a;
      --radius: 16px;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      min-height: 100vh;
      background:
        radial-gradient(circle at 8% 0%, rgba(47, 119, 238, .15), transparent 30rem),
        var(--bg);
    }
    button, input { font: inherit; }
    button { -webkit-tap-highlight-color: transparent; }
    .shell { width: min(1180px, calc(100% - 32px)); margin: 0 auto; padding: 28px 0 44px; }
    header { display: flex; align-items: center; justify-content: space-between; gap: 20px; margin-bottom: 24px; }
    .brand { display: flex; align-items: center; gap: 13px; }
    .brand-mark {
      display: grid; place-items: center; width: 44px; height: 44px; border: 1px solid #3169a9;
      border-radius: 13px; background: linear-gradient(145deg, #1c58a0, #163353); color: #fff;
      box-shadow: 0 10px 32px rgba(20, 88, 166, .25);
    }
    .brand-mark svg { width: 24px; }
    h1 { font-size: 1.1rem; letter-spacing: .01em; margin: 0 0 2px; }
    .subtitle, .muted { color: var(--muted); }
    .subtitle { font-size: .79rem; }
    .live { display: flex; align-items: center; gap: 8px; font-size: .78rem; color: var(--muted); }
    .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--amber); box-shadow: 0 0 0 4px rgba(244, 184, 74, .1); }
    .dot.online { background: var(--green); box-shadow: 0 0 0 4px rgba(69, 212, 138, .1); }
    .dot.offline { background: var(--red); box-shadow: 0 0 0 4px rgba(255, 100, 112, .1); }
    .overview { display: grid; grid-template-columns: 1.35fr repeat(3, 1fr); gap: 12px; margin-bottom: 12px; }
    .card { border: 1px solid var(--line); border-radius: var(--radius); background: rgba(16, 24, 35, .94); box-shadow: 0 14px 36px rgba(0, 0, 0, .16); }
    .metric { min-height: 112px; padding: 18px; display: flex; flex-direction: column; justify-content: space-between; }
    .metric-label { display: flex; align-items: center; gap: 8px; color: var(--muted); font-size: .72rem; font-weight: 700; letter-spacing: .09em; text-transform: uppercase; }
    .metric-label svg { width: 15px; color: #9eb0c5; }
    .metric-value { display: flex; align-items: baseline; gap: 8px; font-size: 1.3rem; font-weight: 750; }
    .metric-value small { color: var(--muted); font-size: .72rem; font-weight: 500; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    .power-metric { background: linear-gradient(120deg, rgba(32, 64, 103, .72), rgba(16, 24, 35, .95)); }
    #pc.on { color: var(--green); }
    #pc.off { color: #aeb9c6; }
    .layout { display: grid; grid-template-columns: minmax(0, 1.25fr) minmax(320px, .75fr); gap: 12px; align-items: start; }
    .stack { display: grid; gap: 12px; }
    .panel { padding: 20px; }
    .panel-head { display: flex; justify-content: space-between; align-items: flex-start; gap: 16px; margin-bottom: 18px; }
    .panel-title { display: flex; align-items: center; gap: 9px; }
    .panel-title svg { width: 18px; color: var(--blue); }
    h2 { margin: 0; font-size: .93rem; letter-spacing: .01em; }
    .eyebrow { margin-top: 4px; color: var(--muted); font-size: .76rem; }
    .badge { padding: 5px 9px; border: 1px solid var(--line); border-radius: 999px; background: #0d1520; color: var(--muted); font-size: .7rem; font-weight: 700; white-space: nowrap; }
    .badge.active { border-color: rgba(69, 212, 138, .35); background: rgba(69, 212, 138, .09); color: var(--green); }
    .power-controls { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
    button {
      min-height: 42px; padding: 10px 15px; border: 1px solid transparent; border-radius: 10px;
      background: linear-gradient(180deg, var(--blue), var(--blue-2)); color: white; font-weight: 750; cursor: pointer;
      transition: transform .15s ease, border-color .15s ease, filter .15s ease;
    }
    button:hover:not(:disabled) { filter: brightness(1.09); transform: translateY(-1px); }
    button:active:not(:disabled) { transform: translateY(0); }
    button:focus-visible, input:focus-visible { outline: 2px solid var(--blue); outline-offset: 2px; }
    button:disabled { opacity: .42; cursor: not-allowed; }
    button.secondary { border-color: #34455a; background: #1a2736; color: #d8e2ee; }
    button.danger { border-color: rgba(255, 100, 112, .34); background: rgba(255, 100, 112, .1); color: #ff8790; }
    button.compact { min-height: 34px; padding: 7px 11px; font-size: .76rem; }
    .control-button { min-height: 74px; display: flex; align-items: center; justify-content: center; gap: 10px; }
    .control-button svg { width: 19px; }
    .control-button.danger { background: linear-gradient(180deg, #c94855, #a93440); border-color: transparent; color: white; }
    .section-rule { height: 1px; background: var(--line); margin: 18px 0; }
    .row { display: flex; gap: 9px; align-items: center; flex-wrap: wrap; }
    .field-row { display: grid; grid-template-columns: minmax(0, 1fr) auto; gap: 9px; }
    label.field { display: block; margin: 13px 0 6px; color: #a6b4c5; font-size: .76rem; font-weight: 650; }
    label.field-with-info { display: flex; align-items: center; gap: 7px; }
    .info-tip {
      position: relative; display: inline-grid; place-items: center; width: 17px; height: 17px;
      border: 1px solid #52677f; border-radius: 50%; color: #b9c8d8; font-size: .68rem;
      font-weight: 800; line-height: 1; cursor: help; outline: none;
    }
    .info-tip:focus-visible { outline: 2px solid var(--blue); outline-offset: 2px; }
    .tooltip {
      position: absolute; z-index: 20; right: -8px; bottom: calc(100% + 9px); width: min(290px, calc(100vw - 48px));
      padding: 10px 11px; border: 1px solid #39506b; border-radius: 9px; background: #172435;
      color: #dbe6f2; box-shadow: 0 12px 32px rgba(0,0,0,.38); font-size: .7rem;
      font-weight: 500; line-height: 1.45; opacity: 0; visibility: hidden; pointer-events: none;
      transform: translateY(4px); transition: opacity .15s ease, transform .15s ease, visibility .15s;
    }
    .info-tip:hover .tooltip, .info-tip:focus .tooltip { opacity: 1; visibility: visible; transform: translateY(0); }
    input {
      width: 100%; min-height: 42px; border: 1px solid #334358; border-radius: 10px;
      padding: 9px 12px; background: #0b121b; color: #edf3f9;
    }
    input::placeholder { color: #5f7084; }
    input[type="number"] { width: 92px; }
    input[type="checkbox"] { width: 16px; min-height: auto; accent-color: var(--blue); }
    .check-row { margin-top: 12px; color: #b4c0ce; font-size: .78rem; }
    .hint { margin-top: 8px; color: var(--muted); font-size: .72rem; line-height: 1.5; }
    .item { display: flex; justify-content: space-between; gap: 12px; align-items: center; border-top: 1px solid #202d3c; padding: 12px 0; }
    .item:first-child { border-top: 0; padding-top: 0; }
    .item:last-child { padding-bottom: 0; }
    .item-name { margin-bottom: 3px; font-size: .84rem; font-weight: 700; }
    .item-meta { color: var(--muted); font-size: .72rem; }
    code { color: #aec4dc; font-family: "SFMono-Regular", Consolas, monospace; font-size: .72rem; }
    .empty { padding: 18px; border: 1px dashed #2c3c50; border-radius: 11px; color: var(--muted); text-align: center; font-size: .78rem; }
    .scan-tools { display: flex; justify-content: space-between; align-items: center; gap: 12px; margin-bottom: 13px; }
    .rssi-tools { display: flex; gap: 8px; align-items: center; }
    .rssi-tools label { color: var(--muted); font-size: .72rem; }
    .network-status { display: flex; gap: 11px; align-items: center; padding: 12px; border: 1px solid #29384b; border-radius: 11px; background: #0c141e; }
    .network-status svg { width: 21px; color: var(--green); flex: 0 0 auto; }
    #wifiState { font-size: .82rem; font-weight: 700; }
    #wifiAddress { margin-top: 2px; font-size: .7rem; word-break: break-all; }
    .message { min-height: 18px; margin-top: 9px; color: var(--muted); font-size: .72rem; }
    .restart-row { display: flex; justify-content: space-between; gap: 12px; align-items: center; }
    .log-panel { overflow: hidden; }
    .log-toolbar { padding: 14px 17px; border-bottom: 1px solid var(--line); display: flex; justify-content: space-between; align-items: center; }
    .terminal-dots { display: flex; gap: 5px; }
    .terminal-dots i { width: 7px; height: 7px; border-radius: 50%; background: #405064; }
    pre { margin: 0; min-height: 160px; max-height: 290px; padding: 16px 17px; overflow: auto; white-space: pre-wrap; word-break: break-word; color: #a7b8ca; background: #090f17; font: .7rem/1.55 "SFMono-Regular", Consolas, monospace; }
    .toast { position: fixed; right: 20px; bottom: 20px; z-index: 10; max-width: min(360px, calc(100% - 40px)); padding: 11px 14px; border: 1px solid #355072; border-radius: 10px; background: #152335; color: #e9f2fc; box-shadow: 0 14px 40px rgba(0,0,0,.35); font-size: .78rem; opacity: 0; transform: translateY(10px); pointer-events: none; transition: .2s ease; }
    .toast.show { opacity: 1; transform: translateY(0); }
    @media (max-width: 860px) {
      .overview { grid-template-columns: 1fr 1fr; }
      .layout { grid-template-columns: 1fr; }
    }
    @media (max-width: 540px) {
      .shell { width: min(100% - 20px, 1180px); padding-top: 18px; }
      header { margin-bottom: 18px; }
      .overview { gap: 9px; }
      .metric { min-height: 98px; padding: 14px; }
      .metric-value { font-size: 1.08rem; }
      .panel { padding: 16px; }
      .power-controls { grid-template-columns: 1fr; }
      .control-button { min-height: 54px; }
      .scan-tools { align-items: flex-start; flex-direction: column; }
      .field-row { grid-template-columns: 1fr; }
      .restart-row { align-items: flex-start; flex-direction: column; }
      .restart-row button { width: 100%; }
    }
  </style>
</head>
<body>
  <main class="shell">
    <header>
      <div class="brand">
        <div class="brand-mark" aria-hidden="true"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><path d="M7.5 8h9a4 4 0 0 1 3.8 2.8l1.3 4.2a2.4 2.4 0 0 1-4.1 2.3l-1.7-1.8H8.2l-1.7 1.8A2.4 2.4 0 0 1 2.4 15l1.3-4.2A4 4 0 0 1 7.5 8Z"/><path d="M8 11v4M6 13h4M15.5 12.5h.01M18 14h.01"/></svg></div>
        <div><h1>BC250 Control Panel</h1><div class="subtitle">ESP32 power &amp; controller management</div></div>
      </div>
      <div class="live"><span id="liveDot" class="dot"></span><span id="liveText">Connecting</span></div>
    </header>

    <section class="overview" aria-label="System overview">
      <div class="card metric power-metric"><div class="metric-label"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 2v10M18.4 6.6a9 9 0 1 1-12.8 0"/></svg>System power</div><div class="metric-value"><span id="pc">...</span><small id="powerState">Waiting for controller</small></div></div>
      <div class="card metric"><div class="metric-label"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M6 9h12a4 4 0 0 1 3.8 5.2l-1 3a2 2 0 0 1-3.3.8l-2-2H8.5l-2 2a2 2 0 0 1-3.3-.8l-1-3A4 4 0 0 1 6 9Z"/></svg>Controllers</div><div class="metric-value"><span id="controllerCount">—</span><small>registered</small></div></div>
      <div class="card metric"><div class="metric-label"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M5 12.5a10 10 0 0 1 14 0M8.5 16a5 5 0 0 1 7 0M12 20h.01"/></svg>Network</div><div class="metric-value"><span id="networkSummary">—</span><small id="networkIp">No address</small></div></div>
      <div class="card metric"><div class="metric-label"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M4 12h3l2-6 4 12 2-6h5"/></svg>Activity</div><div class="metric-value"><span id="activitySummary">Idle</span><small id="lastUpdate">Not updated</small></div></div>
    </section>

    <div class="layout">
      <div class="stack">
        <section class="card panel">
          <div class="panel-head"><div><div class="panel-title"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 2v10M18.4 6.6a9 9 0 1 1-12.8 0"/></svg><h2>Power control</h2></div><div class="eyebrow">Operate the ATX PSU and motherboard power switch</div></div><span id="powerBadge" class="badge">Checking</span></div>
          <div class="power-controls">
            <button id="powerOnBtn" class="control-button" onclick="post('/api/power/on', 'Power-on command sent')"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 2v10M18.4 6.6a9 9 0 1 1-12.8 0"/></svg>Power on</button>
            <button id="powerOffBtn" class="control-button danger" onclick="powerOff()"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 2v10M18.4 6.6a9 9 0 1 1-12.8 0"/></svg>Shut down</button>
          </div>
        </section>

        <section class="card panel">
          <div class="panel-head"><div><div class="panel-title"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M6 9h12a4 4 0 0 1 3.8 5.2l-1 3a2 2 0 0 1-3.3.8l-2-2H8.5l-2 2a2 2 0 0 1-3.3-.8l-1-3A4 4 0 0 1 6 9Z"/><path d="M8 11v4M6 13h4"/></svg><h2>Registered controllers</h2></div><div class="eyebrow">Controllers allowed to trigger the system</div></div><span id="pairedBadge" class="badge">0 paired</span></div>
          <div id="paired"><div class="empty">Loading controllers…</div></div>
          <div class="section-rule"></div>
          <label class="field" for="manualMac">Add by Bluetooth address</label>
          <div class="field-row"><input id="manualMac" aria-label="Controller Bluetooth address" placeholder="aa:bb:cc:dd:ee:ff" maxlength="17"><button onclick="manualAdd()">Add controller</button></div>
          <div class="hint">Use the manual address if a controller does not appear in a nearby scan.</div>
        </section>

        <section class="card panel">
          <div class="panel-head"><div><div class="panel-title"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="11" cy="11" r="7"/><path d="m20 20-4-4"/></svg><h2>Discover controllers</h2></div><div class="eyebrow">Search for nearby Bluetooth gamepads</div></div><span id="scanState" class="badge">Idle</span></div>
          <div class="scan-tools">
            <button id="scanBtn" onclick="post('/api/scan/start', 'Controller scan started')">Start scan</button>
            <div class="rssi-tools"><label for="rssiInput">Signal filter</label><input id="rssiInput" type="number" min="-100" max="-20" step="1" aria-label="RSSI threshold"><button id="rssiBtn" class="secondary compact" onclick="setRssi()">Apply</button></div>
          </div>
          <div class="hint" style="margin-bottom:12px">A higher dBm value is stricter: −45 finds close devices; −70 covers a typical room.</div>
          <div id="found"><div class="empty">Start a scan to find nearby controllers.</div></div>
        </section>

        <section class="card log-panel">
          <div class="log-toolbar"><div class="panel-title"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="m8 9 3 3-3 3M13 15h3"/><rect x="3" y="4" width="18" height="16" rx="2"/></svg><h2>Event log</h2></div><div class="terminal-dots" aria-hidden="true"><i></i><i></i><i></i></div></div>
          <pre id="log">Waiting for controller…</pre>
        </section>
      </div>

      <aside class="stack">
        <section class="card panel">
          <div class="panel-head"><div><div class="panel-title"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M5 12.5a10 10 0 0 1 14 0M8.5 16a5 5 0 0 1 7 0M12 20h.01"/></svg><h2>Wi-Fi &amp; device</h2></div><div class="eyebrow">Network and controller settings</div></div></div>
          <div class="network-status"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M5 12.5a10 10 0 0 1 14 0M8.5 16a5 5 0 0 1 7 0M12 20h.01"/></svg><div><div id="wifiState">Connecting…</div><div class="muted" id="wifiAddress"></div></div></div>
          <label class="field" for="wifiSsid">2.4 GHz network name (SSID)</label>
          <input id="wifiSsid" type="text" maxlength="32" autocomplete="off" placeholder="Setup AP only when empty">
          <label class="field" for="wifiPassword">Network password</label>
          <input id="wifiPassword" type="password" maxlength="63" autocomplete="new-password" placeholder="Leave blank to keep saved password">
          <label class="row check-row" for="wifiOpen"><input id="wifiOpen" type="checkbox" onchange="openNetworkChanged()"><span>Open network (clear saved password)</span></label>
          <button id="wifiSaveBtn" style="width:100%;margin-top:15px" onclick="saveWifi()">Save network settings</button>
          <div class="message" id="wifiMessage">Saving settings starts one connection attempt.</div>
          <div class="section-rule"></div>
          <label class="field field-with-info" for="btSpoofMac"><span>Bluetooth spoof address</span><span class="info-tip" tabindex="0" aria-label="About the Bluetooth spoof address"><span aria-hidden="true">i</span><span class="tooltip" role="tooltip">Set the MAC address of the BC250 Bluetooth adapter that the gamepads are already paired with. While the PC is off, the ESP32 uses it to notice a paired gamepad trying to reconnect.<br/><br/>Leave this empty to disable spoofing.<br/>This is not required for BLE gamepads.<br/><br/>You can get the adapter MAC address with the <i>`bluetoothctl list`</i> command.<br/><br/>A restart is required after changing it.</span></span></label>
          <div class="field-row"><input id="btSpoofMac" type="text" maxlength="17" autocomplete="off" placeholder="Disabled when empty" aria-label="Bluetooth spoof address"><button id="btSpoofSaveBtn" class="secondary" onclick="saveBtSpoofMac()">Save</button></div>
          <div class="message" id="btSpoofMessage">Changes take effect after restarting the ESP32.</div>
          <div class="section-rule"></div>
          <div class="restart-row"><div><div style="font-size:.8rem;font-weight:700">Restart ESP32</div><div class="hint">Available only while the PC is off.</div></div><button id="restartBtn" class="danger compact" onclick="restartDevice()">Restart</button></div>
        </section>
      </aside>
    </div>
  </main>
  <div id="toast" class="toast" role="status" aria-live="polite"></div>

<script>
const byId = id => document.getElementById(id);
async function api(path) {
  const response = await fetch(path, { cache: 'no-store' });
  if (!response.ok) throw new Error(path + ' returned ' + response.status);
  return response.json();
}
let refreshInFlight = false;
let refreshQueued = false;
let deviceConfigLoaded = false;
let restarting = false;
let toastTimer;
function notify(message) {
  const toast = byId('toast');
  toast.textContent = message;
  toast.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toast.classList.remove('show'), 2600);
}
function scheduleRefresh(delayMs = 0) { setTimeout(refresh, delayMs); }
async function post(path, message) {
  try {
    const response = await fetch(path, { method: 'POST', cache: 'no-store' });
    if (!response.ok) throw new Error('Request failed');
    if (message) notify(message);
  } catch (error) { notify('Command could not be sent'); }
  scheduleRefresh();
}
function powerOff() {
  if (confirm('Send a normal shutdown command to the PC?')) post('/api/power/off', 'Shutdown command sent');
}
async function manualAdd() {
  const mac = byId('manualMac').value.trim();
  const response = await fetch('/api/manual-add?mac=' + encodeURIComponent(mac), { method: 'POST', cache: 'no-store' });
  if (response.ok) { byId('manualMac').value = ''; notify('Controller added'); }
  else { notify(response.status === 409 ? 'Controller is already registered' : 'Enter a valid Bluetooth address'); }
  scheduleRefresh();
}
async function setRssi() {
  const value = byId('rssiInput').value;
  const response = await fetch('/api/rssi?value=' + encodeURIComponent(value), { method: 'POST', cache: 'no-store' });
  notify(response.ok ? 'Signal filter updated' : 'Could not update signal filter');
  scheduleRefresh();
}
function openNetworkChanged() {
  const password = byId('wifiPassword');
  const isOpen = byId('wifiOpen').checked;
  password.disabled = isOpen;
  if (isOpen) password.value = '';
}
async function saveWifi() {
  const ssid = byId('wifiSsid').value;
  const password = byId('wifiPassword').value;
  const clearPassword = byId('wifiOpen').checked;
  const message = byId('wifiMessage');
  const path = '/api/wifi/save?ssid=' + encodeURIComponent(ssid) + '&password=' + encodeURIComponent(password) + '&clearPassword=' + (clearPassword ? '1' : '0');
  message.textContent = 'Saving settings…';
  const response = await fetch(path, { method: 'POST', cache: 'no-store' });
  const result = await response.json();
  if (!response.ok) {
    message.textContent = result.error === 'invalid_password' ? 'Password must be empty/open or 8 to 63 characters.' : 'Could not save Wi-Fi settings.';
    return;
  }
  byId('wifiPassword').value = '';
  byId('wifiOpen').checked = false;
  openNetworkChanged();
  deviceConfigLoaded = false;
  message.textContent = result.connecting ? 'Saved. Trying the new network…' : 'Saved. Setup AP will remain active.';
  notify('Network settings saved');
  scheduleRefresh();
}
async function saveBtSpoofMac() {
  const input = byId('btSpoofMac');
  const message = byId('btSpoofMessage');
  const mac = input.value.trim();
  message.textContent = 'Saving setting…';
  const response = await fetch('/api/bluetooth/save?mac=' + encodeURIComponent(mac), { method: 'POST', cache: 'no-store' });
  const result = await response.json();
  if (!response.ok) {
    message.textContent = 'Enter a valid unicast MAC address, or leave the field empty to disable spoofing.';
    return;
  }
  input.value = result.mac;
  message.textContent = result.mac ? 'Saved. Restart the ESP32 while the PC is off to apply it.' : 'Spoofing disabled. Restart the ESP32 to apply it.';
  notify(result.mac ? 'Bluetooth spoof address saved' : 'Bluetooth spoofing disabled');
}
async function restartDevice() {
  if (!confirm('Restart the ESP32 controller now?')) return;
  const response = await fetch('/api/restart', { method: 'POST', cache: 'no-store' });
  if (!response.ok) { byId('wifiMessage').textContent = 'Restart is allowed only while the PC is off and idle.'; return; }
  restarting = true;
  byId('wifiMessage').textContent = 'Restarting… Reconnect if the Wi-Fi network changed.';
  byId('restartBtn').disabled = true;
  setTimeout(() => location.reload(), 6000);
}
function empty(text) {
  const div = document.createElement('div');
  div.className = 'empty';
  div.textContent = text;
  return div;
}
function controllerRow(controller, actionText, actionClass, action) {
  const row = document.createElement('div');
  row.className = 'item';
  const label = document.createElement('div');
  if (controller.name !== undefined) {
    const name = document.createElement('div');
    name.className = 'item-name';
    name.textContent = controller.name || 'Unknown controller';
    label.appendChild(name);
  }
  const meta = document.createElement('div');
  meta.className = 'item-meta';
  const mac = document.createElement('code');
  mac.textContent = controller.mac;
  meta.appendChild(mac);
  if (controller.rssi !== undefined) meta.append('  ·  ' + controller.rssi + ' dBm');
  label.appendChild(meta);
  const button = document.createElement('button');
  button.className = (actionClass || 'secondary') + ' compact';
  button.textContent = actionText;
  button.onclick = action;
  row.append(label, button);
  return row;
}
function formatState(value) {
  return (value || 'idle').replaceAll('_', ' ').replace(/\b\w/g, c => c.toUpperCase());
}
async function refresh() {
  if (refreshInFlight) { refreshQueued = true; return; }
  refreshInFlight = true;
  try {
    if (restarting) return;
    const [status, paired, found, logs, wifiConfig] = await Promise.all([api('/api/status'), api('/api/controllers'), api('/api/found'), api('/api/logs'), api('/api/wifi')]);
    const pc = byId('pc');
    pc.textContent = status.pcOn ? 'ON' : 'OFF';
    pc.className = status.pcOn ? 'on' : 'off';
    byId('powerState').textContent = formatState(status.powerState);
    byId('powerBadge').textContent = status.busy ? 'Operation active' : (status.pcOn ? 'System online' : 'System offline');
    byId('powerBadge').className = 'badge' + (status.pcOn ? ' active' : '');
    byId('scanState').textContent = status.scanning ? 'Scanning…' : 'Idle';
    byId('scanState').className = 'badge' + (status.scanning ? ' active' : '');
    byId('activitySummary').textContent = status.scanning ? 'Scanning' : (status.busy ? 'Power task' : 'Idle');
    byId('powerOnBtn').disabled = status.pcOn || status.busy;
    byId('powerOffBtn').disabled = !status.pcOn || status.busy;
    byId('scanBtn').disabled = status.scanning;
    byId('rssiBtn').disabled = status.scanning;
    byId('restartBtn').disabled = status.pcOn || status.busy;
    byId('wifiState').textContent = formatState(status.wifi);
    byId('wifiAddress').textContent = status.ip ? 'http://' + status.ip + '/' : 'No address assigned';
    byId('networkSummary').textContent = status.lanIp ? 'LAN' : 'AP';
    byId('networkIp').textContent = status.ip || 'No address';
    byId('controllerCount').textContent = paired.length;
    byId('pairedBadge').textContent = paired.length + (paired.length === 1 ? ' paired' : ' paired');
    byId('lastUpdate').textContent = 'Updated ' + new Date().toLocaleTimeString([], {hour:'2-digit', minute:'2-digit', second:'2-digit'});
    byId('liveText').textContent = 'Live';
    byId('liveDot').className = 'dot online';
    const rssiInput = byId('rssiInput');
    if (document.activeElement !== rssiInput) rssiInput.value = status.rssiThreshold;
    if (!deviceConfigLoaded) {
      byId('wifiSsid').value = wifiConfig.ssid;
      byId('wifiPassword').placeholder = wifiConfig.hasPassword ? 'Saved password (leave blank to keep)' : 'Leave blank for an open network';
      byId('btSpoofMac').value = wifiConfig.classicBtSpoofMac || '';
      deviceConfigLoaded = true;
    }
    const pairedBox = byId('paired');
    pairedBox.replaceChildren();
    if (!paired.length) pairedBox.appendChild(empty('No controllers registered yet.'));
    paired.forEach(c => pairedBox.appendChild(controllerRow(c, 'Remove', 'danger', () => post('/api/remove?slot=' + c.slot, 'Controller removed'))));
    const foundBox = byId('found');
    foundBox.replaceChildren();
    if (!found.length) foundBox.appendChild(empty(status.scanning ? 'Listening for nearby controllers…' : 'Start a scan to find nearby controllers.'));
    found.forEach(c => foundBox.appendChild(controllerRow(c, 'Pair', '', () => post('/api/pair?mac=' + encodeURIComponent(c.mac), 'Controller paired'))));
    const logBox = byId('log');
    logBox.textContent = logs.length ? logs.join('\n') : 'No log entries yet.';
    logBox.scrollTop = logBox.scrollHeight;
  } catch (error) {
    console.error('Refresh failed', error);
    byId('liveText').textContent = 'Disconnected';
    byId('liveDot').className = 'dot offline';
    byId('lastUpdate').textContent = 'Refresh failed';
  } finally {
    refreshInFlight = false;
    if (refreshQueued) { refreshQueued = false; scheduleRefresh(); }
  }
}
setInterval(() => scheduleRefresh(), 1500);
scheduleRefresh();
</script>
</body>
</html>
)rawliteral";

    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html; charset=utf-8");
    client.println("Cache-Control: no-store");
    client.println("Connection: close");
    client.println();
    client.print(page);
}

void handleApiStatus(WiFiClient& client) {
    String json = "{\"pcOn\":";
    json += stablePcOn ? "true" : "false";
    json += ",\"powerState\":\"";
    json += powerStateName();
    json += "\",\"scanning\":";
    json += webScanActive ? "true" : "false";
    json += ",\"classicScanning\":";
    json += classicDiscoveryRunning ? "true" : "false";
    json += ",\"busy\":";
    json += powerState != POWER_IDLE ? "true" : "false";
    json += ",\"rssiThreshold\":";
    json += webScanRssiThreshold;
    json += ",\"ip\":\"";
    json += webIpAddress();
    json += "\",\"lanIp\":\"";
    json += (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "";
    json += "\",\"apIp\":\"";
    json += WiFi.softAPIP().toString();
    json += "\",\"wifi\":\"";
    json += wifiStatusName(WiFi.status());
    json += "\"}";
    sendJson(client, 200, json);
}

void handleApiRemove(WiFiClient& client, const String& query) {
    int slot = queryParam(query, "slot").toInt();
    String removedMac = (slot >= 0 && slot < MAX_CONTROLLERS) ? savedMACs[slot] : "";
    bool removed = removeController(slot);
    if (removed) {
        addLog(String("CONTROLLER - Removed: ") + removedMac);
    }
    sendJson(client, removed ? 200 : 404, removed ? "{\"ok\":true}" : "{\"ok\":false}");
}

void handleApiStartScan(WiFiClient& client) {
    clearFoundControllers();
    webScanActive = true;
    webScanStartTime = millis();
    lastBleScanTime = webScanStartTime - BLE_SCAN_INTERVAL_MS;
    lastClassicScanTime = webScanStartTime - CLASSIC_SCAN_INTERVAL_MS;
    addLog("CONTROLLER - Web scan started.");
    sendJson(client, 200, "{\"ok\":true}");
}

void handleApiRssi(WiFiClient& client, const String& query) {
    String value = queryParam(query, "value");
    if (value.length() == 0) {
        sendJson(client, 400, "{\"ok\":false,\"error\":\"missing_value\"}");
        return;
    }

    int threshold = value.toInt();
    saveRssiThreshold(threshold);
    addLog(String("CONTROLLER - Web scan RSSI threshold set to ") + String(webScanRssiThreshold) + " dBm");
    sendJson(client, 200, "{\"ok\":true}");
}

void handleApiPair(WiFiClient& client, const String& query) {
    String mac = queryParam(query, "mac");
    int foundIndex = foundControllerIndex(mac);
    if (mac.length() == 0 || foundIndex < 0) {
        addLog("CONTROLLER - Web pair rejected: controller was not in scan results.");
        sendJson(client, 400, "{\"ok\":false}");
        return;
    }

    saveController(mac);
    addLog(String("CONTROLLER - Web registered controller: ") + mac);
    sendJson(client, 200, "{\"ok\":true}");
}

void handleApiManualAdd(WiFiClient& client, const String& query) {
    String mac = normalizeMac(queryParam(query, "mac"));

    if (!isValidMac(mac)) {
        addLog(String("CONTROLLER - Rejected, invalid MAC: ") + mac);
        sendJson(client, 400, "{\"ok\":false,\"error\":\"invalid_mac\"}");
        return;
    }

    if (isKnownController(mac)) {
        addLog(String("CONTROLLER - Already registered: ") + mac);
        sendJson(client, 409, "{\"ok\":false,\"error\":\"already_paired\"}");
        return;
    }

    saveController(mac);
    addLog(String("CONTROLLER - Registered: ") + mac);
    sendJson(client, 200, "{\"ok\":true}");
}

void handleApiWiFiSave(WiFiClient& client, const String& query) {
    if (query.indexOf("ssid=") < 0) {
        sendJson(client, 400, "{\"ok\":false,\"error\":\"missing_ssid\"}");
        return;
    }

    String ssid = queryParam(query, "ssid");
    String password = queryParam(query, "password");
    bool clearPassword = queryParam(query, "clearPassword") == "1";
    bool replacePassword = clearPassword || password.length() > 0;
    if (clearPassword) password = "";

    if (ssid.length() > 32) {
        sendJson(client, 400, "{\"ok\":false,\"error\":\"invalid_ssid\"}");
        return;
    }
    if (replacePassword && (password.length() > 63 || (password.length() > 0 && password.length() < 8))) {
        sendJson(client, 400, "{\"ok\":false,\"error\":\"invalid_password\"}");
        return;
    }

    if (!saveWiFiSettings(ssid, password, replacePassword)) {
        sendJson(client, 400, "{\"ok\":false,\"error\":\"invalid_settings\"}");
        return;
    }

    addLog(String("WIFI - Settings saved for SSID: ") + (wifiSsid.length() > 0 ? wifiSsid : "(router WiFi disabled)"));
    // Defer the radio-mode change until after this HTTP response has left the
    // setup AP. The loop will make exactly one attempt with the new settings.
    wifiConnectRequested = true;
    sendJson(client, 200, wifiSsid.length() > 0
        ? "{\"ok\":true,\"connecting\":true}"
        : "{\"ok\":true,\"connecting\":false}");
}

void handleApiBluetoothSave(WiFiClient& client, const String& query) {
    String mac = normalizeMac(queryParam(query, "mac"));
    uint8_t address[6];

    if (mac.length() > 0 && (!isValidMac(mac) || !parseMacAddress(mac, address))) {
        sendJson(client, 400, "{\"ok\":false,\"error\":\"invalid_mac\"}");
        return;
    }

    classicBtSpoofMac = mac;
    preferences.begin(CONFIG_NAMESPACE, false);
    if (classicBtSpoofMac.length() > 0) {
        preferences.putString("bt_mac", classicBtSpoofMac);
    } else {
        preferences.remove("bt_mac");
    }
    preferences.end();

    addLog(classicBtSpoofMac.length() > 0
        ? String("Classic Bluetooth - Spoof MAC saved for next restart: ") + classicBtSpoofMac
        : "Classic Bluetooth - Spoofing disabled for next restart.");

    String json = "{\"ok\":true,\"mac\":\"";
    json += jsonEscape(classicBtSpoofMac);
    json += "\",\"restartRequired\":true}";
    sendJson(client, 200, json);
}

void handleApiRestart(WiFiClient& client) {
    if (stablePcOn || powerState != POWER_IDLE) {
        addLog("SYSTEM - Restart rejected while PC is on or power operation is active.");
        sendJson(client, 409, "{\"ok\":false,\"error\":\"restart_not_safe\"}");
        return;
    }

    addLog("SYSTEM - Restart requested from web portal.");
    sendJson(client, 200, "{\"ok\":true}");
    restartRequested = true;
    restartRequestedAt = millis();
}

void setupWebServer() {
    server.begin();
}

void setupWiFi() {
    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(WEB_HOSTNAME);

    setupWebServer();

    if (wifiSsid.length() == 0) {
        addLog("Router WiFi disabled: saved SSID is empty.");
        startSetupAp();
        return;
    }

    addLog(String("Trying router WiFi first, SSID: ") + wifiSsid);
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());

    Serial.print("Connecting WiFi");
    unsigned long wifiStartTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStartTime < WIFI_CONNECT_TIMEOUT_MS) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();

    wl_status_t status = WiFi.status();
    if (status != WL_CONNECTED) {
        addLog(String("Router WiFi failed: ") + wifiStatusName(status));
        startSetupAp();
        addLog("Portal is available on the setup AP.");
        return;
    }

    routerWifiWasConnected = true;
    addLog(String("Router WiFi connected. LAN webpage: http://") + WiFi.localIP().toString());
    addLog(String("Signal: ") + String(WiFi.RSSI()) + " dBm");
}

void handleWebServer() {
    WiFiClient client = server.available();
    if (!client) return;

    client.setTimeout(50);
    String requestLine = client.readStringUntil('\r');
    client.readStringUntil('\n');

    while (client.connected()) {
        String header = client.readStringUntil('\n');
        if (header == "\r" || header.length() == 0) break;
    }

    int firstSpace = requestLine.indexOf(' ');
    int secondSpace = requestLine.indexOf(' ', firstSpace + 1);
    if (firstSpace < 0 || secondSpace < 0) {
        client.stop();
        return;
    }

    String method = requestLine.substring(0, firstSpace);
    String target = requestLine.substring(firstSpace + 1, secondSpace);
    String query = "";
    int queryStart = target.indexOf('?');
    if (queryStart >= 0) {
        query = target.substring(queryStart + 1);
        target = target.substring(0, queryStart);
    }

    if (method == "GET" && target == "/") {
        handleWebRoot(client);
    } else if (method == "GET" && target == "/api/status") {
        handleApiStatus(client);
    } else if (method == "GET" && target == "/api/controllers") {
        sendJson(client, 200, controllersJson());
    } else if (method == "GET" && target == "/api/found") {
        sendJson(client, 200, foundControllersJson());
    } else if (method == "GET" && target == "/api/logs") {
        sendJson(client, 200, logsJson());
    } else if (method == "GET" && target == "/api/wifi") {
        sendJson(client, 200, wifiConfigJson());
    } else if (method == "POST" && target == "/api/remove") {
        handleApiRemove(client, query);
    } else if (method == "POST" && target == "/api/scan/start") {
        handleApiStartScan(client);
    } else if (method == "POST" && target == "/api/rssi") {
        handleApiRssi(client, query);
    } else if (method == "POST" && target == "/api/pair") {
        handleApiPair(client, query);
    } else if (method == "POST" && target == "/api/manual-add") {
        handleApiManualAdd(client, query);
    } else if (method == "POST" && target == "/api/wifi/save") {
        handleApiWiFiSave(client, query);
    } else if (method == "POST" && target == "/api/bluetooth/save") {
        handleApiBluetoothSave(client, query);
    } else if (method == "POST" && target == "/api/restart") {
        handleApiRestart(client);
    } else if (method == "POST" && target == "/api/power/on") {
        startAtxPowerOnSequence();
        sendJson(client, 200, "{\"ok\":true}");
    } else if (method == "POST" && target == "/api/power/off") {
        startNormalShutdown();
        sendJson(client, 200, "{\"ok\":true}");
    } else {
        sendJson(client, 404, "{\"ok\":false}");
    }

    delay(1);
    client.stop();
}

void monitorWiFi(unsigned long now) {
    if (wifiConnectRequested) {
        wifiConnectRequested = false;
        routerWifiWasConnected = false;

        if (wifiSsid.length() == 0) {
            routerWifiRetrying = false;
            WiFi.disconnect(false);
            startSetupAp();
            addLog("Router WiFi disabled. Setup AP will remain active.");
            return;
        }

        addLog(String("Trying saved router WiFi once, SSID: ") + wifiSsid);
        // Keep the setup AP alive while testing new credentials so the portal
        // remains reachable if the router connection fails.
        WiFi.mode(setupApRunning ? WIFI_AP_STA : WIFI_STA);
        WiFi.disconnect(false);
        WiFi.setHostname(WEB_HOSTNAME);
        WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
        routerWifiRetrying = true;
        wifiRetryStartedAt = now;
        return;
    }

    if (wifiSsid.length() == 0) return;

    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected) {
        if (!routerWifiWasConnected) {
            routerWifiWasConnected = true;
            routerWifiRetrying = false;
            addLog(String("Router WiFi reconnected. LAN webpage: http://") + WiFi.localIP().toString());
            addLog(String("Signal: ") + String(WiFi.RSSI()) + " dBm");
            stopSetupAp();
        }
        return;
    }

    if (routerWifiRetrying && now - wifiRetryStartedAt >= WIFI_CONNECT_TIMEOUT_MS) {
        routerWifiRetrying = false;
        addLog(String("Router WiFi attempt failed: ") + wifiStatusName(WiFi.status()));
        startSetupAp();
        addLog("Setup AP will remain active until WiFi settings are saved again.");
        return;
    }

    if (routerWifiWasConnected) {
        routerWifiWasConnected = false;
        addLog(String("Router WiFi dropped: ") + wifiStatusName(WiFi.status()));
        startSetupAp();
        addLog("Setup AP will remain active until WiFi settings are saved again.");
    }
}

// ================= SETUP =================
void setup() {
    Serial.begin(115200);
    delay(1000);

    pinMode(RELAY_PSU_PIN, OUTPUT);
    digitalWrite(RELAY_PSU_PIN, RELAY_OFF);

    pinMode(RELAY_PWR_PIN, OUTPUT);
    digitalWrite(RELAY_PWR_PIN, RELAY_OFF);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(PC_MONITOR_PIN, INPUT_PULLDOWN);

    // Read the initial PC state at boot and sync ATX PSU relay to it.
    bool initialPcState = digitalRead(PC_MONITOR_PIN) == HIGH;
    pcMonitor.raw = initialPcState;
    pcMonitor.stable = initialPcState;
    pcMonitor.changedAt = millis();
    stablePcOn = initialPcState;
    lastPcState = initialPcState;
    digitalWrite(RELAY_PSU_PIN, lastPcState ? RELAY_ON : RELAY_OFF);

    preferences.begin(CONFIG_NAMESPACE, true);
    currentSlot = preferences.getInt("slot", 0);
    webScanRssiThreshold = preferences.getInt("rssi", DEFAULT_WEB_SCAN_RSSI_THRESHOLD);
    classicBtSpoofMac = normalizeMac(preferences.getString("bt_mac", ""));
    if (webScanRssiThreshold < -100) webScanRssiThreshold = -100;
    if (webScanRssiThreshold > -20) webScanRssiThreshold = -20;

    uint8_t storedSpoofAddress[6];
    if (classicBtSpoofMac.length() > 0 &&
        (!isValidMac(classicBtSpoofMac) || !parseMacAddress(classicBtSpoofMac, storedSpoofAddress))) {
        addLog("Classic Bluetooth - Ignoring invalid saved spoof MAC.");
        classicBtSpoofMac = "";
    }

    addLog("Controllers - Loading saved...");
    int count = 0;
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
        savedMACs[i] = preferences.getString(("mac" + String(i)).c_str(), "");
        if (savedMACs[i] != "") {
            addLog(String("Saved slot ") + String(i + 1) + ": " + savedMACs[i]);
            count++;
        }
    }
    if (count == 0) addLog("Controllers - No saved controllers");
    preferences.end();

    // Bluetooth and WiFi initialize the shared radio below. The interface MAC
    // must be selected before either stack starts using it.
    applyClassicBluetoothSpoof();

    loadWiFiSettings();
    setupWiFi();

    BLEDevice::init("");
    bleScan = BLEDevice::getScan();
    bleScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    bleScan->setActiveScan(true);
    bleScan->setInterval(100);
    bleScan->setWindow(99);

    setupClassicBluetooth();
}

// ================= LOOP =================
void loop() {
    unsigned long now = millis();

    // Update stable inputs and advance any in-progress relay sequence before
    // reacting to BLE or button events.
    updatePcState();
    updateClassicPageScan();
    handlePowerState();
    syncAtxPsuRelay();
    handleShutdownTracking(now); // Turns off PSU after PC shutdown and starts 60s ignore timer
    handleButton(now);
    updateStatusLed(now);
    monitorWiFi(now);
    protectSetupApClient();
    handleWebServer();

    // Give the HTTP response time to leave before restarting. Restart is only
    // accepted while the PC is off so PS_ON cannot be interrupted here.
    if (restartRequested) {
        if (millis() - restartRequestedAt >= 500) ESP.restart();
        delay(10);
        return;
    }

    scanClassicBluetooth();
    scanBle();
}
