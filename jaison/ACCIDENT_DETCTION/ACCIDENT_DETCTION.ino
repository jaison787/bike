/*
 * ============================================================
 * SMART BIKE CRASH DETECTION & SAFETY SYSTEM (ESP32)
 * ============================================================
 * FINAL WORKING VERSION - Ngrok + HTTP Fixes Applied
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
// EXACT Ngrok domain (NO http:// and NO trailing slashes here)
const char server[]   = "unidling-kirsten-suprasegmental.ngrok-free.dev"; 
const int  serverPort = 80; // MUST BE 80 FOR HTTP
const char apn[]      = "internet";  // Change to your SIM APN if needed
const char bike_id[]  = "BIKE_001";

// ==================== OBJECTS ====================
BluetoothSerial SerialBT;
MPU6050 mpu;
TinyGPSPlus gps;
HardwareSerial simSerial(1);
HardwareSerial gpsSerial(2);
TinyGsm modem(simSerial);
TinyGsmClient gprsClient(modem); // Standard Non-Secure Client!
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
const unsigned long GPRS_SYNC_INTERVAL  = 300000;  // 5 minutes
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

unsigned long lastPrintTime = 0;
const unsigned long PRINT_INTERVAL = 500; // reporting interval

// ==================== MAIN LOOP ====================
void loop() {
    // 1. Process GPS data (Non-blocking)
    while (gpsSerial.available()) {
        gps.encode(gpsSerial.read());
    }
    if (gps.location.isValid()) {
        latitude  = gps.location.lat();
        longitude = gps.location.lng();
    }

    // 2. Read Sensors (Instant)
    mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);
    float impactForce = sqrt((float)ax * ax + (float)ay * ay + (float)az * az);
    float rollAngle   = atan2((float)ay, (float)az) * 180.0 / PI;
    float bikeTilt    = abs(rollAngle);
    int vibData = analogRead(VIB_PIN);
    int fsrData = analogRead(FSR_PIN);

    bool isUpright     = (bikeTilt < 45.0);
    bool isFallen      = (bikeTilt > 60.0);
    bool isCrashImpact = (impactForce > 20000);

    // 3. CRASH DETECTION (High Priority - Instant response)
    if (!emergencyActive) {
        if (isCrashImpact || isFallen) {
            handleMajorCrash(vibData, fsrData, impactForce);
        }
        else if (vibData > 800 && fsrData < 500 && isUpright) {
            handleMinorAlert(1, vibData, fsrData, impactForce);
        }
        else if (vibData > 800 && fsrData > 500 && isUpright) {
            handleMinorAlert(2, vibData, fsrData, impactForce);
        }
    }

    // 4. PERIODIC TASKS (Reporting & Syncing)
    unsigned long currentMillis = millis();

    // Check Bluetooth commands from Flutter App
    handleBluetoothInput();

    // reporting task (BT data + Serial)
    if (currentMillis - lastPrintTime >= PRINT_INTERVAL) {
        lastPrintTime = currentMillis;

        // Send sensor data via Bluetooth
        String dataPacket = "{\"vib\":" + String(vibData) +
                            ",\"fsr\":" + String(fsrData) +
                            ",\"fall\":" + String(impactForce) +
                            ",\"tilt\":" + String(bikeTilt) +
                            ",\"lat\":" + String(latitude, 6) +
                            ",\"lng\":" + String(longitude, 6) +
                            ",\"status\":" + String(crashScenario) +
                            ",\"silent\":" + String(silentMode ? "true" : "false") + "}";
        SerialBT.println(dataPacket);

        // Print to Serial Monitor
        Serial.print("Impact: "); Serial.print(impactForce);
        Serial.print(" | Tilt: "); Serial.print(bikeTilt);
        Serial.print(" | Vib: "); Serial.print(vibData);
        Serial.print(" | FSR: "); Serial.println(fsrData);
    }

    // GPRS Sync
    if (currentMillis - lastGPRSSync > GPRS_SYNC_INTERVAL) {
        lastGPRSSync = currentMillis;
        pushStatusToDjango(false, vibData, fsrData, impactForce, bikeTilt);
    }

    // Heartbeat
    if (currentMillis - lastHeartbeat > HEARTBEAT_INTERVAL) {
        lastHeartbeat = currentMillis;
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
    pushStatusToDjango(false, vib, fsr, impact, 0); // Upright scenario

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
    pushStatusToDjango(true, vib, fsr, impact, 90.0); // Assume tilted heavily

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
    pushStatusToDjango(true, vib, fsr, impact, 90.0);

    delay(5000);
    digitalWrite(BUZZER, LOW);
    crashScenario = 0;
    emergencyActive = false;
}

// ==================== DJANGO API CALLS ====================

void pushStatusToDjango(bool isCrashed, int vib, int fsr, float impact, float tilt) {
    if (!modem.isGprsConnected()) {
        Serial.println("[GPRS] Reconnecting...");
        if (!modem.gprsConnect(apn)) {
            Serial.println("[GPRS] Failed to reconnect - skipping sync");
            return;
        }
    }

    // --- Build JSON payload (Numeric values must be bare) ---
    String postData = "{";
    postData += "\"bike_id\":\"" + String(bike_id) + "\",";
    postData += "\"lat\":" + String(latitude, 6) + ",";
    postData += "\"lng\":" + String(longitude, 6) + ",";
    postData += "\"is_crashed\":" + String(isCrashed ? "true" : "false") + ",";
    postData += "\"silent_mode\":" + String(silentMode ? "true" : "false") + ",";
    postData += "\"emergency_number\":\"" + emergencyNumber + "\",";
    postData += "\"vibration\":" + String(vib) + ",";
    postData += "\"fsr\":" + String(fsr) + ",";
    postData += "\"impact_force\":" + String(impact) + ",";
    postData += "\"tilt_angle\":" + String(tilt);
    postData += "}";

    Serial.println("[DEBUG] Body: " + postData);

    HttpClient http(gprsClient, server, serverPort);
    http.setHttpResponseTimeout(15000); 
    
    Serial.println("[DJANGO] Syncing...");
    
    http.beginRequest();
    http.post("/api/update_status/");
    
    // THE NGROK FIXES
    http.sendHeader("ngrok-skip-browser-warning", "true");
    http.sendHeader("Content-Type", "application/json");
    http.sendHeader("Content-Length", postData.length());
    http.sendHeader("User-Agent", "ESP32-SmartBike");
    
    http.endRequest();
    http.print(postData);

    int statusCode = http.responseStatusCode();
    Serial.print("[DJANGO] Sync result: ");
    Serial.println(statusCode);
    
    if (statusCode < 0) {
        Serial.println("[WARN] Connection failed.");
    }
    http.stop();
}

void sendHeartbeat() {
    if (!modem.isGprsConnected()) {
        if (!modem.gprsConnect(apn)) return;
    }

    String postData = "{\"bike_id\":\"" + String(bike_id) + "\",\"type\":\"heartbeat\"}";

    HttpClient http(gprsClient, server, serverPort);
    http.setHttpResponseTimeout(15000);
    
    http.beginRequest();
    http.post("/api/heartbeat/");
    
    // THE NGROK FIXES
    http.sendHeader("ngrok-skip-browser-warning", "true");
    http.sendHeader("Content-Type", "application/json");
    http.sendHeader("Content-Length", postData.length());
    http.sendHeader("User-Agent", "ESP32-SmartBike");
    
    http.endRequest();
    http.print(postData);

    int statusCode = http.responseStatusCode();
    Serial.print("[HEARTBEAT] Status: ");
    Serial.println(statusCode);

    if (statusCode == 200) {
        String response = http.responseBody();
        
        // Manual parsing if library is missing
        if (response.indexOf("emergency_number") > 0) {
            int start = response.indexOf("emergency_number") + 18;
            int q1 = response.indexOf("\"", start) + 1;
            int q2 = response.indexOf("\"", q1);
            if (q1 > 0 && q2 > q1) {
                String newNum = response.substring(q1, q2);
                if (newNum != emergencyNumber && newNum.length() > 5) {
                    emergencyNumber = newNum;
                    preferences.begin("bike", false);
                    preferences.putString("emNum", emergencyNumber);
                    preferences.end();
                    Serial.println("[SYNC] Emergency Number updated from Cloud");
                }
            }
        }
        
        if (response.indexOf("silent_mode") > 0) {
            bool cloudSilent = (response.indexOf("\"silent_mode\":true") > 0 || response.indexOf("\"silent_mode\": true") > 0);
            if (cloudSilent != silentMode) {
                silentMode = cloudSilent;
                preferences.begin("bike", false);
                preferences.putBool("silent", silentMode);
                preferences.end();
                Serial.println("[SYNC] Silent Mode updated from Cloud");
            }
        }
    }
    http.stop();
}

void syncConfigFromDjango() {
    if (!modem.isGprsConnected()) return;

    HttpClient http(gprsClient, server, serverPort);
    String path = "/api/get_config/?bike_id=" + String(bike_id);
    
    // We changed this from http.get(path) to allow the Ngrok header!
    http.beginRequest();
    http.get(path);
    http.sendHeader("ngrok-skip-browser-warning", "true");
    http.endRequest();

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
    
    // We changed this from shorthand http.post() to allow the Ngrok header!
    http.beginRequest();
    http.post("/api/cancel_emergency/");
    http.sendHeader("ngrok-skip-browser-warning", "true");
    http.sendHeader("Content-Type", "application/json");
    http.sendHeader("Content-Length", postData.length());
    http.endRequest();
    http.print(postData);
    
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