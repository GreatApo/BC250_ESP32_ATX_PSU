#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <Preferences.h>
#include <WiFi.h>
#include <string.h>
#include "esp_gap_bt_api.h"

// ================= PIN SETTINGS =================
const int RELAY_PWR_PIN = 17;     // (Left relay) BC-250 power button pins
const int RELAY_PSU_PIN = 16;     // (Right relay) ATX PS_ON green wire to PSU ground
const int BUTTON_PIN = 19;        // ORANGE wire: Physical button
const int LED_PIN = 23;           // GREEN wire: Button LED
const int PC_MONITOR_PIN = 4;     // RED wire (TPMS1 pin 9 / 3V): PC Monitor

const int RELAY_ON = HIGH;
const int RELAY_OFF = LOW;

// ================= WIFI / WEB UI SETTINGS =================
// Fill in WiFi to enable the controller management webpage.
const char* WIFI_SSID = "";
const char* WIFI_PASSWORD = "";
const char* WEB_HOSTNAME = "bc250-controller";
const char* WIFI_AP_PASSWORD = "12345678"; // Leave empty for an open setup AP, or use 8+ chars.

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
const unsigned long WIFI_RECONNECT_INTERVAL_MS = 30000; // Retry router WiFi this often after a drop
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
unsigned long lastWifiReconnectAttempt = 0;
bool classicDiscoveryRunning = false;

bool setupApRunning = false;
bool routerWifiWasConnected = false;
bool routerWifiRetrying = false;
unsigned long wifiRetryStartedAt = 0;

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
            //addLog("Classic Bluetooth inquiry finished.");
        }
    }
}

void setupClassicBluetooth() {
    esp_err_t result = esp_bt_gap_register_callback(classicGapCallback);
    if (result == ESP_OK) {
        addLog("Classic Bluetooth - Inquiry ready.");
    } else {
        addLog(String("Classic Bluetooth - WARNING: callback failed: ") + String((int)result));
    }
}

void scanClassicBluetooth() {
    unsigned long now = millis();

    if (buttonIsPressed()) return;

    if (webScanActive && now - webScanStartTime >= WEB_SCAN_DURATION_MS) {
        webScanActive = false;
        addLog("Web controller scan finished.");
    }

    if (!webScanActive) {
        if (stablePcOn) return;
        if (!hasSavedController()) return;
    }

    if (classicDiscoveryRunning) return;
    if (bleScanRunning) return;
    if (now - lastClassicScanTime < CLASSIC_SCAN_INTERVAL_MS) return;
    lastClassicScanTime = now;

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
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <link rel="icon" href="data:image/svg+xml,<svg xmlns=%22http://www.w3.org/2000/svg%22 viewBox=%220 0 100 100%22><text y=%22.9em%22 font-size=%2290%22>🎮</text></svg>">
  <title>BC250 Controller Portal</title>
  <style>
    :root { color-scheme: dark; font-family: system-ui, sans-serif; background: #111; color: #eee; }
    body { margin: 0; padding: 18px; max-width: 760px; margin-inline: auto; }
    h1 { font-size: 1.45rem; margin: 0 0 14px; }
    h2 { font-size: 1rem; margin: 24px 0 8px; color: #bbb; }
    .row { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
    .panel { border: 1px solid #333; border-radius: 8px; padding: 12px; margin: 10px 0; background: #181818; }
    button { border: 0; border-radius: 6px; padding: 10px 12px; background: #2f7df6; color: white; font-weight: 700; cursor: pointer; }
    button.danger { background: #b83d3d; }
    button.secondary { background: #444; }
    button:disabled { opacity: .5; }
    input { border: 1px solid #444; border-radius: 6px; padding: 10px; background: #101010; color: #eee; }
    .muted { color: #999; }
    .item { display: flex; justify-content: space-between; gap: 8px; align-items: center; border-top: 1px solid #2b2b2b; padding: 10px 0; }
    .item:first-child { border-top: 0; }
    code { font-size: .95rem; }
    pre { margin: 0; white-space: pre-wrap; word-break: break-word; max-height: 260px; overflow: auto; font-size: .85rem; line-height: 1.35; }
  </style>
</head>
<body>
  <h1>BC250 Controller Portal</h1>
  
  <h2>⏻ Status</h2>
  <div class="panel">
    <div class="row">
      <strong>PC status:</strong><span id="pc">...</span>
      <span class="muted" id="powerState"></span>
    </div>
    <div class="row" style="margin-top:12px">
      <button id="powerOnBtn" onclick="post('/api/power/on')">Power on</button>
      <button id="powerOffBtn" class="danger" onclick="post('/api/power/off')">Power off</button>
    </div>
  </div>

  <h2>🎮 Registered Controllers</h2>
  <div class="panel" id="paired"></div>

  <h2>➕ Add controller manually</h2>
  <div class="panel">
    <div class="row">
      <input id="manualMac" placeholder="aa:bb:cc:dd:ee:ff" maxlength="17">
      <button onclick="manualAdd()">Add</button>
    </div>
    <div class="muted" style="margin-top:8px">Use this when a controller does not appear during scan.</div>
  </div>

  <h2>🔎 Scan for controllers</h2>
  <div class="panel">
    <div class="row">
      <button id="scanBtn" onclick="post('/api/scan/start')">Scan</button>
      <span class="muted" id="scanState"></span>
    </div>
    <div class="row" style="margin-top:12px">
      <label for="rssiInput">dBm filter</label>
      <input id="rssiInput" type="number" min="-100" max="-20" step="1">
      <button id="rssiBtn" class="secondary" onclick="setRssi()">Apply</button>
      <span class="muted">Higher is stricter, e.g. -45 close, -70 room.</span>
    </div>
    <div id="found" style="margin-top:8px"></div>
  </div>

  <h2>🧾 Log</h2>
  <div class="panel">
    <pre id="log">...</pre>
  </div>

<script>
async function api(path) {
  const response = await fetch(path, { cache: 'no-store' });
  return response.json();
}
let refreshInFlight = false;
let refreshQueued = false;
function scheduleRefresh(delayMs = 0) {
    setTimeout(() => {
        refresh();
    }, delayMs);
}
async function post(path) {
  await fetch(path, { method: 'POST', cache: 'no-store' });
    scheduleRefresh();
}
async function manualAdd() {
  const mac = document.getElementById('manualMac').value.trim();
  const response = await fetch('/api/manual-add?mac=' + encodeURIComponent(mac), { method: 'POST', cache: 'no-store' });
  if (response.ok) {
    document.getElementById('manualMac').value = '';
  }
    scheduleRefresh();
}
async function setRssi() {
  const value = document.getElementById('rssiInput').value;
  await fetch('/api/rssi?value=' + encodeURIComponent(value), { method: 'POST', cache: 'no-store' });
    scheduleRefresh();
}
function empty(text) {
  const div = document.createElement('div');
  div.className = 'muted';
  div.textContent = text;
  return div;
}
function controllerRow(controller, actionText, actionClass, action) {
  const row = document.createElement('div');
  row.className = 'item';
  const label = document.createElement('div');
  if (controller.name !== undefined) {
    const name = document.createElement('div');
    name.textContent = controller.name || 'Unknown';
    label.appendChild(name);
  }
  const mac = document.createElement('code');
  mac.textContent = controller.mac;
  label.appendChild(mac);
  if (controller.rssi !== undefined) label.append(' (' + controller.rssi + ' dBm)');
  const button = document.createElement('button');
  button.className = actionClass || 'secondary';
  button.textContent = actionText;
  button.onclick = action;
  row.append(label, button);
  return row;
}
async function refresh() {
    if (refreshInFlight) {
        refreshQueued = true;
        return;
    }

    refreshInFlight = true;
    try {
        const [status, paired, found, logs] = await Promise.all([
            api('/api/status'),
            api('/api/controllers'),
            api('/api/found'),
            api('/api/logs')
        ]);

        document.getElementById('pc').textContent = status.pcOn ? 'ON' : 'OFF';
        document.getElementById('powerState').textContent = status.powerState;
        document.getElementById('scanState').textContent = status.scanning ? 'Scanning...' : 'Idle';
        document.getElementById('powerOnBtn').disabled = status.pcOn || status.busy;
        document.getElementById('powerOffBtn').disabled = !status.pcOn || status.busy;
        document.getElementById('scanBtn').disabled = status.scanning;
        document.getElementById('rssiBtn').disabled = status.scanning;
        const rssiInput = document.getElementById('rssiInput');
        if (document.activeElement !== rssiInput) rssiInput.value = status.rssiThreshold;

        const pairedBox = document.getElementById('paired');
        pairedBox.replaceChildren();
        if (!paired.length) pairedBox.appendChild(empty('No controllers paired.'));
        paired.forEach(c => pairedBox.appendChild(controllerRow(c, 'Remove', 'danger', () => post('/api/remove?slot=' + c.slot))));

        const foundBox = document.getElementById('found');
        foundBox.replaceChildren();
        if (!found.length) foundBox.appendChild(empty(status.scanning ? 'No new controllers found yet.' : 'Start a scan to find nearby controllers.'));
        found.forEach(c => foundBox.appendChild(controllerRow(c, 'Pair', '', () => post('/api/pair?mac=' + encodeURIComponent(c.mac)))));

        const logBox = document.getElementById('log');
        logBox.textContent = logs.length ? logs.join('\n') : 'No log entries yet.';
        logBox.scrollTop = logBox.scrollHeight;
    } catch (error) {
        console.error('Refresh failed', error);
        document.getElementById('scanState').textContent = 'Refresh failed';
    } finally {
        refreshInFlight = false;
        if (refreshQueued) {
            refreshQueued = false;
            scheduleRefresh();
        }
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

void setupWebServer() {
    server.begin();
}

void setupWiFi() {
    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(WEB_HOSTNAME);

    setupWebServer();

    if (WIFI_SSID[0] == '\0') {
        addLog("Router WiFi disabled: WIFI_SSID is empty.");
        startSetupAp();
        return;
    }

    addLog(String("Trying router WiFi first, SSID: ") + WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

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
    if (WIFI_SSID[0] == '\0') return;

    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected) {
        if (!routerWifiWasConnected) {
            routerWifiWasConnected = true;
            routerWifiRetrying = false;
            addLog(String("Router WiFi reconnected. LAN webpage: http://") + WiFi.localIP().toString());
            addLog(String("Signal: ") + String(WiFi.RSSI()) + " dBm");
        }
        return;
    }

    if (routerWifiRetrying && now - wifiRetryStartedAt >= WIFI_CONNECT_TIMEOUT_MS) {
        routerWifiRetrying = false;
        addLog(String("Router WiFi retry failed: ") + wifiStatusName(WiFi.status()));
        startSetupAp();
    }

    if (routerWifiWasConnected) {
        routerWifiWasConnected = false;
        addLog(String("Router WiFi dropped: ") + wifiStatusName(WiFi.status()));
        startSetupAp();
    }

    if (now - lastWifiReconnectAttempt >= WIFI_RECONNECT_INTERVAL_MS) {
        lastWifiReconnectAttempt = now;
        addLog("Retrying router WiFi...");
        stopSetupAp();
        WiFi.mode(WIFI_STA);
        WiFi.disconnect(false);
        WiFi.setHostname(WEB_HOSTNAME);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        routerWifiRetrying = true;
        wifiRetryStartedAt = now;
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
    if (webScanRssiThreshold < -100) webScanRssiThreshold = -100;
    if (webScanRssiThreshold > -20) webScanRssiThreshold = -20;

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

    /*
    static String lastStatusLog = "";
    String statusLog = "DEBUG BUTTON=" + String(digitalRead(BUTTON_PIN)) +
    " RELAY_PWR=" + String(digitalRead(RELAY_PWR_PIN)) +
    " RELAY_PSU=" + String(digitalRead(RELAY_PSU_PIN));
    
    // DEBUG: periodically print button / PC / relay states for troubleshooting
    if (statusLog != lastStatusLog) {
        addLog(statusLog);
        lastStatusLog = statusLog;
    }
    */

    // Update stable inputs and advance any in-progress relay sequence before
    // reacting to BLE or button events.
    updatePcState();
    handlePowerState();
    syncAtxPsuRelay();
    handleShutdownTracking(now); // Turns off PSU after PC shutdown and starts 60s ignore timer
    handleButton(now);
    updateStatusLed(now);
    monitorWiFi(now);
    handleWebServer();
    scanClassicBluetooth();
    scanBle();
}
