#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <Preferences.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <string.h>
#include "mbedtls/sha256.h"
#include "esp_gap_bt_api.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

struct HttpRequest;

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
const size_t MAX_HTTP_LENGTH = 512;
const unsigned long HTTP_HEADER_DEADLINE_MS = 500;
const int MAX_WEB_SESSIONS = 4;
const unsigned long WEB_SESSION_IDLE_TIMEOUT_MS = 8UL * 60UL * 60UL * 1000UL;
const unsigned long MAX_LOGIN_BACKOFF_MS = 60000;

String wifiSsid = DEFAULT_WIFI_SSID;
String wifiPassword = DEFAULT_WIFI_PASSWORD;
String webPasswordSalt = "";
String webPasswordHash = "";
struct WebSession {
    String token;
    unsigned long lastSeenAt;
};
WebSession webSessions[MAX_WEB_SESSIONS];
int nextWebSessionSlot = 0;
uint8_t failedLoginCount = 0;
unsigned long loginBlockedUntil = 0;
unsigned long loginBackoffMs = 0;

const uint32_t WIFI_CONFIG_MAGIC = 0x42433235; // "BC25"
struct StoredWiFiConfig {
    uint32_t magic;
    char ssid[33];
    char password[64];
};
String classicBtSpoofMac = "";
bool bleControllerScanEnabled = true;
bool classicInquiryScanEnabled = true;
bool classicPairedScanEnabled = true;

WiFiServer server(80);
bool littleFsReady = false;

const int MAX_LOG_LINES = 40;
String logLines[MAX_LOG_LINES];
int logStart = 0;
int logCount = 0;
unsigned long logUpdatedAt = 0;

// ================= TIMING SETTINGS =================
const unsigned long WAKE_COOLDOWN_MS = 15000;           // Ignore controllers briefly after wakes up from BLE
const unsigned long SHUTDOWN_COOLDOWN_MS = 60000;       // Ignore controllers briefly after shutdown
const unsigned long WEB_SCAN_DURATION_MS = 15000;       // Stop webpage BLE scans after this long

const unsigned long POWER_OFF_HOLD_MS = 3000;           // Hold physical button this long to turn the PC off

const unsigned long CONFIRM_BLINK_MS = 100;             // LED on/off duration for 2-blink confirmations
const unsigned long BLINK_COOLDOWN_MS = 500;            // Skip status LED blinking briefly after a confirmation blink

const unsigned long SHUTDOWN_TIMEOUT_MS = 120000;        // Abort shutdown if the PC doesn't turn off within this time
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

const uint32_t CONTROLLER_CONFIG_MAGIC = 0x434E5431; // "CNT1"
struct StoredControllerConfig {
    uint32_t magic;
    uint8_t nextSlot;
    char macs[MAX_CONTROLLERS][18];
    char names[MAX_CONTROLLERS][MAX_CONTROLLER_NICKNAME_LENGTH + 1];
};

struct FoundController {
    String mac;
    String name;
    int rssi;
};

enum BluetoothEventType : uint8_t {
    BT_EVENT_BLE_DEVICE,
    BT_EVENT_CLASSIC_DEVICE,
    BT_EVENT_CLASSIC_PAIRED_DEVICE,
    BT_EVENT_CLASSIC_DISCOVERY_STARTED,
    BT_EVENT_CLASSIC_DISCOVERY_STOPPED
};

struct BluetoothEvent {
    BluetoothEventType type;
    char mac[18];
    char name[32];
    int rssi;
};

const int BLUETOOTH_EVENT_QUEUE_LENGTH = 24;
QueueHandle_t bluetoothEventQueue = nullptr;

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
    unsigned long now = millis();
    String line = String(now) + "ms - " + message;

    if (logCount < MAX_LOG_LINES) {
        logLines[(logStart + logCount) % MAX_LOG_LINES] = line;
        logCount++;
    } else {
        logLines[logStart] = line;
        logStart = (logStart + 1) % MAX_LOG_LINES;
    }

    // Ensure back-to-back entries still produce a distinct change marker.
    logUpdatedAt = now == logUpdatedAt ? logUpdatedAt + 1 : now;
    Serial.println(line);
}

bool debounceInput(DebouncedInput &input, bool rawState) {
    unsigned long now = millis();
    // Track raw edge changes first; only promote them to "stable" after the
    // input has stopped changing for PC_STABLE_DELAY_MS.
    if (rawState != input.raw) {
        // Raw state changed, reset the timer.
        input.raw = rawState;
        input.changedAt = now;
    }else{
        // Raw state is stable, check if it has been stable long enough
        if (now - input.changedAt >= PC_STABLE_DELAY_MS) {
            input.stable = rawState;
        }
    }

    return input.stable;
}

void updatePcState(unsigned long now) {
    bool newPcState = debounceInput(pcMonitor, digitalRead(PC_MONITOR_PIN) == HIGH);
    if (newPcState == stablePcOn) return;

    stablePcOn = newPcState;

    if (stablePcOn) {
        addLog("STATE - PC ON");

    } else {
        // PC just shutdown, start the 60s ignore timer for controller wake-ups.
        lastShutdownTime = now;
        addLog("STATE - PC OFF - Activating 60s gamepad ignore timer...");
    };
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

bool startAtxPowerOnSequence() {
    // Only one relay sequence should run at a time. This keeps repeated BLE
    // adverts or button presses from stacking extra power-button pulses.
    if (stablePcOn) {
        addLog("STATE - PC already on.");
        return false;
    }

    if (powerState != POWER_IDLE) {
        addLog("STATE - Power operation already in progress (power-on aborted).");
        return false;
    }

    addLog("STATE - Setting PSU ON");
    digitalWrite(RELAY_PSU_PIN, RELAY_ON);
    powerState = POWER_ON_WAIT_FOR_PSU;
    powerStateStartedAt = millis();
    return true;
}

bool startNormalShutdown() {
    if (!stablePcOn) {
        addLog("STATE - PC already off.");
        return false;
    }

    if (powerState != POWER_IDLE) {
        addLog("STATE - Power operation already in progress (shutdown aborted).");
        return false;
    }

    addLog("STATE - Pulsing motherboard power button for normal shutdown...");
    // Arm the controller wake lockout immediately. Waiting until the monitor
    // pin reports OFF leaves a window where controller adverts can restart the PC.
    digitalWrite(RELAY_PWR_PIN, RELAY_ON);
    powerState = POWER_OFF_PRESS_BUTTON;
    powerStateStartedAt = millis();
    lastShutdownTime = powerStateStartedAt;
    return true;
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
            } else if (now - powerStateStartedAt >= SHUTDOWN_TIMEOUT_MS) {
                addLog("STATE - WARNING: Shutdown timed out, reverting relays.");
                digitalWrite(RELAY_PSU_PIN, RELAY_OFF);
                powerState = POWER_IDLE;
            } else {
                powerStateStartedAt = now;
            }
            break;

        default:
            break;
    }

    // Sync ATX PSU relay
    if (powerState == POWER_IDLE) {
        digitalWrite(RELAY_PSU_PIN, stablePcOn ? RELAY_ON : RELAY_OFF);
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

bool persistControllerSettings() {
    StoredControllerConfig stored = {};
    stored.magic = CONTROLLER_CONFIG_MAGIC;
    stored.nextSlot = static_cast<uint8_t>(currentSlot);
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
        strlcpy(stored.macs[i], savedMACs[i].c_str(), sizeof(stored.macs[i]));
        strlcpy(stored.names[i], savedControllerNicknames[i].c_str(), sizeof(stored.names[i]));
    }

    preferences.begin(CONFIG_NAMESPACE, false);
    size_t written = preferences.putBytes("controllers_v1", &stored, sizeof(stored));
    StoredControllerConfig verified = {};
    bool saved = written == sizeof(stored) &&
        preferences.getBytes("controllers_v1", &verified, sizeof(verified)) == sizeof(verified) &&
        memcmp(&stored, &verified, sizeof(stored)) == 0;
    if (saved) {
        preferences.remove("slot");
        for (int i = 0; i < MAX_CONTROLLERS; i++) {
            preferences.remove(("mac" + String(i)).c_str());
            preferences.remove(("name" + String(i)).c_str());
        }
    }
    preferences.end();
    return saved;
}

bool saveController(const String& deviceMAC, const String& nickname = "") {
    if (isKnownController(deviceMAC)) return true;

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
    String previousMac = savedMACs[slotToUse];
    String previousName = savedControllerNicknames[slotToUse];
    int previousSlot = currentSlot;
    savedMACs[slotToUse] = deviceMAC;
    savedControllerNicknames[slotToUse] = normalizeControllerNickname(nickname);
    currentSlot = (slotToUse + 1) % MAX_CONTROLLERS;
    if (!persistControllerSettings()) {
        savedMACs[slotToUse] = previousMac;
        savedControllerNicknames[slotToUse] = previousName;
        currentSlot = previousSlot;
        return false;
    }
    return true;
}

bool saveControllerNickname(int slot, const String& nickname) {
    if (slot < 0 || slot >= MAX_CONTROLLERS || savedMACs[slot].length() == 0) return false;

    String previousName = savedControllerNicknames[slot];
    savedControllerNicknames[slot] = normalizeControllerNickname(nickname);
    if (!persistControllerSettings()) {
        savedControllerNicknames[slot] = previousName;
        return false;
    }
    return true;
}

bool removeController(int slot) {
    if (slot < 0 || slot >= MAX_CONTROLLERS) return false;
    if (savedMACs[slot].length() == 0) return false;

    String previousMac = savedMACs[slot];
    String previousName = savedControllerNicknames[slot];
    savedMACs[slot] = "";
    savedControllerNicknames[slot] = "";
    if (!persistControllerSettings()) {
        savedMACs[slot] = previousMac;
        savedControllerNicknames[slot] = previousName;
        return false;
    }
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
        uint8_t c = static_cast<uint8_t>(value.charAt(i));
        if (c == '"' || c == '\\') {
            escaped += '\\';
            escaped += static_cast<char>(c);
        } else if (c == '\n') {
            escaped += "\\n";
        } else if (c == '\r') {
            escaped += "\\r";
        } else if (c == '\t') {
            escaped += "\\t";
        } else if (c < 0x20) {
            char unicodeEscape[7];
            snprintf(unicodeEscape, sizeof(unicodeEscape), "\\u%04x", c);
            escaped += unicodeEscape;
        } else {
            escaped += static_cast<char>(c);
        }
    }
    return escaped;
}

String logsJson() {
    String json = "{\"updatedAt\":";
    json += String(logUpdatedAt);
    json += ",\"lines\":[";
    for (int i = 0; i < logCount; i++) {
        if (i > 0) json += ",";
        int index = (logStart + i) % MAX_LOG_LINES;
        json += "\"";
        json += jsonEscape(logLines[index]);
        json += "\"";
    }
    json += "]}";
    return json;
}

void blinkConfirmation() {
    for (int i = 0; i < 2; i++) {
        digitalWrite(LED_PIN, HIGH);
        delay(CONFIRM_BLINK_MS);
        digitalWrite(LED_PIN, LOW);
        delay(CONFIRM_BLINK_MS);
    }
    ledBlinkState = LOW;
    lastBlinkTime = millis();
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

void queueBluetoothEvent(BluetoothEventType type, const String& mac,
                         const String& name, int rssi) {
    if (bluetoothEventQueue == nullptr) return;

    BluetoothEvent queuedEvent = {};
    queuedEvent.type = type;
    queuedEvent.rssi = rssi;
    strlcpy(queuedEvent.mac, mac.c_str(), sizeof(queuedEvent.mac));
    strlcpy(queuedEvent.name, name.c_str(), sizeof(queuedEvent.name));
    xQueueSend(bluetoothEventQueue, &queuedEvent, 0);
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
    if (startAtxPowerOnSequence()) lastWakeTime = now;
}

void classicGapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
    if (event == ESP_BT_GAP_DISC_RES_EVT) {
        String deviceMAC = classicAddressToString(param->disc_res.bda);
        String deviceName = "";
        int rssi = -127;

        parseClassicProperties(param->disc_res.num_prop, param->disc_res.prop, deviceName, rssi);
        queueBluetoothEvent(BT_EVENT_CLASSIC_DEVICE, deviceMAC, deviceName, rssi);
    } else if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT) {
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            queueBluetoothEvent(BT_EVENT_CLASSIC_DISCOVERY_STARTED, "", "", -127);
        } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            queueBluetoothEvent(BT_EVENT_CLASSIC_DISCOVERY_STOPPED, "", "", -127);
        }
    } else if (event == ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT &&
               param->acl_conn_cmpl_stat.stat == ESP_BT_STATUS_SUCCESS) {
        // A gamepad paired to the PC pages the PC adapter's address instead of
        // entering inquiry/discoverable mode. With that address spoofed, the
        // incoming ACL attempt exposes the gamepad address here even though
        // authentication will normally fail without the PC's link key.
        String deviceMAC = classicAddressToString(param->acl_conn_cmpl_stat.bda);
        queueBluetoothEvent(BT_EVENT_CLASSIC_PAIRED_DEVICE, deviceMAC, "Paired Classic device", -127);
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
        String deviceName = advertisedDevice.haveName()
            ? String(advertisedDevice.getName().c_str())
            : "";
        queueBluetoothEvent(BT_EVENT_BLE_DEVICE, deviceMAC, deviceName,
                            advertisedDevice.getRSSI());
    }
};

void handleBleDevice(const String& deviceMAC, const String& deviceName, int rssi) {
    unsigned long now = millis();
    if (webScanActive) {
        rememberFoundController(deviceMAC, deviceName, rssi);
        return;
    }
    if (!canWakeFromController(deviceMAC, now)) return;

    addLog("BLE - Known controller detected! Turning on ATX PSU and PC...");
    if (startAtxPowerOnSequence()) lastWakeTime = now;
}

void processBluetoothEvents() {
    if (bluetoothEventQueue == nullptr) return;

    BluetoothEvent event;
    while (xQueueReceive(bluetoothEventQueue, &event, 0) == pdTRUE) {
        if (event.type == BT_EVENT_CLASSIC_DISCOVERY_STARTED) {
            classicDiscoveryRunning = true;
        } else if (event.type == BT_EVENT_CLASSIC_DISCOVERY_STOPPED) {
            classicDiscoveryRunning = false;
            classicDiscoveryCancelRequested = false;
        } else if (event.type == BT_EVENT_CLASSIC_DEVICE) {
            if (classicInquiryScanEnabled) {
                handleClassicDevice(String(event.mac), String(event.name), event.rssi);
            }
        } else if (event.type == BT_EVENT_CLASSIC_PAIRED_DEVICE) {
            if (classicBtSpoofEnabled && classicPairedScanEnabled) {
                handleClassicDevice(String(event.mac), String(event.name), event.rssi);
            }
        } else if (event.type == BT_EVENT_BLE_DEVICE) {
            handleBleDevice(String(event.mac), String(event.name), event.rssi);
        }
    }
}

void updateStatusLed(unsigned long now) {
    // Do not blink immediately after a blink.
    if (now - lastBlinkTime < BLINK_COOLDOWN_MS) return;

    lastBlinkTime = now;
    ledBlinkState = !ledBlinkState;
    digitalWrite(LED_PIN, ledBlinkState);
}

void handleButton(unsigned long now) {
    bool buttonPressed = buttonIsPressed();

    if (buttonPressed) {
        if (buttonPressStartTime == 0) {
            buttonPressStartTime = now;
            shutdownPress = false;
        }

        unsigned long pressDuration = now - buttonPressStartTime;

        // Skip if long press has already been handled.
        if (!shutdownPress) {
            // Check if long press
            if (pressDuration >= POWER_OFF_HOLD_MS) {
                if(powerState == POWER_IDLE){
                    blinkConfirmation();
                    addLog("BUTTON - LONG PRESS (" + String(pressDuration) + "ms) - Normal shutdown.");
                    startNormalShutdown();
                    shutdownPress = true;
                }else{
                    addLog("BUTTON PRESSED (" + String(pressDuration) + "ms) - Power operation already in progress...");
                }
            }else{
                addLog("BUTTON PRESSED (" + String(pressDuration) + "ms) - No action.");
            }
        }

    } else {
        if (buttonPressStartTime > 0) {
            unsigned long pressDuration = now - buttonPressStartTime;

            if (pressDuration < POWER_OFF_HOLD_MS) {
                // Short press detected
                blinkConfirmation();
                if (startAtxPowerOnSequence()) {
                    addLog("BUTTON RELEASED (" + String(pressDuration) + "ms) - Power on.");
                } else {
                    addLog("BUTTON RELEASED (" + String(pressDuration) + "ms) - Power on rejected (PC already on or operation in progress).");
                }
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
        difference |= static_cast<uint8_t>(static_cast<unsigned char>(left.charAt(i)) ^ static_cast<unsigned char>(right.charAt(i)));
    }
    return difference == 0;
}

bool webPasswordConfigured() {
    return webPasswordSalt.length() == 32 && webPasswordHash.length() == 64;
}

void clearWebSessions() {
    for (int i = 0; i < MAX_WEB_SESSIONS; i++) {
        webSessions[i].token = "";
        webSessions[i].lastSeenAt = 0;
    }
    nextWebSessionSlot = 0;
}

String createWebSession() {
    String token = randomHex(32);
    webSessions[nextWebSessionSlot].token = token;
    webSessions[nextWebSessionSlot].lastSeenAt = millis();
    nextWebSessionSlot = (nextWebSessionSlot + 1) % MAX_WEB_SESSIONS;
    return token;
}

void loadWebAuthSettings() {
    preferences.begin(WEB_AUTH_NAMESPACE, true);
    String credential = preferences.getString("credential_v1", "");
    if (credential.startsWith("v1:") && credential.length() == 100 &&
        credential.charAt(35) == ':') {
        webPasswordSalt = credential.substring(3, 35);
        webPasswordHash = credential.substring(36);
    } else {
        // Backward-compatible read for installations created before the
        // combined credential record was introduced.
        webPasswordSalt = preferences.getString("salt", "");
        webPasswordHash = preferences.getString("hash", "");
    }
    preferences.end();

    if (!webPasswordConfigured()) {
        webPasswordSalt = "";
        webPasswordHash = "";
    }
    clearWebSessions();
}

bool saveWebPassword(const String& password) {
    if (password.length() < 8 || password.length() > 64) return false;

    String salt = randomHex(16);
    String hash = hashWebPassword(password, salt);
    if (salt.length() != 32 || hash.length() != 64) return false;

    String credential = String("v1:") + salt + ":" + hash;
    preferences.begin(WEB_AUTH_NAMESPACE, false);
    size_t written = preferences.putString("credential_v1", credential);
    bool verified = written > 0 && preferences.getString("credential_v1", "") == credential;
    if (verified) {
        preferences.remove("salt");
        preferences.remove("hash");
    }
    preferences.end();
    if (!verified) return false;

    webPasswordSalt = salt;
    webPasswordHash = hash;
    return true;
}

bool verifyWebPassword(const String& password) {
    if (!webPasswordConfigured() || password.length() > 64) return false;
    return constantTimeEquals(hashWebPassword(password, webPasswordSalt), webPasswordHash);
}

void loadWiFiSettings() {
    preferences.begin(WIFI_CONFIG_NAMESPACE, true);
    StoredWiFiConfig stored = {};
    bool loadedCombined = preferences.getBytesLength("config_v1") == sizeof(stored) &&
        preferences.getBytes("config_v1", &stored, sizeof(stored)) == sizeof(stored) &&
        stored.magic == WIFI_CONFIG_MAGIC;
    if (loadedCombined) {
        stored.ssid[sizeof(stored.ssid) - 1] = '\0';
        stored.password[sizeof(stored.password) - 1] = '\0';
        wifiSsid = String(stored.ssid);
        wifiPassword = String(stored.password);
    } else {
        wifiSsid = preferences.getString("ssid", DEFAULT_WIFI_SSID);
        wifiPassword = preferences.getString("password", DEFAULT_WIFI_PASSWORD);
    }
    preferences.end();
}

bool saveWiFiSettings(const String& ssid, const String& password, bool replacePassword) {
    if (ssid.length() > 32) return false;
    if (replacePassword && password.length() > 63) return false;
    if (replacePassword && password.length() > 0 && password.length() < 8) return false;

    String candidatePassword = replacePassword ? password : wifiPassword;
    StoredWiFiConfig stored = {};
    stored.magic = WIFI_CONFIG_MAGIC;
    strlcpy(stored.ssid, ssid.c_str(), sizeof(stored.ssid));
    strlcpy(stored.password, candidatePassword.c_str(), sizeof(stored.password));

    preferences.begin(WIFI_CONFIG_NAMESPACE, false);
    size_t written = preferences.putBytes("config_v1", &stored, sizeof(stored));
    StoredWiFiConfig verified = {};
    bool saved = written == sizeof(stored) &&
        preferences.getBytes("config_v1", &verified, sizeof(verified)) == sizeof(verified) &&
        memcmp(&stored, &verified, sizeof(stored)) == 0;
    if (saved) {
        preferences.remove("ssid");
        preferences.remove("password");
    }
    preferences.end();
    if (!saved) return false;

    wifiSsid = ssid;
    wifiPassword = candidatePassword;
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

void sendFileAsset(WiFiClient& client, const char* path, const char* contentType, const char* cacheControl) {
    if (!littleFsReady) {
        sendResponse(client, 503, "text/plain; charset=utf-8",
                     "WebUI filesystem is not available.", nullptr);
        return;
    }

    File file = LittleFS.open(path, "r");
    if (!file || file.isDirectory()) {
        sendResponse(client, 404, "text/plain; charset=utf-8", "Asset not found.", nullptr);
        return;
    }

    client.println("HTTP/1.1 200 OK");
    client.print("Content-Type: ");
    client.println(contentType);
    client.print("Content-Length: ");
    client.println(file.size());
    client.print("Cache-Control: ");
    client.println(cacheControl);
    client.println("Connection: close");
    client.println();

    uint8_t buffer[512];
    while (file.available() && client.connected()) {
        size_t bytesRead = file.read(buffer, sizeof(buffer));
        if (bytesRead == 0 || client.write(buffer, bytesRead) != bytesRead) break;
    }
    file.close();
}

String sessionCookieValue(const String& token) {
    return String(WEB_SESSION_COOKIE) + "=" + token + "; Path=/; HttpOnly; SameSite=Strict";
}

String webSessionTokenFromCookie(const String& cookieHeader) {
    int start = 0;
    while (start < cookieHeader.length()) {
        int end = cookieHeader.indexOf(';', start);
        if (end < 0) end = cookieHeader.length();
        String cookie = cookieHeader.substring(start, end);
        cookie.trim();
        String prefix = String(WEB_SESSION_COOKIE) + "=";
        if (cookie.startsWith(prefix)) {
            return cookie.substring(prefix.length());
        }
        start = end + 1;
    }
    return "";
}

bool hasValidWebSession(const String& cookieHeader) {
    if (!webPasswordConfigured()) return false;

    String token = webSessionTokenFromCookie(cookieHeader);
    if (token.length() != 64) return false;
    unsigned long now = millis();
    for (int i = 0; i < MAX_WEB_SESSIONS; i++) {
        if (webSessions[i].token.length() != 64) continue;
        if (now - webSessions[i].lastSeenAt >= WEB_SESSION_IDLE_TIMEOUT_MS) {
            webSessions[i].token = "";
            webSessions[i].lastSeenAt = 0;
            continue;
        }
        if (constantTimeEquals(token, webSessions[i].token)) {
            webSessions[i].lastSeenAt = now;
            return true;
        }
    }
    return false;
}

void invalidateWebSession(const String& cookieHeader) {
    String token = webSessionTokenFromCookie(cookieHeader);
    if (token.length() != 64) return;
    for (int i = 0; i < MAX_WEB_SESSIONS; i++) {
        if (webSessions[i].token.length() == 64 &&
            constantTimeEquals(token, webSessions[i].token)) {
            webSessions[i].token = "";
            webSessions[i].lastSeenAt = 0;
            return;
        }
    }
}

void handleAuthPage(WiFiClient& client) {
    sendFileAsset(client, "/auth.html", "text/html; charset=utf-8", "no-store");
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
    unsigned long now = millis();
    if (loginBlockedUntil != 0 && now - loginBlockedUntil < loginBackoffMs) {
        sendJson(client, 429, "{\"ok\":false,\"error\":\"rate_limited\"}");
        return;
    }

    String password = queryParam(body, "password");
    if (!verifyWebPassword(password)) {
        if (failedLoginCount < 10) failedLoginCount++;
        if (failedLoginCount >= 3) {
            uint8_t exponent = failedLoginCount - 3;
            if (exponent > 5) exponent = 5;
            unsigned long backoff = 2000UL << exponent;
            if (backoff > MAX_LOGIN_BACKOFF_MS) backoff = MAX_LOGIN_BACKOFF_MS;
            loginBlockedUntil = now;
            loginBackoffMs = backoff;
        }
        addLog("WEB - Rejected WebUI sign-in attempt.");
        sendJson(client, 401, "{\"ok\":false,\"error\":\"incorrect_password\"}");
        return;
    }

    failedLoginCount = 0;
    loginBlockedUntil = 0;
    loginBackoffMs = 0;
    String sessionToken = createWebSession();
    addLog("WEB - WebUI sign-in successful.");
    sendJsonWithCookie(client, 200, "{\"ok\":true}", sessionCookieValue(sessionToken));
}

void handleAuthLogout(WiFiClient& client, const String& cookieHeader) {
    invalidateWebSession(cookieHeader);
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
        preferences.remove("credential_v1");
        preferences.remove("salt");
        preferences.remove("hash");
        preferences.end();
        webPasswordSalt = "";
        webPasswordHash = "";
        clearWebSessions();
        addLog("WEB - Password protection disabled.");
        sendJsonWithCookie(client, 200, "{\"ok\":true,\"enabled\":false}", String(WEB_SESSION_COOKIE) + "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
        return;
    }

    if (!saveWebPassword(newPassword)) {
        sendJson(client, 400, "{\"ok\":false,\"error\":\"invalid_password\"}");
        return;
    }

    clearWebSessions();
    String sessionToken = createWebSession();
    addLog("WEB - Password protection enabled or updated.");
    sendJsonWithCookie(client, 200, "{\"ok\":true,\"enabled\":true}", sessionCookieValue(sessionToken));
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
    json += ",\"logUpdatedAt\":";
    json += String(logUpdatedAt);
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
    String slotValue = queryParam(query, "slot");
    if (slotValue.length() != 1 || slotValue.charAt(0) < '0' || slotValue.charAt(0) > '9') {
        sendJson(client, 400, "{\"ok\":false,\"error\":\"invalid_slot\"}");
        return;
    }

    int slot = slotValue.toInt();
    if (slot < 0 || slot >= MAX_CONTROLLERS) {
        sendJson(client, 400, "{\"ok\":false,\"error\":\"invalid_slot\"}");
        return;
    }
    String removedMac = (slot >= 0 && slot < MAX_CONTROLLERS) ? savedMACs[slot] : "";
    bool removed = removeController(slot);
    if (removed) {
        addLog(String("CONTROLLER - Removed: ") + removedMac);
    }
    if (removed) sendJson(client, 200, "{\"ok\":true}");
    else if (removedMac.length() == 0) sendJson(client, 404, "{\"ok\":false,\"error\":\"controller_not_found\"}");
    else sendJson(client, 500, "{\"ok\":false,\"error\":\"storage_failed\"}");
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

    if (!saveController(mac, foundControllers[foundIndex].name)) {
        sendJson(client, 500, "{\"ok\":false,\"error\":\"storage_failed\"}");
        return;
    }
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

    if (!saveController(mac, nickname)) {
        sendJson(client, 500, "{\"ok\":false,\"error\":\"storage_failed\"}");
        return;
    }
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
    if (slot < 0 || slot >= MAX_CONTROLLERS || savedMACs[slot].length() == 0) {
        sendJson(client, 404, "{\"ok\":false,\"error\":\"controller_not_found\"}");
        return;
    }
    if (!saveControllerNickname(slot, nickname)) {
        sendJson(client, 500, "{\"ok\":false,\"error\":\"storage_failed\"}");
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

    // Defer the radio-mode change until after this HTTP response has left the
    // setup AP. The loop will make exactly one attempt with the new settings.
    wifiConnectRequested = true;
    if (wifiSsid.length() > 0) {
        addLog("WIFI - Settings saved for SSID: " + wifiSsid);
        sendJson(client, 200, "{\"ok\":true,\"connecting\":true}");
    } else {
        addLog("WIFI - Settings saved for SSID: (router WiFi disabled)");
        sendJson(client, 200, "{\"ok\":true,\"connecting\":false}");
    }
}

void handleApiBluetoothSave(WiFiClient& client, const String& query) {
    String mac = normalizeMac(queryParam(query, "mac"));
    uint8_t address[6];

    if (mac.length() > 0 && (!isValidMac(mac) || !parseMacAddress(mac, address))) {
        sendJson(client, 400, "{\"ok\":false,\"error\":\"invalid_mac\"}");
        return;
    }

    preferences.begin(CONFIG_NAMESPACE, false);
    bool saved = false;
    if (mac.length() > 0) {
        saved = preferences.putString("bt_mac", mac) > 0 &&
            preferences.getString("bt_mac", "") == mac;
    } else {
        preferences.remove("bt_mac");
        saved = preferences.getString("bt_mac", "").length() == 0;
    }
    preferences.end();
    if (!saved) {
        sendJson(client, 500, "{\"ok\":false,\"error\":\"storage_failed\"}");
        return;
    }
    classicBtSpoofMac = mac;

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

    bool newBleEnabled = bleValue == "1";
    bool newClassicInquiryEnabled = inquiryValue == "1";
    bool newClassicPairedEnabled = pairedValue == "1";
    uint8_t scanFlags = (newBleEnabled ? 0x01 : 0) |
                        (newClassicInquiryEnabled ? 0x02 : 0) |
                        (newClassicPairedEnabled ? 0x04 : 0);
    preferences.begin(CONFIG_NAMESPACE, false);
    size_t written = preferences.putUChar("scan_flags", scanFlags);
    bool saved = written == sizeof(uint8_t) &&
        preferences.getUChar("scan_flags", 0xff) == scanFlags;
    if (saved) {
        preferences.remove("scan_ble");
        preferences.remove("scan_inquiry");
        preferences.remove("scan_paired");
    }
    preferences.end();
    if (!saved) {
        sendJson(client, 500, "{\"ok\":false,\"error\":\"storage_failed\"}");
        return;
    }

    bool wasClassicInquiryEnabled = classicInquiryScanEnabled;
    bool wasBleEnabled = bleControllerScanEnabled;
    bleControllerScanEnabled = newBleEnabled;
    classicInquiryScanEnabled = newClassicInquiryEnabled;
    classicPairedScanEnabled = newClassicPairedEnabled;

    if (!bleControllerScanEnabled && !classicInquiryScanEnabled && webScanActive) {
        webScanActive = false;
        addLog("CONTROLLER - Web scan stopped: BLE and Classic pairing scans are disabled.");
    }

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

String normalizedWebHost(String host) {
    host.trim();
    host.toLowerCase();

    // This server uses IPv4. Strip an optional HTTP port before comparing the
    // Host/Origin authority with the device's known names and addresses.
    int colon = host.lastIndexOf(':');
    if (colon > 0 && host.indexOf(':') == colon) host = host.substring(0, colon);
    if (host.endsWith(".")) host.remove(host.length() - 1);
    return host;
}

bool isAllowedWebHost(const String& hostHeader) {
    if (hostHeader.length() == 0 || hostHeader.indexOf('/') >= 0 ||
        hostHeader.indexOf(' ') >= 0 || hostHeader.indexOf('\t') >= 0) {
        return false;
    }

    String host = normalizedWebHost(hostHeader);
    String hostname = String(WEB_HOSTNAME);
    hostname.toLowerCase();
    if (host == hostname || host == hostname + ".local") return true;
    if (WiFi.status() == WL_CONNECTED && host == WiFi.localIP().toString()) return true;
    if (setupApRunning && host == WiFi.softAPIP().toString()) return true;
    return false;
}

bool originMatchesWebHost(String origin, const String& hostHeader) {
    origin.trim();
    origin.toLowerCase();
    const String prefix = "http://";
    if (!origin.startsWith(prefix)) return false;

    String authority = origin.substring(prefix.length());
    if (authority.length() == 0 || authority.indexOf('/') >= 0) return false;
    return normalizedWebHost(authority) == normalizedWebHost(hostHeader);
}

bool readBoundedHttpLine(WiFiClient& client, String& line, size_t maxLength, unsigned long requestStartedAt, bool& tooLong) {
    line = "";
    tooLong = false;

    while (millis() - requestStartedAt < HTTP_HEADER_DEADLINE_MS) {
        while (client.available()) {
            char c = static_cast<char>(client.read());
            if (c == '\n') return true;
            if (c == '\r') continue;
            if (line.length() >= maxLength) {
                tooLong = true;
                return false;
            }
            line += c;
        }

        if (!client.connected()) return false;
        delay(1);
    }

    return false;
}

struct HttpRequest {
    String method;
    String path;
    String query;
    String body;
    String host;
    String origin;
    String cookie;
};

bool rejectHttpRequest(WiFiClient& client, int status, const char* error) {
    String json = "{\"ok\":false,\"error\":\"" + String(error) + "\"}";
    sendJson(client, status, json);
    delay(1);
    client.stop();
    return false;
}

bool parseContentLength(const String& value, int& contentLength) {
    if (value.length() == 0) return false;
    unsigned long parsed = 0;
    for (unsigned int i = 0; i < value.length(); i++) {
        char c = value.charAt(i);
        if (c < '0' || c > '9') return false;
        parsed = parsed * 10 + static_cast<unsigned long>(c - '0');
        if (parsed > MAX_HTTP_LENGTH) {
            contentLength = static_cast<int>(parsed);
            return true;
        }
    }
    contentLength = static_cast<int>(parsed);
    return true;
}

bool readHttpRequest(WiFiClient& client, HttpRequest& request) {
    client.setTimeout(100);
    unsigned long requestStartedAt = millis();
    String requestLine;

    bool lineTooLong = false;
    if (!readBoundedHttpLine(client, requestLine, MAX_HTTP_LENGTH, requestStartedAt, lineTooLong)) {
        if(lineTooLong) {
            return rejectHttpRequest(client, 414, "request_target_too_large");
        } else {
            return rejectHttpRequest(client, 408, "request_timeout");
        }
    }

    int firstSpace = requestLine.indexOf(' ');
    int secondSpace = requestLine.indexOf(' ', firstSpace + 1);
    if (firstSpace <= 0 || secondSpace <= firstSpace + 1) {
        return rejectHttpRequest(client, 400, "invalid_request_line");
    }
    request.method = requestLine.substring(0, firstSpace);
    request.path = requestLine.substring(firstSpace + 1, secondSpace);
    int queryStart = request.path.indexOf('?');
    if (queryStart >= 0) {
        request.query = request.path.substring(queryStart + 1);
        request.path = request.path.substring(0, queryStart);
    }

    int contentLength = 0;
    bool contentLengthSeen = false;
    String transferEncoding;
    size_t headerBytes = 0;
    while (true) {
        String header;
        if (!readBoundedHttpLine(client, header, MAX_HTTP_LENGTH, requestStartedAt, lineTooLong)) {
            if(lineTooLong) {
                return rejectHttpRequest(client, 431, "headers_too_large");
            } else {
                return rejectHttpRequest(client, 408, "request_timeout");
            }
        }
        if (header.length() == 0) break;
        headerBytes += header.length();
        if (headerBytes > 4 * MAX_HTTP_LENGTH) {
            return rejectHttpRequest(client, 431, "headers_too_large");
        }

        int colon = header.indexOf(':');
        if (colon <= 0) return rejectHttpRequest(client, 400, "invalid_header");
        String name = header.substring(0, colon);
        String value = header.substring(colon + 1);
        name.trim();
        value.trim();
        if (name.equalsIgnoreCase("Cookie")) request.cookie = value;
        else if (name.equalsIgnoreCase("Host")) request.host = value;
        else if (name.equalsIgnoreCase("Origin")) request.origin = value;
        else if (name.equalsIgnoreCase("Transfer-Encoding")) transferEncoding = value;
        else if (name.equalsIgnoreCase("Content-Length")) {
            if (contentLengthSeen || !parseContentLength(value, contentLength)) {
                return rejectHttpRequest(client, 400, "invalid_content_length");
            }
            contentLengthSeen = true;
        }
    }

    if (transferEncoding.length() > 0 && !transferEncoding.equalsIgnoreCase("identity")) {
        return rejectHttpRequest(client, 400, "unsupported_transfer_encoding");
    }
    if (contentLength < 0 || contentLength > static_cast<int>(MAX_HTTP_LENGTH)) {
        return rejectHttpRequest(client, 413, "request_too_large");
    }

    request.body.reserve(contentLength);
    unsigned long bodyStartedAt = millis();
    while ((int)request.body.length() < contentLength && millis() - bodyStartedAt < 500) {
        while (client.available() && (int)request.body.length() < contentLength) {
            request.body += static_cast<char>(client.read());
        }
        if ((int)request.body.length() < contentLength) delay(1);
    }
    if ((int)request.body.length() != contentLength) {
        return rejectHttpRequest(client, 408, "incomplete_request_body");
    }

    if (!isAllowedWebHost(request.host) ||
        (request.method == "POST" && request.origin.length() > 0 &&
         !originMatchesWebHost(request.origin, request.host))) {
        return rejectHttpRequest(client, 403, "invalid_origin");
    }
    return true;
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

    HttpRequest request;
    if (!readHttpRequest(client, request)) return;

    const String& method = request.method;
    const String& target = request.path;
    const String& body = request.body;
    const String& cookieHeader = request.cookie;
    bool authenticated = hasValidWebSession(cookieHeader);
    bool webAccessAllowed = !webPasswordConfigured() || authenticated;
    if (method == "GET" && target == "/") {
        if (webPasswordConfigured() && !authenticated) {
            handleAuthPage(client);
        } else {
            sendFileAsset(client, "/index.html", "text/html; charset=utf-8", "no-store");
        }
    } else if (method == "GET" && target == "/icon.png") {
        sendFileAsset(client, "/icon.png", "image/png", "public, max-age=86400");
    } else if (method == "GET" && target == "/manifest.webmanifest") {
        sendFileAsset(client, "/manifest.webmanifest", "application/manifest+json; charset=utf-8", "no-cache");

    } else if (method == "POST" && target == "/api/auth/login") {
        handleAuthLogin(client, body);
    } else if (!webAccessAllowed) {
        sendJson(client, 401, "{\"ok\":false,\"error\":\"authentication_required\"}");
    } else if (method == "POST" && target == "/api/auth/logout") {
        handleAuthLogout(client, cookieHeader);
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
        handleApiRemove(client, body);
    } else if (method == "POST" && target == "/api/scan/start") {
        handleApiStartScan(client);
    } else if (method == "POST" && target == "/api/pair") {
        handleApiPair(client, body);
    } else if (method == "POST" && target == "/api/manual-add") {
        handleApiManualAdd(client, body);
    } else if (method == "POST" && target == "/api/nickname") {
        handleApiNickname(client, body);
    } else if (method == "POST" && target == "/api/wifi/save") {
        handleApiWiFiSave(client, body);
    } else if (method == "POST" && target == "/api/bluetooth/save") {
        handleApiBluetoothSave(client, body);
    } else if (method == "POST" && target == "/api/scan/options") {
        handleApiScanOptionsSave(client, body);
    } else if (method == "POST" && target == "/api/restart") {
        handleApiRestart(client);
    } else if (method == "POST" && target == "/api/power/on") {
        bool started = startAtxPowerOnSequence();
        sendJson(client, started ? 200 : 409, started
            ? "{\"ok\":true}"
            : "{\"ok\":false,\"error\":\"power_operation_rejected\"}");
    } else if (method == "POST" && target == "/api/power/off") {
        bool started = startNormalShutdown();
        sendJson(client, started ? 200 : 409, started
            ? "{\"ok\":true}"
            : "{\"ok\":false,\"error\":\"power_operation_rejected\"}");
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

    littleFsReady = LittleFS.begin(false);
    if (!littleFsReady) {
        addLog("WebUI - LittleFS mount failed. Upload the sketch data filesystem.");
    }

    bluetoothEventQueue = xQueueCreate(BLUETOOTH_EVENT_QUEUE_LENGTH, sizeof(BluetoothEvent));
    if (bluetoothEventQueue == nullptr) {
        addLog("Bluetooth - WARNING: event queue allocation failed.");
    }

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
    digitalWrite(RELAY_PSU_PIN, stablePcOn ? RELAY_ON : RELAY_OFF);

    preferences.begin(CONFIG_NAMESPACE, true);
    StoredControllerConfig storedControllers = {};
    bool loadedControllerRecord =
        preferences.getBytesLength("controllers_v1") == sizeof(storedControllers) &&
        preferences.getBytes("controllers_v1", &storedControllers, sizeof(storedControllers)) == sizeof(storedControllers) &&
        storedControllers.magic == CONTROLLER_CONFIG_MAGIC &&
        storedControllers.nextSlot < MAX_CONTROLLERS;
    currentSlot = loadedControllerRecord ? storedControllers.nextSlot : preferences.getInt("slot", 0);
    if (currentSlot < 0 || currentSlot >= MAX_CONTROLLERS) currentSlot = 0;
    classicBtSpoofMac = normalizeMac(preferences.getString("bt_mac", ""));
    if (preferences.isKey("scan_flags")) {
        uint8_t scanFlags = preferences.getUChar("scan_flags", 0x07);
        bleControllerScanEnabled = (scanFlags & 0x01) != 0;
        classicInquiryScanEnabled = (scanFlags & 0x02) != 0;
        classicPairedScanEnabled = (scanFlags & 0x04) != 0;
    } else {
        bleControllerScanEnabled = preferences.getBool("scan_ble", true);
        classicInquiryScanEnabled = preferences.getBool("scan_inquiry", true);
        classicPairedScanEnabled = preferences.getBool("scan_paired", true);
    }
    uint8_t storedSpoofAddress[6];
    if (classicBtSpoofMac.length() > 0 &&
        (!isValidMac(classicBtSpoofMac) || !parseMacAddress(classicBtSpoofMac, storedSpoofAddress))) {
        addLog("Classic Bluetooth - Ignoring invalid saved spoof MAC.");
        classicBtSpoofMac = "";
    }

    addLog("Controllers - Loading saved...");
    int count = 0;
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
        if (loadedControllerRecord) {
            storedControllers.macs[i][sizeof(storedControllers.macs[i]) - 1] = '\0';
            storedControllers.names[i][sizeof(storedControllers.names[i]) - 1] = '\0';
            savedMACs[i] = String(storedControllers.macs[i]);
            savedControllerNicknames[i] = normalizeControllerNickname(String(storedControllers.names[i]));
        } else {
            savedMACs[i] = preferences.getString(("mac" + String(i)).c_str(), "");
            savedControllerNicknames[i] = normalizeControllerNickname(
                preferences.getString(("name" + String(i)).c_str(), "")
            );
        }
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
    unsigned long now = millis() + 1; // Avoid zero for time comparisons.

    processBluetoothEvents();
    // Update stable inputs and advance any in-progress relay sequence before
    // reacting to BLE or button events.
    updatePcState(now);
    updateClassicPageScan();
    handlePowerState();

    handleButton(now);
    updateStatusLed(now);
    monitorWiFi(now);
    protectSetupApClient();
    handleWebServer();

    // Give the HTTP response time to leave before restarting. Restart is only
    // accepted while the PC is off so PS_ON cannot be interrupted here.
    if (restartRequested) {
        if (now - restartRequestedAt >= 500) ESP.restart();
        delay(10);
        return;
    }

    scanClassicBluetooth();
    scanBle();
}
