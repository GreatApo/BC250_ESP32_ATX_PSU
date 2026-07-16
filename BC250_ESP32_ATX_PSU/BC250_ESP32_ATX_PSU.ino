#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <Preferences.h>

// ================= PIN SETTINGS =================
const int RELAY_PWR_PIN = 17;     // Left relay: Power ON BC-250
const int RELAY_CONST_PIN = 16;   // Right relay: 12V Power
const int BUTTON_PIN = 23;        // ORANGE wire: Physical button
const int LED_PIN = 19;           // GREEN wire: Button LED
const int PC_MONITOR_PIN = 4;     // RED wire (TPMS1 9 pin): PC Monitor

const int RELAY_ON = HIGH;
const int RELAY_OFF = LOW;

// ================= BLACKLIST =================
const int BLACKLIST_SIZE = 1; // Number of devices in the blacklist
String blacklistedMACs[BLACKLIST_SIZE] = {
   "c7:46:01:56:dd:32", // Malicious MAC address (strictly lowercase)
};

// ================= PAIRING SETTINGS =================
// -30 = point-blank (touching the board)
// -45 = about 20-30 cm (our choice)
// -70 = within the room
// -90 = behind a wall
const int PAIRING_RSSI_THRESHOLD = -45; 

// ================= MEMORY FOR 5 GAMEPADS =================
const int MAX_CONTROLLERS = 5;
String savedMACs[MAX_CONTROLLERS];
int currentSlot = 0; 

Preferences preferences;
bool pairingMode = false;
bool triggerPcWake = false;

unsigned long buttonPressStartTime = 0;
unsigned long lastWakeTime = 0;
const unsigned long COOLDOWN = 15000; 

// --- LED BLINK TIMERS ---
unsigned long lastBlinkTime = 0;
bool ledBlinkState = LOW;

// --- SHUTDOWN PROTECTION COOLDOWN ---
unsigned long lastShutdownTime = 0;
const unsigned long SHUTDOWN_COOLDOWN = 60000; // 60 seconds of "deafness" after PC shuts down
bool lastPcState = LOW;

// ================= BLE CALLBACK =================
class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
        String deviceMAC = advertisedDevice.getAddress().toString().c_str();
        int currentRSSI = advertisedDevice.getRSSI();
        
        // 🛑 STRICT BLACKLIST CHECK 🛑
        for (int i = 0; i < BLACKLIST_SIZE; i++) {
            if (deviceMAC.equalsIgnoreCase(blacklistedMACs[i])) {
                return; // If it's a blacklisted device — instantly ignore it
            }
        }
        
        // PAIRING MODE
        if (pairingMode) {
            // STRICT DISTANCE FILTER (SNIPER)
            if (currentRSSI > PAIRING_RSSI_THRESHOLD) {
                
                bool alreadySaved = false;
                for (int i = 0; i < MAX_CONTROLLERS; i++) {
                    if (savedMACs[i].equalsIgnoreCase(deviceMAC)) {
                        alreadySaved = true;
                        break;
                    }
                }
                
                if (alreadySaved) {
                    Serial.println("\n👌 This gamepad is already saved: " + deviceMAC);
                } else {
                    savedMACs[currentSlot] = deviceMAC;
                    
                    preferences.begin("xbox_cfg", false);
                    preferences.putString(("mac" + String(currentSlot)).c_str(), deviceMAC);
                    
                    currentSlot = (currentSlot + 1) % MAX_CONTROLLERS;
                    preferences.putInt("slot", currentSlot);
                    preferences.end();
                    
                    Serial.println("\n✅ SUCCESS! Paired a gamepad with a strong signal (" + String(currentRSSI) + " dBm): " + deviceMAC);
                }
                
                pairingMode = false; // Exiting pairing mode, LED will automatically stop blinking
            }
        } 
        // NORMAL MODE
        else {
            bool isKnownController = false;
            for (int i = 0; i < MAX_CONTROLLERS; i++) {
                if (savedMACs[i].length() > 0 && deviceMAC.equalsIgnoreCase(savedMACs[i])) {
                    isKnownController = true;
                    break;
                }
            }
            
            if (isKnownController) {
                if (digitalRead(PC_MONITOR_PIN) == LOW) {
                    
                    // CHECK: ARE WE IN THE SHUTDOWN COOLDOWN DEADZONE
                    bool inShutdownCooldown = (lastShutdownTime > 0) && (millis() - lastShutdownTime < SHUTDOWN_COOLDOWN);
                    
                    if (!inShutdownCooldown) {
                        if (millis() - lastWakeTime > COOLDOWN) {
                            Serial.println("🎮 Known controller detected! Turning on the PC...");
                            triggerPcWake = true;
                            lastWakeTime = millis();
                        }
                    }
                }
            }
        }
    }
};

// ================= SETUP =================
void setup() {
    Serial.begin(115200);
    delay(1000);
    
    pinMode(RELAY_PWR_PIN, OUTPUT);
    digitalWrite(RELAY_PWR_PIN, RELAY_OFF);
    
    pinMode(RELAY_CONST_PIN, OUTPUT);
    digitalWrite(RELAY_CONST_PIN, RELAY_OFF);
    
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
    
    pinMode(BUTTON_PIN, INPUT_PULLUP); 
    pinMode(PC_MONITOR_PIN, INPUT_PULLDOWN);

    // Read the initial PC state at boot
    lastPcState = digitalRead(PC_MONITOR_PIN);

    preferences.begin("xbox_cfg", true);
    currentSlot = preferences.getInt("slot", 0);
    
    Serial.println("\n====== SAVED GAMEPADS ======");
    int count = 0;
    for (int i = 0; i < MAX_CONTROLLERS; i++) {
        savedMACs[i] = preferences.getString(("mac" + String(i)).c_str(), "");
        if (savedMACs[i] != "") {
            Serial.println("Slot " + String(i + 1) + ": " + savedMACs[i]);
            count++;
        }
    }
    if (count == 0) Serial.println("Memory is empty. No saved gamepads!");
    Serial.println("==================================\n");
    preferences.end();

    BLEDevice::init("");
    BLEScan* pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);
}

// ================= LOOP =================
void loop() {
    // 1. Constant relay (12v) and BC-250 state tracking
    bool currentPcState = digitalRead(PC_MONITOR_PIN);
    
    if (currentPcState == HIGH) {
        digitalWrite(RELAY_CONST_PIN, RELAY_ON);
    } else {
        digitalWrite(RELAY_CONST_PIN, RELAY_OFF);
    }

    // --- LED INDICATOR SYNC ---
    if (pairingMode) {
        // Fast blinking during pairing mode (every 100ms)
        if (millis() - lastBlinkTime >= 100) {
            lastBlinkTime = millis();
            ledBlinkState = !ledBlinkState;
            digitalWrite(LED_PIN, ledBlinkState);
        }
    } else {
        // Normal mode: Make LED match the PC state
        digitalWrite(LED_PIN, currentPcState); 
    }

    // --- TRACKING BC-250 SHUTDOWN MOMENT ---
    if (currentPcState == LOW && lastPcState == HIGH) {
        lastShutdownTime = millis();
        if (lastShutdownTime == 0) lastShutdownTime = 1; // Protection against zero millis value
        Serial.println("💻 PC turned OFF. Activating 60s gamepad ignore timer...");
    }
    lastPcState = currentPcState;

    // 2. Turn on BC-250
    if (triggerPcWake) {
        digitalWrite(RELAY_PWR_PIN, RELAY_ON);
        delay(500); 
        digitalWrite(RELAY_PWR_PIN, RELAY_OFF);
        triggerPcWake = false;
    }

    // 3. Physical button handling (5 sec = pairing, 10 sec = clear gamepad memory)
    static bool memoryCleared = false;

    if (digitalRead(BUTTON_PIN) == LOW) { 
        if (buttonPressStartTime == 0) {
            buttonPressStartTime = millis(); 
            memoryCleared = false;
        }
        
        unsigned long heldTime = millis() - buttonPressStartTime;

        // 10 SECONDS = CLEAR MEMORY
        if (heldTime >= 10000 && !memoryCleared) {
            preferences.begin("xbox_cfg", false);
            preferences.clear(); 
            currentSlot = 0;
            preferences.putInt("slot", 0);
            preferences.end();

            for (int i = 0; i < MAX_CONTROLLERS; i++) {
                savedMACs[i] = "";
            }

            memoryCleared = true;
            pairingMode = false; 
            Serial.println("\n🗑️ MEMORY COMPLETELY CLEARED! NO SAVED GAMEPADS!");

            // 3 LONG BLINKS
            for(int i = 0; i < 3; i++) {
                digitalWrite(LED_PIN, HIGH); delay(500);
                digitalWrite(LED_PIN, LOW); delay(500);
            }
        }
        // 5 SECONDS = PAIRING MODE
        else if (heldTime >= 5000 && heldTime < 10000 && !pairingMode && !memoryCleared) {
            pairingMode = true;
            Serial.println("\n📡 PAIRING MODE! Bring the gamepad POINT-BLANK (closer than 30 cm)...");
            lastBlinkTime = millis(); // Initialize the blink timer
        }

    } else {
        if (buttonPressStartTime > 0) {
            unsigned long pressDuration = millis() - buttonPressStartTime;
            
            if (pressDuration > 50 && pressDuration < 5000 && !pairingMode && !memoryCleared) {
                Serial.println("👉 Physical button! Turning on BC-250...");
                triggerPcWake = true; 
            }
            buttonPressStartTime = 0; 
        }
    }

    // 4. BLE Scanning
    BLEScan* pBLEScan = BLEDevice::getScan();
    pBLEScan->start(1, false); 
    pBLEScan->clearResults(); 
}