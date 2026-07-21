#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <string.h>
#include "mbedtls/sha256.h"
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
const char* WEB_AUTH_NAMESPACE = "web_auth";
const char* WEB_SESSION_COOKIE = "bc250_session";

String wifiSsid = DEFAULT_WIFI_SSID;
String wifiPassword = DEFAULT_WIFI_PASSWORD;
String webPasswordSalt = "";
String webPasswordHash = "";
String webSessionToken = "";
String classicBtSpoofMac = "";
bool bleControllerScanEnabled = true;
bool classicInquiryScanEnabled = true;
bool classicPairedScanEnabled = true;

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

// ================= MEMORY FOR 5 GAMEPADS =================
const char* CONFIG_NAMESPACE = "xbox_cfg";
const int MAX_CONTROLLERS = 5;
const int MAX_FOUND_CONTROLLERS = 10;
const int MAX_CONTROLLER_NICKNAME_LENGTH = 32;
String savedMACs[MAX_CONTROLLERS];
String savedControllerNicknames[MAX_CONTROLLERS];
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
bool mdnsRunning = false;
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

bool canWakeFromController(const String& deviceMAC, unsigned long now) {
    if (powerState != POWER_IDLE) return false;
    if (stablePcOn) return false;
    if (!isKnownController(deviceMAC)) return false;
    if (lastShutdownTime > 0 && now - lastShutdownTime < SHUTDOWN_COOLDOWN_MS) return false;
    if (now - lastWakeTime < WAKE_COOLDOWN_MS) return false;
    return true;
}

bool hasSavedController() {
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
        if (savedMACs[i].length() > 0) return true;
    }
    return false;
}

String normalizeControllerNickname(const String& value) {
    String nickname;
    nickname.reserve(value.length());
    for (unsigned int i = 0; i < value.length() && nickname.length() < MAX_CONTROLLER_NICKNAME_LENGTH; i++) {
        char c = value.charAt(i);
        if ((uint8_t)c >= 0x20 && c != 0x7f) nickname += c;
    }
    nickname.trim();
    return nickname;
}

void saveController(const String& deviceMAC, const String& nickname = "") {
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
    savedControllerNicknames[slotToUse] = normalizeControllerNickname(nickname);

    preferences.begin(CONFIG_NAMESPACE, false);
    preferences.putString(("mac" + String(slotToUse)).c_str(), deviceMAC);
    if (savedControllerNicknames[slotToUse].length() > 0) {
        preferences.putString(("name" + String(slotToUse)).c_str(), savedControllerNicknames[slotToUse]);
    } else {
        preferences.remove(("name" + String(slotToUse)).c_str());
    }

    currentSlot = (slotToUse + 1) % MAX_CONTROLLERS;
    preferences.putInt("slot", currentSlot);
    preferences.end();
}

bool saveControllerNickname(int slot, const String& nickname) {
    if (slot < 0 || slot >= MAX_CONTROLLERS || savedMACs[slot].length() == 0) return false;

    savedControllerNicknames[slot] = normalizeControllerNickname(nickname);
    preferences.begin(CONFIG_NAMESPACE, false);
    if (savedControllerNicknames[slot].length() > 0) {
        preferences.putString(("name" + String(slot)).c_str(), savedControllerNicknames[slot]);
    } else {
        preferences.remove(("name" + String(slot)).c_str());
    }
    preferences.end();
    return true;
}

bool removeController(int slot) {
    if (slot < 0 || slot >= MAX_CONTROLLERS) return false;
    if (savedMACs[slot].length() == 0) return false;

    savedMACs[slot] = "";
    savedControllerNicknames[slot] = "";
    preferences.begin(CONFIG_NAMESPACE, false);
    preferences.remove(("mac" + String(slot)).c_str());
    preferences.remove(("name" + String(slot)).c_str());
    preferences.end();
    return true;
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
        String label = deviceName.length() > 0 ? deviceName : "Classic Bluetooth";
        rememberFoundController(deviceMAC, label, rssi);
        return;
    }

    if (!canWakeFromController(deviceMAC, now)) return;

    addLog("Classic Bluetooth - Known controller detected! Turning on ATX PSU and PC...");
    startAtxPowerOnSequence();
    lastWakeTime = now;
}

void classicGapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
    if (event == ESP_BT_GAP_DISC_RES_EVT) {
        if (!classicInquiryScanEnabled) return;
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
               classicPairedScanEnabled &&
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
    bool shouldEnable = classicPairedScanEnabled && !stablePcOn;
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

    if (!classicInquiryScanEnabled) return;
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
        if (!canWakeFromController(deviceMAC, now)) return;

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

    if (!bleControllerScanEnabled) return;
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
        json += "\",\"name\":\"";
        json += jsonEscape(savedControllerNicknames[i]);
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

String bytesToHex(const uint8_t* bytes, size_t length) {
    static const char hex[] = "0123456789abcdef";
    String result;
    result.reserve(length * 2);
    for (size_t i = 0; i < length; i++) {
        result += hex[bytes[i] >> 4];
        result += hex[bytes[i] & 0x0f];
    }
    return result;
}

String randomHex(size_t byteCount) {
    String result;
    result.reserve(byteCount * 2);
    while (byteCount > 0) {
        uint32_t value = esp_random();
        size_t take = byteCount < sizeof(value) ? byteCount : sizeof(value);
        result += bytesToHex(reinterpret_cast<const uint8_t*>(&value), take);
        byteCount -= take;
    }
    return result;
}

String hashWebPassword(const String& password, const String& salt) {
    // Repeated salted SHA-256 avoids keeping the WebUI password itself in NVS.
    // The work factor also makes password guesses more expensive than one hash.
    const int rounds = 4096;
    uint8_t digest[32];
    mbedtls_sha256_context context;

    mbedtls_sha256_init(&context);
    mbedtls_sha256_starts(&context, 0);
    mbedtls_sha256_update(&context, reinterpret_cast<const unsigned char*>(salt.c_str()), salt.length());
    mbedtls_sha256_update(&context, reinterpret_cast<const unsigned char*>(password.c_str()), password.length());
    mbedtls_sha256_finish(&context, digest);

    for (int round = 1; round < rounds; round++) {
        mbedtls_sha256_starts(&context, 0);
        mbedtls_sha256_update(&context, digest, sizeof(digest));
        mbedtls_sha256_update(&context, reinterpret_cast<const unsigned char*>(password.c_str()), password.length());
        mbedtls_sha256_finish(&context, digest);
        if ((round & 0xff) == 0) yield();
    }
    mbedtls_sha256_free(&context);
    return bytesToHex(digest, sizeof(digest));
}

bool constantTimeEquals(const String& left, const String& right) {
    if (left.length() != right.length()) return false;
    uint8_t difference = 0;
    for (unsigned int i = 0; i < left.length(); i++) {
        difference |= static_cast<uint8_t>(left.charAt(i) ^ right.charAt(i));
    }
    return difference == 0;
}

bool webPasswordConfigured() {
    return webPasswordSalt.length() == 32 && webPasswordHash.length() == 64;
}

void loadWebAuthSettings() {
    preferences.begin(WEB_AUTH_NAMESPACE, true);
    webPasswordSalt = preferences.getString("salt", "");
    webPasswordHash = preferences.getString("hash", "");
    preferences.end();

    if (!webPasswordConfigured()) {
        webPasswordSalt = "";
        webPasswordHash = "";
    }
    webSessionToken = randomHex(32);
}

bool saveWebPassword(const String& password) {
    if (password.length() < 8 || password.length() > 64) return false;

    String salt = randomHex(16);
    String hash = hashWebPassword(password, salt);
    if (salt.length() != 32 || hash.length() != 64) return false;

    preferences.begin(WEB_AUTH_NAMESPACE, false);
    size_t saltBytes = preferences.putString("salt", salt);
    size_t hashBytes = preferences.putString("hash", hash);
    preferences.end();
    if (saltBytes == 0 || hashBytes == 0) return false;

    webPasswordSalt = salt;
    webPasswordHash = hash;
    webSessionToken = randomHex(32);
    return true;
}

bool verifyWebPassword(const String& password) {
    if (!webPasswordConfigured() || password.length() > 64) return false;
    return constantTimeEquals(hashWebPassword(password, webPasswordSalt), webPasswordHash);
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
    json += "\",\"bleScanEnabled\":";
    json += bleControllerScanEnabled ? "true" : "false";
    json += ",\"classicInquiryScanEnabled\":";
    json += classicInquiryScanEnabled ? "true" : "false";
    json += ",\"classicPairedScanEnabled\":";
    json += classicPairedScanEnabled ? "true" : "false";
    json += ",\"webPasswordEnabled\":";
    json += webPasswordConfigured() ? "true" : "false";
    json += "}";
    return json;
}

void setupMdns() {
    if (mdnsRunning) return;

    if (!MDNS.begin(WEB_HOSTNAME)) {
        addLog("mDNS - WARNING: responder failed to start.");
        return;
    }

    MDNS.addService("http", "tcp", 80);
    mdnsRunning = true;
    addLog(String("mDNS - Webpage: http://") + WEB_HOSTNAME + ".local/");
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
        setupMdns();
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

void sendResponse(WiFiClient& client, int code, const char* type, const String& body, const String* cookie) {
    client.print("HTTP/1.1 ");
    client.print(code);
    client.println(code == 200 ? " OK" : " Error");
    client.print("Content-Type: ");
    client.println(type);
    client.println("Cache-Control: no-store");
    if (cookie != nullptr) {
        client.print("Set-Cookie: ");
        client.println(*cookie);
    }
    client.println("Connection: close");
    client.println();
    client.print(body);
}

void sendJson(WiFiClient& client, int code, const String& json) {
    sendResponse(client, code, "application/json; charset=utf-8", json, nullptr);
}

void sendJsonWithCookie(WiFiClient& client, int code, const String& json, const String& cookie) {
    sendResponse(client, code, "application/json; charset=utf-8", json, &cookie);
}

String sessionCookieValue() {
    return String(WEB_SESSION_COOKIE) + "=" + webSessionToken + "; Path=/; HttpOnly; SameSite=Strict";
}

bool hasValidWebSession(const String& cookieHeader) {
    if (!webPasswordConfigured() || webSessionToken.length() != 64) return false;

    int start = 0;
    while (start < cookieHeader.length()) {
        int end = cookieHeader.indexOf(';', start);
        if (end < 0) end = cookieHeader.length();
        String cookie = cookieHeader.substring(start, end);
        cookie.trim();
        String prefix = String(WEB_SESSION_COOKIE) + "=";
        if (cookie.startsWith(prefix)) {
            return constantTimeEquals(cookie.substring(prefix.length()), webSessionToken);
        }
        start = end + 1;
    }
    return false;
}

void handleAuthPage(WiFiClient& client) {
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html; charset=utf-8");
    client.println("Cache-Control: no-store");
    client.println("Connection: close");
    client.println();
    client.print(F(R"rawliteral(
<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="theme-color" content="#0a1018"><title>BC250 Control Panel</title>
<style>
:root{color-scheme:dark;--blue:#4da3ff;--muted:#8291a5;--line:#263548}*{box-sizing:border-box}body{margin:0;min-height:100vh;display:grid;place-items:center;padding:20px;background:radial-gradient(circle at top,#16263a 0,#0a1018 48%);color:#edf3f9;font-family:Inter,system-ui,sans-serif}.card{width:min(100%,410px);padding:28px;border:1px solid var(--line);border-radius:16px;background:#101a27;box-shadow:0 22px 60px rgba(0,0,0,.38)}.mark{display:grid;place-items:center;width:46px;height:46px;margin-bottom:19px;border-radius:13px;background:#183657;color:var(--blue)}.mark svg{width:28px}h1{margin:0;font-size:1.35rem}p{margin:8px 0 22px;color:var(--muted);font-size:.84rem;line-height:1.55}label{display:block;margin:14px 0 7px;color:#bdc9d7;font-size:.78rem;font-weight:650}input{width:100%;min-height:44px;padding:10px 12px;border:1px solid #33465d;border-radius:10px;background:#0a121c;color:#f2f6fa;font:inherit}input:focus{outline:2px solid var(--blue);outline-offset:1px}button{width:100%;min-height:44px;margin-top:20px;border:0;border-radius:10px;background:#2588eb;color:white;font:700 .84rem inherit;cursor:pointer}button:disabled{opacity:.55;cursor:wait}.message{min-height:18px;margin-top:12px;color:#ff9e9e;font-size:.76rem}.hint{color:var(--muted);font-size:.72rem}</style></head><body><main class="card"><div class="mark" aria-hidden="true"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8"><path d="M7.5 8h9a4 4 0 0 1 3.8 2.8l1.3 4.2a2.4 2.4 0 0 1-4.1 2.3l-1.7-1.8H8.2l-1.7 1.8A2.4 2.4 0 0 1 2.4 15l1.3-4.2A4 4 0 0 1 7.5 8Z"/><path d="M8 11v4M6 13h4M15.5 12.5h.01M18 14h.01"/></svg></div>
)rawliteral"));
    client.print(F(R"rawliteral(<h1>Welcome back</h1><p>Enter the WebUI password to open the BC250 control panel.</p><form id="authForm"><label for="password">Password</label><input id="password" name="password" type="password" maxlength="64" autocomplete="current-password" required autofocus><button id="submit" type="submit">Sign in</button><div id="message" class="message" role="alert"></div></form><script>
const form=document.getElementById('authForm'),password=document.getElementById('password'),message=document.getElementById('message'),button=document.getElementById('submit');form.addEventListener('submit',async event=>{event.preventDefault();message.textContent='';button.disabled=true;try{const body='password='+encodeURIComponent(password.value);const response=await fetch('/api/auth/login',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body,cache:'no-store'});await response.json();if(!response.ok){message.textContent='Incorrect password.';password.select();return}location.replace('/')}catch(error){message.textContent='Could not contact the controller.'}finally{button.disabled=false}});
</script></main></body></html>)rawliteral"));
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

void handleAuthLogin(WiFiClient& client, const String& body) {
    String password = queryParam(body, "password");
    if (!verifyWebPassword(password)) {
        addLog("WEB - Rejected WebUI sign-in attempt.");
        sendJson(client, 401, "{\"ok\":false,\"error\":\"incorrect_password\"}");
        return;
    }

    webSessionToken = randomHex(32);
    addLog("WEB - WebUI sign-in successful.");
    sendJsonWithCookie(client, 200, "{\"ok\":true}", sessionCookieValue());
}

void handleAuthLogout(WiFiClient& client) {
    webSessionToken = randomHex(32);
    sendJsonWithCookie(client, 200, "{\"ok\":true}", String(WEB_SESSION_COOKIE) + "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
}

void handleWebPasswordSave(WiFiClient& client, const String& body) {
    String currentPassword = queryParam(body, "currentPassword");
    String newPassword = queryParam(body, "newPassword");
    bool removePassword = queryParam(body, "remove") == "1";

    if (webPasswordConfigured() && !verifyWebPassword(currentPassword)) {
        sendJson(client, 403, "{\"ok\":false,\"error\":\"incorrect_current_password\"}");
        return;
    }

    if (removePassword) {
        preferences.begin(WEB_AUTH_NAMESPACE, false);
        preferences.remove("salt");
        preferences.remove("hash");
        preferences.end();
        webPasswordSalt = "";
        webPasswordHash = "";
        webSessionToken = randomHex(32);
        addLog("WEB - Password protection disabled.");
        sendJsonWithCookie(client, 200, "{\"ok\":true,\"enabled\":false}", String(WEB_SESSION_COOKIE) + "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
        return;
    }

    if (!saveWebPassword(newPassword)) {
        sendJson(client, 400, "{\"ok\":false,\"error\":\"invalid_password\"}");
        return;
    }

    addLog("WEB - Password protection enabled or updated.");
    sendJsonWithCookie(client, 200, "{\"ok\":true,\"enabled\":true}", sessionCookieValue());
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
  <!-- Web Manifest for Android Home Screen Shortcuts -->
  <link rel="manifest" href='data:application/manifest+json,{
    "name": "BC250 Control Panel",
    "short_name": "BC250 CP",
    "start_url": "/",
    "display": "standalone",
    "icons": [{
        "src": "data:image/png;base64,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",
        "sizes": "192x192",
        "type": "image/png"
     }]
  }'>
  <!-- Standard Native Webkit Support for iOS Apple-Touch-Icon -->
  <link rel="apple-touch-icon" href="data:image/png;base64,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">
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
    .power-panel-head { align-items: center; margin-bottom: 0; }
    .power-panel-head .panel-title { flex-wrap: wrap; }
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
    .power-toggle { flex: 0 0 auto; width: 58px; min-height: 58px; padding: 0; border-color: #367ac5; border-radius: 50%; background: linear-gradient(135deg, #226fcb, #174b85); box-shadow: 0 8px 24px rgba(25, 104, 190, .26); }
    .power-toggle.online { border-color: rgba(255, 100, 112, .45); background: linear-gradient(135deg, #c94855, #922c37); box-shadow: 0 12px 30px rgba(201, 72, 85, .2); }
    .power-toggle svg { width: 23px; }
    .section-rule { height: 1px; background: var(--line); margin: 18px 0; }
    .row { display: flex; gap: 9px; align-items: center; flex-wrap: wrap; }
    .field-row { display: grid; grid-template-columns: minmax(0, 1fr) auto; gap: 9px; }
    .controller-add-row { grid-template-columns: minmax(150px, 1.2fr) minmax(120px, 1fr) auto; }
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
    .item-actions { display: flex; gap: 7px; }
    code { color: #aec4dc; font-family: "SFMono-Regular", Consolas, monospace; font-size: .72rem; }
    .empty { padding: 18px; border: 1px dashed #2c3c50; border-radius: 11px; color: var(--muted); text-align: center; font-size: .78rem; }
    .scan-options { display: grid; gap: 4px; }
    .scan-options .check-row { margin-top: 0; }
    .switch-row { display: flex; align-items: center; justify-content: space-between; gap: 18px; min-height: 60px; padding: 10px 11px; border-radius: 9px; cursor: pointer; transition: background .15s ease; }
    .switch-row:hover { background: #111d2a; }
    .switch-copy { display: grid; gap: 3px; min-width: 0; }
    .switch-copy strong { color: #d8e3ee; font-size: .8rem; }
    .switch-control { position: relative; flex: 0 0 auto; }
    .switch-control input { position: absolute; width: 1px; height: 1px; min-height: 0; margin: 0; opacity: 0; }
    .switch-track { display: block; position: relative; width: 48px; height: 26px; border: 1px solid #415166; border-radius: 999px; background: #202c3a; transition: background .18s ease, border-color .18s ease, box-shadow .18s ease; }
    .switch-track::after { content: ""; position: absolute; top: 3px; left: 3px; width: 18px; height: 18px; border-radius: 50%; background: #aab6c4; box-shadow: 0 2px 5px rgba(0,0,0,.35); transition: transform .18s ease, background .18s ease; }
    .switch-control input:checked + .switch-track { border-color: #3c8de9; background: #246cb9; box-shadow: 0 0 0 3px rgba(59, 139, 230, .09); }
    .switch-control input:checked + .switch-track::after { transform: translateX(22px); background: #fff; }
    .switch-control input:focus-visible + .switch-track { outline: 2px solid var(--blue); outline-offset: 3px; }
    .scan-panel-head { align-items: center; }
    .scan-panel-head .panel-title { flex-wrap: wrap; }
    .bluetooth-config .panel-head { margin-bottom: 16px; }
    .bluetooth-config .section-rule { margin: 16px 0; }
    .bluetooth-config .scan-options { margin: 0 11px; }
    .bluetooth-config .switch-row { min-height: 36px; padding: 0px 2px 0px 14px; }
    .bluetooth-config .switch-copy strong { color: #bdc9d6; font-weight: 500; }
    .config-heading { display: grid; gap: 4px; margin-bottom: 8px; padding: 0 11px; }
    .config-heading-title { display: flex; align-items: center; gap: 7px; color: #d8e3ee; font-size: .8rem; font-weight: 700; }
    .config-heading small { color: var(--muted); font-size: .69rem; line-height: 1.4; }
    .config-inline { display: grid; grid-template-columns: minmax(0, 1fr) auto; align-items: center; gap: 16px; padding: 0 11px; }
    .config-inline .config-heading { margin: 0; padding: 0; }
    .bluetooth-config .config-input-row { min-width: 280px; margin: 0; }
    .bluetooth-config .message { margin: 8px 11px 0; }
    button.config-action { min-width: 82px; min-height: 42px; padding: 9px 14px; }
    .network-status { display: flex; gap: 11px; align-items: center; padding: 12px; border: 1px solid #29384b; border-radius: 11px; background: #0c141e; }
    .network-status svg { width: 21px; color: var(--green); flex: 0 0 auto; }
    #wifiState { font-size: .82rem; font-weight: 700; }
    #wifiAddress { margin-top: 2px; font-size: .7rem; word-break: break-all; }
    .message { min-height: 18px; margin-top: 9px; color: var(--muted); font-size: .72rem; }
    .restart-row { display: flex; justify-content: space-between; gap: 12px; align-items: center; }
    .password-actions { display: grid; grid-template-columns: 1fr auto; gap: 9px; margin-top: 15px; }
    .password-actions button { width: 100%; }
    .log-panel { overflow: hidden; }
    .log-toolbar { padding: 14px 17px; border-bottom: 1px solid var(--line); display: flex; justify-content: space-between; align-items: center; }
    .terminal-dots { display: flex; gap: 5px; }
    .terminal-dots i { width: 7px; height: 7px; border-radius: 50%; background: #405064; }
    pre { margin: 0; min-height: 160px; max-height: 290px; padding: 16px 17px; overflow: auto; white-space: pre-wrap; word-break: break-word; color: #a7b8ca; background: #090f17; font: .7rem/1.55 "SFMono-Regular", Consolas, monospace; }
    .toast { position: fixed; right: 20px; bottom: 20px; z-index: 10; max-width: min(360px, calc(100% - 40px)); padding: 11px 14px; border: 1px solid #355072; border-radius: 10px; background: #152335; color: #e9f2fc; box-shadow: 0 14px 40px rgba(0,0,0,.35); font-size: .78rem; opacity: 0; transform: translateY(10px); pointer-events: none; transition: .2s ease; }
    .toast.show { opacity: 1; transform: translateY(0); }
    .header-actions { display: flex; align-items: center; gap: 9px; }
    footer { display: flex; justify-content: center; margin-top: 20px; }
    footer a { display: inline-flex; align-items: center; gap: 7px; color: var(--muted); font-size: .74rem; text-decoration: none; transition: color .15s ease; }
    footer a:hover { color: #dce8f5; }
    footer a:focus-visible { outline: 2px solid var(--blue); outline-offset: 4px; border-radius: 3px; }
    footer svg { width: 16px; height: 16px; fill: currentColor; }
    @media (max-width: 860px) {
      .overview { grid-template-columns: 1fr 1fr; }
      .layout { grid-template-columns: 1fr; }
    }
    @media (max-width: 680px) {
      .config-inline { grid-template-columns: 1fr; align-items: start; }
      .config-inline .config-heading { margin-bottom: 8px; }
      .bluetooth-config .config-input-row { width: 100%; min-width: 0; }
    }
    @media (max-width: 540px) {
      .shell { width: min(100% - 20px, 1180px); padding-top: 18px; }
      header { margin-bottom: 18px; }
      .overview { gap: 9px; }
      .metric { min-height: 98px; padding: 14px; }
      .metric-value { font-size: 1.08rem; }
      .panel { padding: 16px; }
      .control-button { min-height: 54px; }
      .power-toggle { width: 54px; min-height: 54px; }
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
      <div class="header-actions"><div class="live"><span id="liveDot" class="dot"></span><span id="liveText">Connecting</span></div><button id="logoutBtn" class="secondary compact" onclick="logout()" hidden>Log out</button></div>
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
          <div class="panel-head power-panel-head"><div><div class="panel-title"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 2v10M18.4 6.6a9 9 0 1 1-12.8 0"/></svg><h2>BC250 Power control</h2><span id="powerBadge" class="badge">Checking</span></div><div class="eyebrow">Operate the ATX PSU and BC250's power switch</div></div><button id="powerToggleBtn" class="control-button power-toggle" onclick="togglePower()" aria-label="Power on BC250" title="Power on BC250"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true"><path d="M12 2v10M18.4 6.6a9 9 0 1 1-12.8 0"/></svg></button></div>
        </section>

        <section class="card panel">
          <div class="panel-head"><div><div class="panel-title"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M6 9h12a4 4 0 0 1 3.8 5.2l-1 3a2 2 0 0 1-3.3.8l-2-2H8.5l-2 2a2 2 0 0 1-3.3-.8l-1-3A4 4 0 0 1 6 9Z"/><path d="M8 11v4M6 13h4"/></svg><h2>Registered controllers</h2></div><div class="eyebrow">Controllers allowed to trigger the system</div></div><span id="pairedBadge" class="badge">0 devices</span></div>
          <div id="paired"><div class="empty">Loading controllers…</div></div>
          <div class="section-rule"></div>
          <label class="field" for="manualMac">Add by Bluetooth address</label>
          <div class="field-row controller-add-row"><input id="manualMac" aria-label="Controller Bluetooth address" placeholder="aa:bb:cc:dd:ee:ff" maxlength="17"><input id="manualNickname" aria-label="Controller nickname" placeholder="Nickname (optional)" maxlength="32"><button onclick="manualAdd()">Add controller</button></div>
          <div class="hint">Use the manual address if a controller does not appear in a nearby scan.</div>
        </section>

        <section class="card panel">
          <div class="panel-head scan-panel-head"><div><div class="panel-title"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="11" cy="11" r="7"/><path d="m20 20-4-4"/></svg><h2>Discover controllers</h2><span id="scanState" class="badge">Idle</span></div><div class="eyebrow">Search for nearby BLE or Classic Bluetooth gamepads</div></div><button id="scanBtn" onclick="post('/api/scan/start', 'Controller scan started')">Scan</button></div>
          <div id="found"><div class="empty">Start a scan to find nearby controllers.</div></div>
        </section>

        <section class="card panel bluetooth-config">
          <div class="panel-head"><div><div class="panel-title"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="m7 7 10 10-5 5V2l5 5L7 17"/></svg><h2>Bluetooth config</h2></div><div class="eyebrow">Detection and Classic pairing options</div></div></div>
          <div class="config-heading"><div class="config-heading-title"><span>Controller detection methods</span><span class="info-tip" tabindex="0" aria-label="About controller detection methods"><span aria-hidden="true">i</span><span class="tooltip" role="tooltip">These options control both controller discovery and background detection used to turn on the PC.<br/><br/><b>Bluetooth Low Energy (BLE) mode</b> listens for BLE advertisements.<br/><b>Bluetooth Classic (pairing mode)</b> runs Classic inquiries while controllers are discoverable.<br/><b>Bluetooth Classic (paired to the spoofed address)</b> listens for reconnect attempts from controllers paired with the configured spoof address.<br/><br/>Disabling unused methods can reduce radio contention and improve detection time. Changes are saved immediately.</span></span></div><small>Choose how the ESP32 discovers and monitors controllers</small></div>
          <div class="scan-options" aria-label="Controller detection methods">
            <label class="switch-row" for="scanBleEnabled"><span class="switch-copy"><strong>Bluetooth Low Energy (BLE)</strong></span><span class="switch-control"><input id="scanBleEnabled" type="checkbox" role="switch" checked onchange="saveScanOptions()"><span class="switch-track" aria-hidden="true"></span></span></label>
            <label class="switch-row" for="scanClassicInquiryEnabled"><span class="switch-copy"><strong>Bluetooth Classic (pairing mode)</strong></span><span class="switch-control"><input id="scanClassicInquiryEnabled" type="checkbox" role="switch" checked onchange="saveScanOptions()"><span class="switch-track" aria-hidden="true"></span></span></label>
            <label class="switch-row" for="scanClassicPairedEnabled"><span class="switch-copy"><strong>Bluetooth Classic (paired to spoofed address)</strong></span><span class="switch-control"><input id="scanClassicPairedEnabled" type="checkbox" role="switch" checked onchange="saveScanOptions()"><span class="switch-track" aria-hidden="true"></span></span></label>
          </div>
          <div class="message" id="scanOptionsMessage">Changes are saved and applied immediately.</div>
          <div class="section-rule"></div>
          <div class="config-inline"><div class="config-heading"><div class="config-heading-title"><span>Bluetooth spoof address</span><span class="info-tip" tabindex="0" aria-label="About the Bluetooth spoof address"><span aria-hidden="true">i</span><span class="tooltip" role="tooltip">Set the MAC address of the BC250 Bluetooth adapter that the gamepads are already paired with. While the PC is off, the ESP32 uses it to notice a paired gamepad trying to reconnect.<br/><br/>Leave this empty to disable spoofing.<br/>This is not required for BLE gamepads.<br/><br/>You can get the adapter MAC address with the <i>`bluetoothctl list`</i> command.<br/><br/>A restart is required after changing it.</span></span></div><small>Used only for controllers paired with a Classic Bluetooth adapter</small></div><div class="field-row config-input-row"><input id="btSpoofMac" type="text" maxlength="17" autocomplete="off" placeholder="Disabled when empty" aria-label="Bluetooth spoof address"><button id="btSpoofSaveBtn" class="secondary config-action" onclick="saveBtSpoofMac()">Save</button></div></div>
          <div class="message" id="btSpoofMessage">Changes take effect after restarting the ESP32.</div>
        </section>

        <section class="card log-panel">
          <div class="log-toolbar"><div class="panel-title"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="m8 9 3 3-3 3M13 15h3"/><rect x="3" y="4" width="18" height="16" rx="2"/></svg><h2>Event log</h2></div><div class="terminal-dots" aria-hidden="true"><i></i><i></i><i></i></div></div>
          <pre id="log">Waiting for controller…</pre>
        </section>
      </div>

      <aside class="stack">
        <section class="card panel">
          <div class="panel-head"><div><div class="panel-title"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M5 12.5a10 10 0 0 1 14 0M8.5 16a5 5 0 0 1 7 0M12 20h.01"/></svg><h2>ESP32 Settings</h2></div><div class="eyebrow">Network, access and device settings</div></div></div>
          <div class="network-status"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M5 12.5a10 10 0 0 1 14 0M8.5 16a5 5 0 0 1 7 0M12 20h.01"/></svg><div><div id="wifiState">Connecting…</div><div class="muted" id="wifiAddress"></div></div></div>
          <div class="section-rule"></div>
		      <label class="field field-with-info" for="wifiSsid"><span>2.4 GHz network name (SSID)</span><span class="info-tip" tabindex="0" aria-label="About wifi"><span aria-hidden="true">i</span><span class="tooltip" role="tooltip">Set the WiFi details to connect on the local network instead of setting up an Access Point (AP).<br>Leave SSID empty for AP.</span></span></label>
          <input id="wifiSsid" type="text" maxlength="32" autocomplete="off" placeholder="Setup AP only when empty">
          <label class="field" for="wifiPassword">Network password</label>
          <input id="wifiPassword" type="password" maxlength="63" autocomplete="new-password" placeholder="Leave blank to keep saved password">
          <label class="row check-row" for="wifiOpen"><input id="wifiOpen" type="checkbox" onchange="openNetworkChanged()"><span>Open network (clear saved password)</span></label>
          <button id="wifiSaveBtn" style="width:100%;margin-top:15px" onclick="saveWifi()">Save network settings</button>
          <div class="message" id="wifiMessage">Saving settings starts one connection attempt.</div>
          <div class="section-rule"></div>
          <label class="field field-with-info" for="webPassword"><span>WebUI password</span><span class="info-tip" tabindex="0" aria-label="About the WebUI password"><span aria-hidden="true">i</span><span class="tooltip" role="tooltip">Optionally set a password to protect the WebUI.<br/>When enabled, the control panel and all API commands require sign-in.</span></span></label>
          <div id="currentWebPasswordRow" hidden><label class="field" for="currentWebPassword">Current password</label><input id="currentWebPassword" type="password" maxlength="64" autocomplete="current-password"></div>
          <label class="field" for="newWebPassword">New password</label>
          <input id="newWebPassword" type="password" minlength="8" maxlength="64" autocomplete="new-password" placeholder="8 to 64 characters">
          <label class="field" for="confirmWebPassword">Confirm new password</label>
          <input id="confirmWebPassword" type="password" minlength="8" maxlength="64" autocomplete="new-password">
          <div class="password-actions"><button id="webPasswordSaveBtn" onclick="saveWebPassword()">Enable password</button><button id="webPasswordRemoveBtn" class="danger" onclick="removeWebPassword()" hidden>Remove</button></div>
          <div class="message" id="webPasswordMessage">Enabled control panel and API protection.</div>
          <div class="section-rule"></div>
          <div class="restart-row"><div><div style="font-size:.8rem;font-weight:700">Restart ESP32</div><div class="hint">Available only while the PC is off.</div></div><button id="restartBtn" class="danger compact" onclick="restartDevice()">Restart</button></div>
        </section>
      </aside>
    </div>
    <footer><a href="https://github.com/GreatApo/BC250_ESP32_ATX_PSU" target="_blank" rel="noopener noreferrer" aria-label="View BC250 ESP32 ATX PSU on GitHub"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 .7a11.5 11.5 0 0 0-3.6 22.4c.6.1.8-.3.8-.6v-2.2c-3.4.7-4.1-1.4-4.1-1.4-.6-1.4-1.4-1.8-1.4-1.8-1.1-.8.1-.8.1-.8 1.3.1 1.9 1.3 1.9 1.3 1.1 1.9 2.9 1.4 3.6 1.1.1-.8.4-1.4.8-1.7-2.7-.3-5.6-1.4-5.6-5.7 0-1.3.5-2.3 1.2-3.1-.1-.3-.5-1.6.1-3.1 0 0 1-.3 3.2 1.2a11 11 0 0 1 5.8 0C15.1 5.7 16 6 16 6c.6 1.6.2 2.8.1 3.1.8.8 1.2 1.8 1.2 3.1 0 4.4-2.9 5.4-5.6 5.7.4.4.8 1.1.8 2.2v3.3c0 .3.2.7.8.6A11.5 11.5 0 0 0 12 .7Z"/></svg>GreatApo/BC250_ESP32_ATX_PSU</a></footer>
  </main>
  <div id="toast" class="toast" role="status" aria-live="polite"></div>

<script>
const byId = id => document.getElementById(id);
async function api(path) {
  const response = await fetch(path, { cache: 'no-store' });
  if (response.status === 401) { location.replace('/'); throw new Error('Authentication required'); }
  if (!response.ok) throw new Error(path + ' returned ' + response.status);
  return response.json();
}
let refreshInFlight = false;
let refreshQueued = false;
let deviceConfigLoaded = false;
let restarting = false;
let webPasswordEnabled = false;
let pcIsOn = false;
let scanStarted = false;
let toastTimer;
function notify(message) {
  const toast = byId('toast');
  toast.textContent = message;
  toast.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toast.classList.remove('show'), 2600);
}
async function logout() {
  try { await fetch('/api/auth/logout', { method: 'POST', cache: 'no-store' }); } finally { location.replace('/'); }
}
function scheduleRefresh(delayMs = 0) { setTimeout(refresh, delayMs); }
async function post(path, message) {
  try {
    const response = await fetch(path, { method: 'POST', cache: 'no-store' });
    if (!response.ok) {
      const result = await response.json().catch(() => ({}));
      if (result.error === 'no_scan_methods') {
        notify('Enable BLE or Bluetooth Classic (pairing mode) to scan. Spoof mode only listens for paired reconnects.');
        scheduleRefresh();
        return;
      }
      throw new Error('Request failed');
    }
    if (path === '/api/scan/start') scanStarted = true;
    if (message) notify(message);
  } catch (error) { notify('Command could not be sent'); }
  scheduleRefresh();
}
function powerOff() {
  if (confirm('Send a normal shutdown command to the PC?')) post('/api/power/off', 'Shutdown command sent');
}
function togglePower() {
  if (pcIsOn) powerOff();
  else post('/api/power/on', 'Power-on command sent');
}
async function manualAdd() {
  const mac = byId('manualMac').value.trim();
  const nickname = byId('manualNickname').value.trim();
  const response = await fetch('/api/manual-add?mac=' + encodeURIComponent(mac) + '&name=' + encodeURIComponent(nickname), { method: 'POST', cache: 'no-store' });
  if (response.ok) { byId('manualMac').value = ''; byId('manualNickname').value = ''; notify('Controller added'); }
  else { notify(response.status === 409 ? 'Controller is already registered' : 'Enter a valid Bluetooth address'); }
  scheduleRefresh();
}
async function saveScanOptions() {
  const ble = byId('scanBleEnabled').checked;
  const inquiry = byId('scanClassicInquiryEnabled').checked;
  const paired = byId('scanClassicPairedEnabled').checked;
  const message = byId('scanOptionsMessage');
  const path = '/api/scan/options?ble=' + (ble ? '1' : '0') + '&inquiry=' + (inquiry ? '1' : '0') + '&paired=' + (paired ? '1' : '0');
  message.textContent = 'Saving scan options…';
  try {
    const response = await fetch(path, { method: 'POST', cache: 'no-store' });
    if (!response.ok) throw new Error('Request failed');
    message.textContent = 'Saved and applied immediately.';
    notify('Controller scan options updated');
  } catch (error) {
    deviceConfigLoaded = false;
    message.textContent = 'Could not save controller scan options.';
    notify('Could not update scan options');
  }
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
function applyWebPasswordState(enabled) {
  webPasswordEnabled = enabled;
  byId('currentWebPasswordRow').hidden = !enabled;
  byId('webPasswordRemoveBtn').hidden = !enabled;
  byId('logoutBtn').hidden = !enabled;
  byId('webPasswordSaveBtn').textContent = enabled ? 'Change password' : 'Enable password';
}
async function saveWebPassword() {
  const wasEnabled = webPasswordEnabled;
  const currentPassword = byId('currentWebPassword').value;
  const newPassword = byId('newWebPassword').value;
  const confirmPassword = byId('confirmWebPassword').value;
  const message = byId('webPasswordMessage');
  if (newPassword.length < 8 || newPassword.length > 64) { message.textContent = 'New password must be 8 to 64 characters.'; return; }
  if (newPassword !== confirmPassword) { message.textContent = 'New passwords do not match.'; return; }
  message.textContent = webPasswordEnabled ? 'Changing password…' : 'Enabling password protection…';
  const body = 'currentPassword=' + encodeURIComponent(currentPassword) + '&newPassword=' + encodeURIComponent(newPassword);
  const response = await fetch('/api/auth/password', { method: 'POST', headers: {'Content-Type':'application/x-www-form-urlencoded'}, body, cache: 'no-store' });
  const result = await response.json();
  if (!response.ok) {
    message.textContent = result.error === 'incorrect_current_password' ? 'Current password is incorrect.' : 'Could not save the password.';
    return;
  }
  byId('currentWebPassword').value = '';
  byId('newWebPassword').value = '';
  byId('confirmWebPassword').value = '';
  applyWebPasswordState(true);
  message.textContent = 'Password protection is enabled.';
  notify(wasEnabled ? 'WebUI password updated' : 'WebUI password enabled');
}
async function removeWebPassword() {
  if (!confirm('Remove WebUI password protection? Anyone on the network will be able to use the control panel.')) return;
  const message = byId('webPasswordMessage');
  const currentPassword = byId('currentWebPassword').value;
  const body = 'currentPassword=' + encodeURIComponent(currentPassword) + '&remove=1';
  const response = await fetch('/api/auth/password', { method: 'POST', headers: {'Content-Type':'application/x-www-form-urlencoded'}, body, cache: 'no-store' });
  const result = await response.json();
  if (!response.ok) { message.textContent = result.error === 'incorrect_current_password' ? 'Enter the current password before removing protection.' : 'Could not remove the password.'; return; }
  byId('currentWebPassword').value = '';
  byId('newWebPassword').value = '';
  byId('confirmWebPassword').value = '';
  applyWebPasswordState(false);
  message.textContent = 'Password protection is disabled.';
  notify('WebUI password removed');
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
function controllerRow(controller, actionText, actionClass, action, secondaryActionText, secondaryAction) {
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
  const actions = document.createElement('div');
  actions.className = 'item-actions';
  if (secondaryActionText) {
    const secondaryButton = document.createElement('button');
    secondaryButton.className = 'secondary compact';
    secondaryButton.textContent = secondaryActionText;
    secondaryButton.onclick = secondaryAction;
    actions.appendChild(secondaryButton);
  }
  const button = document.createElement('button');
  button.className = (actionClass || 'secondary') + ' compact';
  button.textContent = actionText;
  button.onclick = action;
  actions.appendChild(button);
  row.append(label, actions);
  return row;
}
async function renameController(controller) {
  const nickname = prompt('Controller nickname (leave blank to remove):', controller.name || '');
  if (nickname === null) return;
  const response = await fetch('/api/nickname?slot=' + controller.slot + '&name=' + encodeURIComponent(nickname), { method: 'POST', cache: 'no-store' });
  notify(response.ok ? 'Controller nickname saved' : 'Could not save controller nickname');
  scheduleRefresh();
}
function formatState(value) {
  return (value || 'idle').replaceAll('_', ' ').replace(/\b\w/g, c => c.toUpperCase());
}
async function refresh() {
  if (refreshInFlight) { refreshQueued = true; return; }
  refreshInFlight = true;
  try {
    if (restarting) return;
    const foundRequest = scanStarted ? api('/api/found') : Promise.resolve([]);
    const [status, paired, found, logs, wifiConfig] = await Promise.all([api('/api/status'), api('/api/controllers'), foundRequest, api('/api/logs'), api('/api/wifi')]);
    const pc = byId('pc');
    pc.textContent = status.pcOn ? 'ON' : 'OFF';
    pc.className = status.pcOn ? 'on' : 'off';
    byId('powerState').textContent = formatState(status.powerState);
    byId('powerBadge').textContent = status.busy ? 'Operation active' : (status.pcOn ? 'System online' : 'System offline');
    byId('powerBadge').className = 'badge' + (status.pcOn ? ' active' : '');
    pcIsOn = status.pcOn;
    const powerToggle = byId('powerToggleBtn');
    powerToggle.className = 'control-button power-toggle' + (status.pcOn ? ' online' : '');
    powerToggle.disabled = status.busy;
    powerToggle.setAttribute('aria-label', status.pcOn ? 'Shut down BC250' : 'Power on BC250');
    powerToggle.title = status.pcOn ? 'Shut down BC250' : 'Power on BC250';
    byId('scanState').textContent = status.scanning ? 'Scanning…' : 'Idle';
    byId('scanState').className = 'badge' + (status.scanning ? ' active' : '');
    byId('activitySummary').textContent = status.scanning ? 'Scanning' : (status.busy ? 'Power task' : 'Idle');
    byId('restartBtn').disabled = status.pcOn || status.busy;
    byId('wifiState').textContent = formatState(status.wifi);
    byId('wifiAddress').textContent = status.ip ? 'http://' + status.ip + '/' : 'No address assigned';
    byId('networkSummary').textContent = status.lanIp ? 'LAN' : 'AP';
    byId('networkIp').textContent = status.ip || 'No address';
    byId('controllerCount').textContent = paired.length;
    byId('pairedBadge').textContent = paired.length + (paired.length === 1 ? ' device' : ' devices');
    byId('lastUpdate').textContent = 'Updated ' + new Date().toLocaleTimeString([], {hour:'2-digit', minute:'2-digit', second:'2-digit'});
    byId('liveText').textContent = 'Live';
    byId('liveDot').className = 'dot online';
    if (!deviceConfigLoaded) {
      byId('wifiSsid').value = wifiConfig.ssid;
      byId('wifiPassword').placeholder = wifiConfig.hasPassword ? 'Saved password (leave blank to keep)' : 'Leave blank for an open network';
      byId('btSpoofMac').value = wifiConfig.classicBtSpoofMac || '';
      byId('scanBleEnabled').checked = wifiConfig.bleScanEnabled;
      byId('scanClassicInquiryEnabled').checked = wifiConfig.classicInquiryScanEnabled;
      byId('scanClassicPairedEnabled').checked = wifiConfig.classicPairedScanEnabled;
      applyWebPasswordState(wifiConfig.webPasswordEnabled);
      deviceConfigLoaded = true;
    }
    const noScanMethods = !byId('scanBleEnabled').checked && !byId('scanClassicInquiryEnabled').checked && !byId('scanClassicPairedEnabled').checked;
    byId('scanBtn').disabled = status.scanning || noScanMethods;
    const pairedBox = byId('paired');
    pairedBox.replaceChildren();
    if (!paired.length) pairedBox.appendChild(empty('No controllers registered yet.'));
    paired.forEach(c => pairedBox.appendChild(controllerRow(c, 'Remove', 'danger', () => post('/api/remove?slot=' + c.slot, 'Controller removed'), 'Rename', () => renameController(c))));
    const foundBox = byId('found');
    foundBox.replaceChildren();
    if (!scanStarted) foundBox.appendChild(empty('Start a scan to find nearby controllers.'));
    else if (!found.length) foundBox.appendChild(empty(status.scanning ? 'Listening for nearby controllers…' : 'No controllers found in this scan.'));
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
    if (!bleControllerScanEnabled && !classicInquiryScanEnabled) {
        addLog("CONTROLLER - Web scan rejected: BLE and Classic pairing scans are disabled.");
        sendJson(client, 409, "{\"ok\":false,\"error\":\"no_scan_methods\"}");
        return;
    }

    clearFoundControllers();
    webScanActive = true;
    webScanStartTime = millis();
    lastBleScanTime = webScanStartTime - BLE_SCAN_INTERVAL_MS;
    lastClassicScanTime = webScanStartTime - CLASSIC_SCAN_INTERVAL_MS;
    addLog("CONTROLLER - Web scan started.");
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

    saveController(mac, foundControllers[foundIndex].name);
    addLog(String("CONTROLLER - Web registered controller: ") + mac);
    sendJson(client, 200, "{\"ok\":true}");
}

void handleApiManualAdd(WiFiClient& client, const String& query) {
    String mac = normalizeMac(queryParam(query, "mac"));
    String nickname = normalizeControllerNickname(queryParam(query, "name"));

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

    saveController(mac, nickname);
    addLog(String("CONTROLLER - Registered: ") + (nickname.length() > 0 ? nickname + " / " : "") + mac);
    sendJson(client, 200, "{\"ok\":true}");
}

void handleApiNickname(WiFiClient& client, const String& query) {
    String slotValue = queryParam(query, "slot");
    if (slotValue.length() != 1 || slotValue.charAt(0) < '0' || slotValue.charAt(0) > '9') {
        sendJson(client, 400, "{\"ok\":false,\"error\":\"invalid_slot\"}");
        return;
    }

    int slot = slotValue.toInt();
    String nickname = normalizeControllerNickname(queryParam(query, "name"));
    if (!saveControllerNickname(slot, nickname)) {
        sendJson(client, 404, "{\"ok\":false,\"error\":\"controller_not_found\"}");
        return;
    }

    String nicknameStatus = nickname.length() > 0 ? String("saved: ") + nickname : "removed";
    addLog(String("CONTROLLER - Nickname ") + nicknameStatus + " / " + savedMACs[slot]);
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

void handleApiScanOptionsSave(WiFiClient& client, const String& query) {
    String bleValue = queryParam(query, "ble");
    String inquiryValue = queryParam(query, "inquiry");
    String pairedValue = queryParam(query, "paired");
    if ((bleValue != "0" && bleValue != "1") ||
        (inquiryValue != "0" && inquiryValue != "1") ||
        (pairedValue != "0" && pairedValue != "1")) {
        sendJson(client, 400, "{\"ok\":false,\"error\":\"invalid_scan_options\"}");
        return;
    }

    bool wasClassicInquiryEnabled = classicInquiryScanEnabled;
    bool wasBleEnabled = bleControllerScanEnabled;
    bleControllerScanEnabled = bleValue == "1";
    classicInquiryScanEnabled = inquiryValue == "1";
    classicPairedScanEnabled = pairedValue == "1";

    if (!bleControllerScanEnabled && !classicInquiryScanEnabled && webScanActive) {
        webScanActive = false;
        addLog("CONTROLLER - Web scan stopped: BLE and Classic pairing scans are disabled.");
    }

    preferences.begin(CONFIG_NAMESPACE, false);
    preferences.putBool("scan_ble", bleControllerScanEnabled);
    preferences.putBool("scan_inquiry", classicInquiryScanEnabled);
    preferences.putBool("scan_paired", classicPairedScanEnabled);
    preferences.end();

    if (wasClassicInquiryEnabled && !classicInquiryScanEnabled && classicDiscoveryRunning && !classicDiscoveryCancelRequested) {
        if (esp_bt_gap_cancel_discovery() == ESP_OK) classicDiscoveryCancelRequested = true;
    }
    if (!wasBleEnabled && bleControllerScanEnabled) {
        lastBleScanTime = millis() - BLE_SCAN_INTERVAL_MS;
    }
    if (!wasClassicInquiryEnabled && classicInquiryScanEnabled) {
        lastClassicScanTime = millis() - CLASSIC_SCAN_INTERVAL_MS;
    }
    updateClassicPageScan();

    addLog(String("CONTROLLER - Scan options saved: BLE ") + (bleControllerScanEnabled ? "on" : "off") +
           ", Classic pairing " + (classicInquiryScanEnabled ? "on" : "off") +
           ", Classic paired " + (classicPairedScanEnabled ? "on" : "off"));
    sendJson(client, 200, "{\"ok\":true}");
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
    setupMdns();
    addLog(String("Router WiFi connected. LAN webpage: http://") + WiFi.localIP().toString());
    addLog(String("Signal: ") + String(WiFi.RSSI()) + " dBm");
}

void handleWebServer() {
    WiFiClient client = server.available();
    if (!client) return;

    client.setTimeout(100);
    String requestLine = client.readStringUntil('\r');
    client.readStringUntil('\n');

    String cookieHeader = "";
    String hostHeader = "";
    String originHeader = "";
    int contentLength = 0;
    while (client.connected()) {
        String header = client.readStringUntil('\n');
        if (header == "\r" || header.length() == 0) break;
        int colon = header.indexOf(':');
        if (colon > 0) {
            String name = header.substring(0, colon);
            String value = header.substring(colon + 1);
            value.trim();
            if (name.equalsIgnoreCase("Cookie")) cookieHeader = value;
            if (name.equalsIgnoreCase("Host")) hostHeader = value;
            if (name.equalsIgnoreCase("Origin")) originHeader = value;
            if (name.equalsIgnoreCase("Content-Length")) contentLength = value.toInt();
        }
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

    if (contentLength < 0 || contentLength > 256) {
        sendJson(client, 413, "{\"ok\":false,\"error\":\"request_too_large\"}");
        delay(1);
        client.stop();
        return;
    }

    String body = "";
    body.reserve(contentLength);
    unsigned long bodyStartedAt = millis();
    while ((int)body.length() < contentLength && millis() - bodyStartedAt < 500) {
        while (client.available() && (int)body.length() < contentLength) {
            body += static_cast<char>(client.read());
        }
        if ((int)body.length() < contentLength) delay(1);
    }

    // Browser requests that mutate state must come from this WebUI. Requests
    // without an Origin header remain available to local API clients.
    if (method == "POST" && originHeader.length() > 0 &&
        (hostHeader.length() == 0 || originHeader != String("http://") + hostHeader)) {
        sendJson(client, 403, "{\"ok\":false,\"error\":\"invalid_origin\"}");
        delay(1);
        client.stop();
        return;
    }

    bool authenticated = hasValidWebSession(cookieHeader);
    bool webAccessAllowed = !webPasswordConfigured() || authenticated;
    if (method == "GET" && target == "/") {
        if (webPasswordConfigured() && !authenticated) handleAuthPage(client);
        else handleWebRoot(client);
    } else if (method == "POST" && target == "/api/auth/login") {
        handleAuthLogin(client, body);
    } else if (!webAccessAllowed) {
        sendJson(client, 401, "{\"ok\":false,\"error\":\"authentication_required\"}");
    } else if (method == "POST" && target == "/api/auth/logout") {
        handleAuthLogout(client);
    } else if (method == "POST" && target == "/api/auth/password") {
        handleWebPasswordSave(client, body);
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
    } else if (method == "POST" && target == "/api/pair") {
        handleApiPair(client, query);
    } else if (method == "POST" && target == "/api/manual-add") {
        handleApiManualAdd(client, query);
    } else if (method == "POST" && target == "/api/nickname") {
        handleApiNickname(client, query);
    } else if (method == "POST" && target == "/api/wifi/save") {
        handleApiWiFiSave(client, query);
    } else if (method == "POST" && target == "/api/bluetooth/save") {
        handleApiBluetoothSave(client, query);
    } else if (method == "POST" && target == "/api/scan/options") {
        handleApiScanOptionsSave(client, query);
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
            setupMdns();
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
    classicBtSpoofMac = normalizeMac(preferences.getString("bt_mac", ""));
    bleControllerScanEnabled = preferences.getBool("scan_ble", true);
    classicInquiryScanEnabled = preferences.getBool("scan_inquiry", true);
    classicPairedScanEnabled = preferences.getBool("scan_paired", true);
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
        savedControllerNicknames[i] = normalizeControllerNickname(
            preferences.getString(("name" + String(i)).c_str(), "")
        );
        if (savedMACs[i] != "") {
            addLog(String("Saved slot ") + String(i + 1) + ": " +
                (savedControllerNicknames[i].length() > 0 ? savedControllerNicknames[i] + " / " : "") +
                savedMACs[i]);
            count++;
        }
    }
    if (count == 0) addLog("Controllers - No saved controllers");
    preferences.end();

    // Bluetooth and WiFi initialize the shared radio below. The interface MAC
    // must be selected before either stack starts using it.
    applyClassicBluetoothSpoof();

    loadWebAuthSettings();
    addLog(webPasswordConfigured()
        ? "WEB - Password protection enabled."
        : "WEB - Password protection disabled (optional in Settings).");
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
