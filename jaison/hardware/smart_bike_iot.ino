/*
 * ============================================================
 * SMART BIKE CRASH DETECTION & SAFETY SYSTEM (ESP32)
 * ============================================================
 * Hardware: ESP32 + MPU6050 + NEO-6M GPS + SIM800L + Buzzer + Button
 * Communication: BLE (Local) + GPRS/HTTP (Remote) 
 * Backend: Django REST API
 * 
 * FEATURES:
 * 1. BLE advertising for Flutter app connection
 * 2. GPRS heartbeat + GPS sync to Django
 * 3. Persistent emergency number (Preferences.h)
 * 4. 4-scenario crash detection with 10s countdown
 * 5. SMS + Phone Call emergency fallback
 * 6. Silent mode toggle from app
 * 7. "I'm Okay" cancel from button OR app
 * ============================================================
 */

#define TINY_GSM_MODEM_SIM800
#include <TinyGsmClient.h>
#include <ArduinoHttpClient.h>
#include <HardwareSerial.h>
#include <TinyGPS++.h>
#include <Wire.h>
#include <MPU6050.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// ==================== HARDWARE PINS ====================
#define SIM_RX      16
#define SIM_TX      17
#define GPS_RX      18
#define GPS_TX      19
#define BUZZER      23
#define SWT         27
#define VIB_PIN     34
#define FSR_PIN     35

// ==================== BACKEND CONFIG ====================
const char server[]   = "7fqnrtr5-8000.inc1.devtunnels.ms";
const int  serverPort = 80;
const char apn[]      = "internet";
const char bike_id[]  = "BIKE_001";

// ==================== BLE UUIDs ====================
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_SENSOR_UUID    "beb5483e-36e1-4688-b7f5-ea07361b26a8"  // Notify sensor data
#define CHAR_COMMAND_UUID   "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  // Write commands from app

// ==================== OBJECTS ====================
MPU6050 mpu;
TinyGPSPlus gps;
HardwareSerial simSerial(1);
HardwareSerial gpsSerial(2);
TinyGsm modem(simSerial);
TinyGsmClient gprsClient(modem);
Preferences preferences;

BLEServer* pServer = nullptr;
BLECharacteristic* pSensorCharacteristic = nullptr;
BLECharacteristic* pCommandCharacteristic = nullptr;

// ==================== STATE VARIABLES ====================
String emergencyNumber = "+910000000000";
String emergencyName   = "Emergency";
bool   silentMode      = false;
bool   deviceConnected = false;
bool   crashDetected   = false;
bool   emergencyActive = false;
bool   cancelRequested = false;
int    crashScenario   = 0;

float latitude  = 0.0;
float longitude = 0.0;

unsigned long lastGPRSSync   = 0;
unsigned long lastHeartbeat  = 0;
const unsigned long GPRS_SYNC_INTERVAL  = 10000;   // 10 seconds
const unsigned long HEARTBEAT_INTERVAL  = 300000;  // 5 minutes

// ==================== BLE CALLBACKS ====================
class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        deviceConnected = true;
        Serial.println("[BLE] Device connected!");
    }
    void onDisconnect(BLEServer* pServer) {
        deviceConnected = false;
        Serial.println("[BLE] Device disconnected!");
        // Restart advertising
        pServer->startAdvertising();
    }
};

class CommandCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* pCharacteristic) {
        String value = pCharacteristic->getValue().c_str();
        value.trim();
        Serial.print("[BLE] Received command: ");
        Serial.println(value);

        if (value.startsWith("PHONE:")) {
            // Format: PHONE:+919994235648
            emergencyNumber = value.substring(6);
            preferences.begin("bike", false);
            preferences.putString("emNum", emergencyNumber);
            preferences.end();
            Serial.print("[BLE] Emergency number updated: ");
            Serial.println(emergencyNumber);
        }
        else if (value.startsWith("NAME:")) {
            emergencyName = value.substring(5);
            preferences.begin("bike", false);
            preferences.putString("emName", emergencyName);
            preferences.end();
            Serial.print("[BLE] Emergency name updated: ");
            Serial.println(emergencyName);
        }
        else if (value == "SILENT:ON") {
            silentMode = true;
            preferences.begin("bike", false);
            preferences.putBool("silent", true);
            preferences.end();
            Serial.println("[BLE] Silent mode ON");
        }
        else if (value == "SILENT:OFF") {
            silentMode = false;
            preferences.begin("bike", false);
            preferences.putBool("silent", false);
            preferences.end();
            Serial.println("[BLE] Silent mode OFF");
        }
        else if (value == "CANCEL") {
            // "I'm Okay" from the app
            cancelRequested = true;
            Serial.println("[BLE] Cancel requested from app!");
        }
    }
};

// ==================== SETUP ====================
void setup() {
    Serial.begin(115200);
    Serial.println("========================================");
    Serial.println("  SMART BIKE SAFETY SYSTEM v2.0");
    Serial.println("========================================");

    // --- Load saved preferences ---
    preferences.begin("bike", true);  // read-only
    emergencyNumber = preferences.getString("emNum", "+910000000000");
    emergencyName   = preferences.getString("emName", "Emergency");
    silentMode      = preferences.getBool("silent", false);
    preferences.end();

    Serial.print("[INIT] Emergency Number: ");
    Serial.println(emergencyNumber);
    Serial.print("[INIT] Silent Mode: ");
    Serial.println(silentMode ? "ON" : "OFF");

    // --- GPIO Setup ---
    pinMode(BUZZER, OUTPUT);
    pinMode(SWT, INPUT_PULLUP);
    digitalWrite(BUZZER, LOW);

    // --- I2C + MPU6050 ---
    Wire.begin();
    mpu.initialize();
    if (!mpu.testConnection()) {
        Serial.println("[ERROR] MPU6050 connection failed!");
    } else {
        Serial.println("[OK] MPU6050 initialized");
    }

    // --- GPS Serial ---
    gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
    Serial.println("[OK] GPS Serial started");

    // --- SIM800L Serial ---
    simSerial.begin(9600, SERIAL_8N1, SIM_RX, SIM_TX);
    delay(3000);
    sendATCommand("AT");
    sendATCommand("AT+CMGF=1");  // SMS text mode
    sendATCommand("AT+CLIP=1");  // Caller ID
    Serial.println("[OK] SIM800L initialized");

    // --- BLE Setup ---
    setupBLE();

    // --- GPRS Setup ---
    Serial.println("[INIT] Starting modem...");
    if (modem.restart()) {
        Serial.println("[OK] Modem restarted");
        if (modem.gprsConnect(apn)) {
            Serial.println("[OK] GPRS connected");
            // Send heartbeat on boot
            sendHeartbeat();
            // Check for remote config updates
            syncConfigFromDjango();
        } else {
            Serial.println("[WARN] GPRS connection failed - will retry");
        }
    } else {
        Serial.println("[WARN] Modem restart failed - will retry");
    }

    Serial.println("========================================");
    Serial.println("  SYSTEM READY!");
    Serial.println("========================================");
}

// ==================== BLE INITIALIZATION ====================
void setupBLE() {
    BLEDevice::init("SmartBike_BLE");
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    BLEService* pService = pServer->createService(SERVICE_UUID);

    // Sensor data characteristic (Notify)
    pSensorCharacteristic = pService->createCharacteristic(
        CHAR_SENSOR_UUID,
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
    );
    pSensorCharacteristic->addDescriptor(new BLE2902());

    // Command characteristic (Write)
    pCommandCharacteristic = pService->createCharacteristic(
        CHAR_COMMAND_UUID,
        BLECharacteristic::PROPERTY_WRITE
    );
    pCommandCharacteristic->setCallbacks(new CommandCallbacks());

    pService->start();

    BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);
    pAdvertising->setMinPreferred(0x12);
    BLEDevice::startAdvertising();

    Serial.println("[OK] BLE advertising started as 'SmartBike_BLE'");
}

// ==================== MAIN LOOP ====================
void loop() {
    // --- Process GPS data ---
    while (gpsSerial.available()) {
        gps.encode(gpsSerial.read());
    }
    if (gps.location.isValid()) {
        latitude  = gps.location.lat();
        longitude = gps.location.lng();
    }

    // --- Read Sensors ---
    int16_t ax, ay, az, gx, gy, gz;
    mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);

    float impactForce = sqrt((float)ax * ax + (float)ay * ay + (float)az * az);
    float rollAngle   = atan2((float)ay, (float)az) * 180.0 / PI;
    float bikeTilt    = abs(rollAngle);

    int vibData = analogRead(VIB_PIN);
    int fsrData = analogRead(FSR_PIN);

    bool isUpright    = (bikeTilt < 45.0);
    bool isFallen     = (bikeTilt > 60.0);
    bool isCrashImpact = (impactForce > 20000);

    // --- Send sensor data via BLE (every loop) ---
    if (deviceConnected) {
        StaticJsonDocument<256> doc;
        doc["vib"]    = vibData;
        doc["fsr"]    = fsrData;
        doc["fall"]   = impactForce;
        doc["tilt"]   = bikeTilt;
        doc["lat"]    = latitude;
        doc["lng"]    = longitude;
        doc["status"] = crashScenario;
        doc["silent"] = silentMode;

        String jsonStr;
        serializeJson(doc, jsonStr);
        pSensorCharacteristic->setValue(jsonStr.c_str());
        pSensorCharacteristic->notify();
    }

    // --- Print to Serial Monitor ---
    Serial.print("Impact: "); Serial.print(impactForce);
    Serial.print(" | Tilt: "); Serial.print(bikeTilt);
    Serial.print(" | Vib: "); Serial.print(vibData);
    Serial.print(" | FSR: "); Serial.println(fsrData);

    // ===== CRASH DETECTION LOGIC =====
    if (!emergencyActive) {
        // SCENARIO 1: Parked Bike Hit
        if (fsrData < 1000 && vibData > 1000 && vibData < 4096 && isUpright) {
            handleScenario(1, vibData, fsrData, impactForce);
        }
        // SCENARIO 2: Minor Collision
        else if (vibData < 1000 && fsrData > 1000 && fsrData < 4096 && isUpright) {
            handleScenario(2, vibData, fsrData, impactForce);
        }
        // SCENARIO 3 & 4: Major Accident (with countdown)
        else if ((vibData > 4000) && (fsrData) && (isCrashImpact || isFallen)) {
            handleMajorCrash(vibData, fsrData, impactForce);
        }
    }

    // --- Periodic GPRS Sync ---
    if (millis() - lastGPRSSync > GPRS_SYNC_INTERVAL) {
        lastGPRSSync = millis();
        pushStatusToDjango(false);
    }

    // --- Periodic Heartbeat ---
    if (millis() - lastHeartbeat > HEARTBEAT_INTERVAL) {
        lastHeartbeat = millis();
        sendHeartbeat();
    }

    delay(500);
}

// ==================== SCENARIO HANDLERS ====================

void handleScenario(int scenario, int vib, int fsr, float impact) {
    crashScenario = scenario;
    Serial.print("[ALERT] Scenario ");
    Serial.print(scenario);
    Serial.println(" detected!");

    if (!silentMode) {
        digitalWrite(BUZZER, HIGH);
        delay(2000);
        digitalWrite(BUZZER, LOW);
    }

    // Notify app via BLE
    notifyApp(scenario, 0);

    // Push to Django
    pushStatusToDjango(false);

    // Send SMS
    if (scenario == 1) {
        sendSMS(emergencyNumber,
            "Alert!! Your parked bike was hit. Impact detected. Please check immediately.\nLocation: https://maps.google.com/?q=" +
            String(latitude, 6) + "," + String(longitude, 6));
    } else {
        sendSMS(emergencyNumber,
            "Alert! Minor collision detected on your bike.\nLocation: https://maps.google.com/?q=" +
            String(latitude, 6) + "," + String(longitude, 6));
    }

    crashScenario = 0;
}

void handleMajorCrash(int vib, int fsr, float impact) {
    emergencyActive = true;
    cancelRequested = false;
    crashScenario = 3;
    int countdown = 0;
    const int COUNTDOWN_MAX = 20;  // 20 * 500ms = 10 seconds

    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    Serial.println("  MAJOR CRASH DETECTED!");
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

    // Push crash notification to Django immediately
    pushStatusToDjango(true);

    // --- THE COUNTDOWN LOOP ---
    while (countdown < COUNTDOWN_MAX) {
        // Beep pattern during countdown
        if (!silentMode) {
            digitalWrite(BUZZER, HIGH);
            delay(200);
            digitalWrite(BUZZER, LOW);
            delay(300);
        } else {
            delay(500);
        }

        countdown++;
        int timeLeft = (COUNTDOWN_MAX - countdown) / 2;  // seconds remaining

        Serial.print("[COUNTDOWN] ");
        Serial.print(timeLeft);
        Serial.println("s remaining... Press button or tap 'I'm Okay'");

        // Notify BLE app with countdown
        notifyApp(3, timeLeft);

        // CHECK: Physical button press
        if (digitalRead(SWT) == LOW) {
            Serial.println("[CANCEL] Button pressed - Rider is conscious!");
            crashScenario = 0;
            emergencyActive = false;
            digitalWrite(BUZZER, LOW);

            notifyApp(0, 0);

            // Send "rider is okay" SMS
            sendSMS(emergencyNumber,
                "ACCIDENT ALERT CANCELLED: Impact was detected, but rider is conscious and responsive.\nLocation: https://maps.google.com/?q=" +
                String(latitude, 6) + "," + String(longitude, 6));

            // Update Django
            pushCancelToDjango();
            return;
        }

        // CHECK: App cancel via BLE
        if (cancelRequested) {
            Serial.println("[CANCEL] App 'I'm Okay' received - Rider is conscious!");
            cancelRequested = false;
            crashScenario = 0;
            emergencyActive = false;
            digitalWrite(BUZZER, LOW);

            notifyApp(0, 0);

            sendSMS(emergencyNumber,
                "ACCIDENT ALERT CANCELLED: Impact was detected, but rider confirmed safe via app.\nLocation: https://maps.google.com/?q=" +
                String(latitude, 6) + "," + String(longitude, 6));

            pushCancelToDjango();
            return;
        }
    }

    // --- COUNTDOWN EXPIRED: RIDER UNRESPONSIVE ---
    crashScenario = 4;
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    Serial.println("  RIDER UNRESPONSIVE - SENDING SOS!");
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

    if (!silentMode) {
        digitalWrite(BUZZER, HIGH);
    }

    notifyApp(4, 0);

    // 1. Send Emergency SMS
    sendSMS(emergencyNumber,
        "EMERGENCY SOS!! Rider is unresponsive after a crash!\nIMPACT DETECTED - IMMEDIATE HELP NEEDED!\nLocation: https://maps.google.com/?q=" +
        String(latitude, 6) + "," + String(longitude, 6));

    delay(3000);

    // 2. Make Emergency Call
    makeCall(emergencyNumber);

    // 3. Log accident in Django
    pushStatusToDjango(true);

    delay(5000);
    digitalWrite(BUZZER, LOW);
    crashScenario = 0;
    emergencyActive = false;
}

// ==================== BLE NOTIFICATION ====================
void notifyApp(int status, int countdown) {
    if (deviceConnected) {
        StaticJsonDocument<128> doc;
        doc["status"]    = status;
        doc["countdown"] = countdown;
        doc["lat"]       = latitude;
        doc["lng"]       = longitude;

        String jsonStr;
        serializeJson(doc, jsonStr);
        pSensorCharacteristic->setValue(jsonStr.c_str());
        pSensorCharacteristic->notify();
    }
}

// ==================== DJANGO API CALLS ====================

void pushStatusToDjango(bool isCrashed) {
    if (!modem.isGprsConnected()) {
        Serial.println("[GPRS] Reconnecting...");
        if (!modem.gprsConnect(apn)) {
            Serial.println("[GPRS] Failed to reconnect");
            return;
        }
    }

    StaticJsonDocument<256> doc;
    doc["bike_id"]    = bike_id;
    doc["lat"]        = latitude;
    doc["lng"]        = longitude;
    doc["is_crashed"] = isCrashed;
    doc["silent_mode"] = silentMode;
    doc["emergency_number"] = emergencyNumber;

    String postData;
    serializeJson(doc, postData);

    HttpClient http(gprsClient, server, serverPort);
    http.post("/api/update_status/", "application/json", postData);

    int statusCode = http.responseStatusCode();
    Serial.print("[DJANGO] Sync status: ");
    Serial.println(statusCode);
    http.stop();
}

void sendHeartbeat() {
    if (!modem.isGprsConnected()) {
        if (!modem.gprsConnect(apn)) return;
    }

    StaticJsonDocument<128> doc;
    doc["bike_id"] = bike_id;
    doc["type"]    = "heartbeat";

    String postData;
    serializeJson(doc, postData);

    HttpClient http(gprsClient, server, serverPort);
    http.post("/api/heartbeat/", "application/json", postData);

    int statusCode = http.responseStatusCode();
    String response = http.responseBody();
    Serial.print("[HEARTBEAT] Status: ");
    Serial.println(statusCode);

    // Check if Django has updated config
    if (statusCode == 200) {
        StaticJsonDocument<256> respDoc;
        DeserializationError err = deserializeJson(respDoc, response);
        if (!err) {
            if (respDoc.containsKey("emergency_number")) {
                String newNum = respDoc["emergency_number"].as<String>();
                if (newNum != emergencyNumber && newNum.length() > 5) {
                    emergencyNumber = newNum;
                    preferences.begin("bike", false);
                    preferences.putString("emNum", emergencyNumber);
                    preferences.end();
                    Serial.print("[SYNC] Emergency number updated from cloud: ");
                    Serial.println(emergencyNumber);
                }
            }
            if (respDoc.containsKey("silent_mode")) {
                silentMode = respDoc["silent_mode"].as<bool>();
                preferences.begin("bike", false);
                preferences.putBool("silent", silentMode);
                preferences.end();
            }
        }
    }
    http.stop();
}

void syncConfigFromDjango() {
    if (!modem.isGprsConnected()) return;

    HttpClient http(gprsClient, server, serverPort);
    String path = "/api/get_config/?bike_id=" + String(bike_id);
    http.get(path);

    int statusCode = http.responseStatusCode();
    if (statusCode == 200) {
        String response = http.responseBody();
        StaticJsonDocument<256> doc;
        DeserializationError err = deserializeJson(doc, response);
        if (!err) {
            if (doc.containsKey("emergency_number")) {
                String newNum = doc["emergency_number"].as<String>();
                if (newNum.length() > 5) {
                    emergencyNumber = newNum;
                    preferences.begin("bike", false);
                    preferences.putString("emNum", emergencyNumber);
                    preferences.end();
                }
            }
            if (doc.containsKey("silent_mode")) {
                silentMode = doc["silent_mode"].as<bool>();
                preferences.begin("bike", false);
                preferences.putBool("silent", silentMode);
                preferences.end();
            }
        }
    }
    http.stop();
}

void pushCancelToDjango() {
    if (!modem.isGprsConnected()) {
        if (!modem.gprsConnect(apn)) return;
    }

    StaticJsonDocument<128> doc;
    doc["bike_id"] = bike_id;
    doc["action"]  = "cancel_emergency";

    String postData;
    serializeJson(doc, postData);

    HttpClient http(gprsClient, server, serverPort);
    http.post("/api/cancel_emergency/", "application/json", postData);
    Serial.print("[DJANGO] Cancel status: ");
    Serial.println(http.responseStatusCode());
    http.stop();
}

// ==================== SMS & CALL ====================

void sendSMS(String number, String message) {
    Serial.print("[SMS] Sending to: ");
    Serial.println(number);

    simSerial.println("AT+CMGF=1");
    delay(1000);
    while (simSerial.available()) Serial.write(simSerial.read());

    simSerial.println("AT+CMGS=\"" + number + "\"");
    delay(1000);
    simSerial.print(message);
    delay(500);
    simSerial.write(26);  // Ctrl+Z
    delay(3000);

    Serial.println("[SMS] Sent!");
}

void makeCall(String number) {
    Serial.print("[CALL] Dialing: ");
    Serial.println(number);
    simSerial.println("ATD" + number + ";");
    delay(15000);  // Let it ring for 15 seconds
    simSerial.println("ATH");  // Hang up
    Serial.println("[CALL] Complete");
}

void sendATCommand(String command) {
    Serial.print("[AT] Sending: ");
    Serial.println(command);
    simSerial.println(command);
    delay(1000);
    while (simSerial.available()) {
        Serial.write(simSerial.read());
    }
}
