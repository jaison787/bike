/*
 * ============================================================
 * SMART BIKE CRASH DETECTION & SAFETY SYSTEM (ESP32)
 * ============================================================
 * FINAL WORKING VERSION - Matches Django Backend + Flutter App
 * 
 * Hardware: ESP32 + MPU6050 + NEO-6M GPS + SIM800L + Buzzer + Button
 * Communication: Classic Bluetooth (for flutter_bluetooth_serial) + GPRS/HTTP
 * Backend: Django REST API
 * 
 * SYSTEM FLOW:
 * 1. Boot → Load saved config from Preferences → Start BT & GPRS
 * 2. Local Loop (Bluetooth): App connects, exchanges data in real-time
 * 3. Remote Loop (GPRS): Periodic GPS + status sync to Django
 * 4. Emergency: Crash → 10s countdown → SMS/Call if unresponsive
 * ============================================================
 */

#define TINY_GSM_MODEM_SIM800
#include <TinyGsmClient.h>
#include <ArduinoHttpClient.h>
#include <HardwareSerial.h>
#include <TinyGPS++.h>
#include <Wire.h>
#include <MPU6050.h>
#include "BluetoothSerial.h"
#include <Preferences.h>

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth is not enabled! Please run `make menuconfig` to enable it
#endif

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
const char apn[]      = "internet";  // Change to your SIM APN (e.g. "airtelgprs.com")
const char bike_id[]  = "BIKE_001";

// ==================== OBJECTS ====================
BluetoothSerial SerialBT;
MPU6050 mpu;
TinyGPSPlus gps;
HardwareSerial simSerial(1);
HardwareSerial gpsSerial(2);
TinyGsm modem(simSerial);
TinyGsmClient gprsClient(modem);
Preferences preferences;

// ==================== STATE VARIABLES ====================
String emergencyNumber = "+910000000000";
String emergencyName   = "Emergency";
bool   silentMode      = false;
bool   crashDetected   = false;
bool   emergencyActive = false;
bool   cancelRequested = false;
int    crashScenario   = 0;

float latitude  = 0.0;
float longitude = 0.0;

int16_t ax, ay, az;
int16_t gx, gy, gz;

unsigned long lastGPRSSync   = 0;
unsigned long lastHeartbeat  = 0;
const unsigned long GPRS_SYNC_INTERVAL  = 10000;   // 10 seconds
const unsigned long HEARTBEAT_INTERVAL  = 300000;  // 5 minutes

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

    // --- Bluetooth Classic (for flutter_bluetooth_serial) ---
    SerialBT.begin("SmartBike_System");
    Serial.println("[OK] Bluetooth started as 'SmartBike_System'");

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
        Serial.println("[WARN] Modem restart failed - SMS/Call still available");
    }

    Serial.println("========================================");
    Serial.println("  SYSTEM READY - MONITORING ACTIVE");
    Serial.println("========================================");
}

// ==================== MAIN LOOP ====================
void loop() {
    // --- Check Bluetooth commands from Flutter App ---
    handleBluetoothInput();

    // --- Process GPS data ---
    while (gpsSerial.available()) {
        gps.encode(gpsSerial.read());
    }
    if (gps.location.isValid()) {
        latitude  = gps.location.lat();
        longitude = gps.location.lng();
    }

    // --- Read Sensors ---
    mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);

    float impactForce = sqrt((float)ax * ax + (float)ay * ay + (float)az * az);
    float rollAngle   = atan2((float)ay, (float)az) * 180.0 / PI;
    float bikeTilt    = abs(rollAngle);

    int vibData = analogRead(VIB_PIN);
    int fsrData = analogRead(FSR_PIN);

    bool isUpright     = (bikeTilt < 45.0);
    bool isFallen      = (bikeTilt > 60.0);
    bool isCrashImpact = (impactForce > 20000);

    // --- Send sensor data via Bluetooth (every loop ~1s) ---
    String dataPacket = "{\"vib\":" + String(vibData) +
                        ",\"fsr\":" + String(fsrData) +
                        ",\"fall\":" + String(impactForce) +
                        ",\"tilt\":" + String(bikeTilt) +
                        ",\"lat\":" + String(latitude, 6) +
                        ",\"lng\":" + String(longitude, 6) +
                        ",\"status\":" + String(crashScenario) +
                        ",\"silent\":" + String(silentMode ? "true" : "false") + "}";
    SerialBT.println(dataPacket);

    // --- Print to Serial Monitor ---
    Serial.print("Impact: "); Serial.print(impactForce);
    Serial.print(" | Tilt: "); Serial.print(bikeTilt);
    Serial.print(" | Vib: "); Serial.print(vibData);
    Serial.print(" | FSR: "); Serial.println(fsrData);

    delay(1000);

    // ===== CRASH DETECTION LOGIC =====
    if (!emergencyActive) {
        // SCENARIO 1: Parked Bike Hit
        if (fsrData < 1000 && vibData > 1000 && vibData < 4096 && isUpright) {
            handleMinorAlert(1, vibData, fsrData, impactForce);
        }
        // SCENARIO 2: Minor Collision
        else if (vibData < 1000 && fsrData > 1000 && fsrData < 4096 && isUpright) {
            handleMinorAlert(2, vibData, fsrData, impactForce);
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
}

// ==================== BLUETOOTH INPUT HANDLER ====================
void handleBluetoothInput() {
    if (SerialBT.available()) {
        String incoming = SerialBT.readStringUntil('\n');
        incoming.trim();
        Serial.print("[BT] Received: ");
        Serial.println(incoming);

        if (incoming.startsWith("PHONE:")) {
            emergencyNumber = incoming.substring(6);
            preferences.begin("bike", false);
            preferences.putString("emNum", emergencyNumber);
            preferences.end();
            Serial.print("[BT] Emergency number updated: ");
            Serial.println(emergencyNumber);
        }
        else if (incoming.startsWith("NAME:")) {
            emergencyName = incoming.substring(5);
            preferences.begin("bike", false);
            preferences.putString("emName", emergencyName);
            preferences.end();
            Serial.print("[BT] Emergency name updated: ");
            Serial.println(emergencyName);
        }
        else if (incoming == "SILENT:ON") {
            silentMode = true;
            preferences.begin("bike", false);
            preferences.putBool("silent", true);
            preferences.end();
            Serial.println("[BT] Silent mode ON");
        }
        else if (incoming == "SILENT:OFF") {
            silentMode = false;
            preferences.begin("bike", false);
            preferences.putBool("silent", false);
            preferences.end();
            Serial.println("[BT] Silent mode OFF");
        }
        else if (incoming == "CANCEL") {
            cancelRequested = true;
            Serial.println("[BT] Cancel requested from app!");
        }
    }
}

// ==================== SCENARIO HANDLERS ====================

void handleMinorAlert(int scenario, int vib, int fsr, float impact) {
    crashScenario = scenario;
    Serial.print("[ALERT] Scenario ");
    Serial.print(scenario);
    Serial.println(" detected!");

    // Buzz
    if (!silentMode) {
        digitalWrite(BUZZER, HIGH);
        delay(2000);
        digitalWrite(BUZZER, LOW);
    }

    // Notify app via Bluetooth
    SerialBT.println("{\"status\":" + String(scenario) + "}");

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
    int countLoop = 0;
    const int COUNTDOWN_MAX = 50;  // 50 iterations × ~200ms = ~10 seconds

    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    Serial.println("  MAJOR CRASH DETECTED!");
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

    // Push crash notification to Django immediately
    pushStatusToDjango(true);

    // --- THE COUNTDOWN LOOP (10 seconds) ---
    while (countLoop < COUNTDOWN_MAX) {
        // Beep pattern during countdown
        if (!silentMode) {
            digitalWrite(BUZZER, HIGH);
        }
        countLoop++;
        delay(200);

        if (!silentMode) {
            digitalWrite(BUZZER, LOW);
        }

        int timeLeft = COUNTDOWN_MAX - countLoop;

        Serial.print("[COUNTDOWN] ");
        Serial.print(timeLeft);
        Serial.println(" remaining... Press button or tap 'I'm Okay'");

        // Notify BLE app with countdown
        SerialBT.println("{\"status\":3, \"countdown\":" + String(timeLeft) + "}");

        // CHECK: Physical button press (SCENARIO 3 - Rider Conscious)
        if (digitalRead(SWT) == LOW) {
            Serial.println("[CANCEL] Button pressed - Rider is conscious!");
            crashScenario = 0;
            emergencyActive = false;
            digitalWrite(BUZZER, LOW);

            SerialBT.println("{\"status\":0}");

            // Send "rider is okay" SMS
            sendSMS(emergencyNumber,
                "ACCIDENT ALERT CANCELLED: Impact was detected, but rider is conscious and responsive.\nLocation: https://maps.google.com/?q=" +
                String(latitude, 6) + "," + String(longitude, 6));

            pushCancelToDjango();
            return;
        }

        // CHECK: App cancel via Bluetooth
        if (SerialBT.available()) {
            String incoming = SerialBT.readStringUntil('\n');
            incoming.trim();
            if (incoming == "CANCEL") {
                cancelRequested = true;
            }
        }

        if (cancelRequested) {
            Serial.println("[CANCEL] App 'I'm Okay' received!");
            cancelRequested = false;
            crashScenario = 0;
            emergencyActive = false;
            digitalWrite(BUZZER, LOW);

            SerialBT.println("{\"status\":0}");

            sendSMS(emergencyNumber,
                "ACCIDENT ALERT CANCELLED: Impact was detected, but rider confirmed safe via app.\nLocation: https://maps.google.com/?q=" +
                String(latitude, 6) + "," + String(longitude, 6));

            pushCancelToDjango();
            return;
        }
    }

    // --- COUNTDOWN EXPIRED: RIDER UNRESPONSIVE (SCENARIO 4) ---
    crashScenario = 4;
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    Serial.println("  RIDER UNRESPONSIVE - SENDING SOS!");
    Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

    if (!silentMode) {
        digitalWrite(BUZZER, HIGH);
    }

    SerialBT.println("{\"status\":4}");

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

// ==================== DJANGO API CALLS ====================

void pushStatusToDjango(bool isCrashed) {
    if (!modem.isGprsConnected()) {
        Serial.println("[GPRS] Reconnecting...");
        if (!modem.gprsConnect(apn)) {
            Serial.println("[GPRS] Failed to reconnect - skipping sync");
            return;
        }
    }

    String postData = "{\"bike_id\":\"" + String(bike_id) + "\","
                      "\"lat\":" + String(latitude, 6) + ","
                      "\"lng\":" + String(longitude, 6) + ","
                      "\"is_crashed\":" + String(isCrashed ? "true" : "false") + ","
                      "\"silent_mode\":" + String(silentMode ? "true" : "false") + ","
                      "\"emergency_number\":\"" + emergencyNumber + "\"}";

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

    String postData = "{\"bike_id\":\"" + String(bike_id) + "\",\"type\":\"heartbeat\"}";

    HttpClient http(gprsClient, server, serverPort);
    http.post("/api/heartbeat/", "application/json", postData);

    int statusCode = http.responseStatusCode();
    String response = http.responseBody();
    Serial.print("[HEARTBEAT] Status: ");
    Serial.println(statusCode);

    // Parse the response to check for config updates
    if (statusCode == 200) {
        // Simple JSON parsing for emergency_number
        int numStart = response.indexOf("emergency_number");
        if (numStart > 0) {
            int valStart = response.indexOf("\"", numStart + 18) + 1;
            int valEnd = response.indexOf("\"", valStart);
            if (valStart > 0 && valEnd > valStart) {
                String newNum = response.substring(valStart, valEnd);
                if (newNum != emergencyNumber && newNum.length() > 5) {
                    emergencyNumber = newNum;
                    preferences.begin("bike", false);
                    preferences.putString("emNum", emergencyNumber);
                    preferences.end();
                    Serial.print("[SYNC] Emergency number from cloud: ");
                    Serial.println(emergencyNumber);
                }
            }
        }

        // Check silent mode
        if (response.indexOf("\"silent_mode\": true") > 0 || response.indexOf("\"silent_mode\":true") > 0) {
            if (!silentMode) {
                silentMode = true;
                preferences.begin("bike", false);
                preferences.putBool("silent", true);
                preferences.end();
                Serial.println("[SYNC] Silent mode set to ON from cloud");
            }
        } else if (response.indexOf("\"silent_mode\": false") > 0 || response.indexOf("\"silent_mode\":false") > 0) {
            if (silentMode) {
                silentMode = false;
                preferences.begin("bike", false);
                preferences.putBool("silent", false);
                preferences.end();
                Serial.println("[SYNC] Silent mode set to OFF from cloud");
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
        Serial.print("[SYNC] Config response: ");
        Serial.println(response);

        // Parse emergency number
        int numStart = response.indexOf("emergency_number");
        if (numStart > 0) {
            int valStart = response.indexOf("\"", numStart + 18) + 1;
            int valEnd = response.indexOf("\"", valStart);
            if (valStart > 0 && valEnd > valStart) {
                String newNum = response.substring(valStart, valEnd);
                if (newNum.length() > 5) {
                    emergencyNumber = newNum;
                    preferences.begin("bike", false);
                    preferences.putString("emNum", emergencyNumber);
                    preferences.end();
                    Serial.print("[SYNC] Emergency number loaded: ");
                    Serial.println(emergencyNumber);
                }
            }
        }
    }
    http.stop();
}

void pushCancelToDjango() {
    if (!modem.isGprsConnected()) {
        if (!modem.gprsConnect(apn)) return;
    }

    String postData = "{\"bike_id\":\"" + String(bike_id) + "\",\"action\":\"cancel_emergency\"}";

    HttpClient http(gprsClient, server, serverPort);
    http.post("/api/cancel_emergency/", "application/json", postData);
    Serial.print("[DJANGO] Cancel status: ");
    Serial.println(http.responseStatusCode());
    http.stop();
}

// ==================== SMS & CALL FUNCTIONS ====================

void sendSMS(String number, String message) {
    Serial.print("[SMS] Sending to: ");
    Serial.println(number);

    simSerial.println("AT+CMGF=1");
    delay(1000);
    while (simSerial.available()) { Serial.write(simSerial.read()); }

    simSerial.println("AT+CMGS=\"" + number + "\"");
    delay(1000);
    simSerial.print(message);
    delay(500);
    simSerial.write(26);  // Ctrl+Z to send
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