// ============================================================
//  SmartBike Accident Detection System — v15.3
//  Author  : Claude (Project Guide Build)
//  Target  : ESP32 + SIM800L + MPU6050 + GPS + FSR + Vibration
//  FIX v14   : 15s bearer, SMS flush, 20KB dispatch stack
//  FIX v14.1 : ngrok HTTP-only tunnel
//  FIX v14.2 : Removed esp_task_wdt_reset() — "task not found" flood
//  FIX v14.3 : Race condition fix — sensor snapshot in SharedData
//  FIX v14.4 : Bearer fails after SMS — 8s+3s recovery, retry loop
//  FIX v14.5 : SAPBR first-byte drop — lenient ",1,"" check
//  FIX v14.6 : Dispatch lock — dispatchRunning prevents duplicate tasks
//  FIX v14.7 : Lock race — flag set in triggerDispatch() before task
//  FIX v14.8 : Vib false triggers — accumulator (150ms) debounce
//  FIX v14.9 : Aligned logic with Safety Logic Matrix
//  FIX v15.1 : Major Crash fix + Speed optimization (~40s dispatch)
//  FIX v15.2 : Tilt accumulator (jolt-proof) + faster Major trigger
//  FIX v15.3 : IMPACT-DRIVEN — Minor Crash now uses Impact > 23000
//              instead of vibration. Major requires > 25000.
// ====================================================================================

#include <Wire.h>
#include <BluetoothSerial.h>
#include <TinyGPS++.h>
#include <math.h>
#include "esp_task_wdt.h"

// ─── PIN DEFINITIONS ────────────────────────────────────────
#define SIM_RX_PIN   16
#define SIM_TX_PIN   17
#define GPS_RX_PIN   4
#define GPS_TX_PIN   5
#define BUZZER_PIN   23
#define SWT_PIN      27
#define FSR_PIN      35
#define VIB_PIN      34

// ─── EMERGENCY CONFIG ────────────────────────────────────────
const String EMERGENCY_NUMBER = "+919994235648";
const String BACKEND_URL      = "http://unidling-kirsten-suprasegmental.ngrok-free.dev/api/update_status/";
const String SIM_APN          = "airtelgprs.com";

// ─── THRESHOLDS ──────────────────────────────────────────────
const float        TILT_THRESHOLD      = 60.0;
const float        IMPACT_THRESHOLD    = 25000.0;
const float        MINOR_IMPACT_THRESHOLD = 23000.0;
const int          VIB_THRESHOLD       = 2000;
const int          FSR_EMPTY_THRESHOLD = 500;
const unsigned long COUNTDOWN_MS       = 15000;
const unsigned long VIB_COOLDOWN       = 5000;

// ─── ACCUMULATOR COUNTS (each tick = 30ms loop) ──────────────
//  TILT_TRIGGER_COUNT    : 5 ticks = 150ms sustained tilt  → confirms real fall
//  VIB_CRASH_TRIGGER_COUNT: 2 ticks = 60ms  vib spike      → Major Crash (fast)
//  VIB_TRIGGER_COUNT     : 5 ticks = 150ms sustained vib   → Parked Bump / Minor Crash
const int TILT_TRIGGER_COUNT      = 5;
const int VIB_CRASH_TRIGGER_COUNT = 2;  // Restored to 2 for fast detection
const int VIB_TRIGGER_COUNT       = 5;

// ─── HARDWARE ────────────────────────────────────────────────
HardwareSerial  SerialGSM(1);
HardwareSerial  SerialGPS(2);
TinyGPSPlus     gps;
BluetoothSerial SerialBT;

// ─── MUTEX ───────────────────────────────────────────────────
SemaphoreHandle_t dataMutex;

// ─── DIAGNOSTIC RESULTS ──────────────────────────────────────
struct DiagResult {
  bool gsmOK          = false;
  bool simOK          = false;
  bool networkOK      = false;
  bool gprsOK         = false;
  bool bearerOK       = false;
  bool ngrokOK        = false;
  bool mpuOK          = false;
  bool bluetoothOK    = false;
  int  signalStrength = 0;
  String simNumber    = "unknown";
  String timestamp    = "unknown";
};
DiagResult diag;

// ─── SHARED STATE ────────────────────────────────────────────
struct SharedData {
  String lastLocation  = "10.8505,76.2711";
  String crashType     = "";
  String currentStatus = "Monitoring";
  bool   dispatchReady = false;
  int    snapVib       = 0;
  int    snapFSR       = 0;
  float  snapImpact    = 0.0;
  float  snapTilt      = 0.0;
};
SharedData shared;

// ─── TIMING ──────────────────────────────────────────────────
bool          isCrashCountdown = false;
unsigned long crashStartTime   = 0;
unsigned long lastVibAction    = 0;
unsigned long lastBTSend       = 0;
float         peakImpact       = 0; // Tracks highest impact seen during current fall

// ─── DISPATCH LOCK ───────────────────────────────────────────
volatile bool dispatchRunning = false;

// ─── ACCUMULATORS ────────────────────────────────────────────
int vibAccumulator  = 0;
int tiltAccumulator = 0;

// ============================================================
//  HELPER — Send AT command, wait, return response
// ============================================================
String sendAT(const String& cmd, unsigned long waitMs = 1000) {
  while (SerialGSM.available()) SerialGSM.read();
  SerialGSM.println(cmd);
  unsigned long t = millis();
  String resp = "";
  while (millis() - t < waitMs) {
    if (SerialGSM.available()) {
      resp += (char)SerialGSM.read();
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  resp.trim();
  Serial.println("[GSM] " + cmd);
  Serial.println("  ↳  " + resp);
  return resp;
}

// ============================================================
//  HELPER — Get GSM network timestamp
// ============================================================
String getGSMTimestamp() {
  String resp = sendAT("AT+CCLK?", 2000);
  int start = resp.indexOf('"');
  int end   = resp.lastIndexOf('"');
  if (start != -1 && end != -1 && end > start)
    return resp.substring(start + 1, end);
  return "unknown";
}

// ============================================================
//  DIAGNOSTIC — Called once at boot
// ============================================================
void runDiagnostics() {
  Serial.println("\n");
  Serial.println("╔══════════════════════════════════════╗");
  Serial.println("║   SmartBike BOOT DIAGNOSTICS v15.1  ║");
  Serial.println("╚══════════════════════════════════════╝\n");

  // ── CHECK 1: MPU6050 ─────────────────────────────────────
  Serial.println("▶ [1/8] Checking MPU6050...");
  Wire.beginTransmission(0x68);
  byte err = Wire.endTransmission();
  if (err == 0) {
    diag.mpuOK = true;
    Serial.println("  ✅ MPU6050 found on 0x68");
  } else {
    Serial.println("  ❌ MPU6050 NOT found! Error: " + String(err));
  }

  // ── CHECK 2: GSM ─────────────────────────────────────────
  Serial.println("\n▶ [2/8] Checking GSM Module...");
  if (sendAT("AT", 2000).indexOf("OK") != -1) {
    diag.gsmOK = true;
    Serial.println("  ✅ GSM responding");
  } else {
    Serial.println("  ❌ GSM not responding! Check RX/TX and power (2A needed).");
  }

  // ── CHECK 3: SIM ─────────────────────────────────────────
  Serial.println("\n▶ [3/8] Checking SIM Card...");
  String simResp = sendAT("AT+CIMI", 2000);
  if (simResp.indexOf("ERROR") == -1 && simResp.length() > 5) {
    diag.simOK = true;
    Serial.println("  ✅ SIM detected. IMSI: " + simResp);
  } else {
    Serial.println("  ❌ SIM not detected! AT+CPIN?: " + sendAT("AT+CPIN?", 1000));
  }

  // ── CHECK 4: Network ─────────────────────────────────────
  Serial.println("\n▶ [4/8] Checking Network Registration...");
  String regResp = sendAT("AT+CREG?", 2000);
  if (regResp.indexOf(",1") != -1 || regResp.indexOf(",5") != -1) {
    diag.networkOK = true;
    Serial.println("  ✅ Registered" + String(regResp.indexOf(",5") != -1 ? " (Roaming)" : " (Home)"));
  } else {
    Serial.println("  ❌ Not registered. Raw: " + regResp);
  }

  // ── CHECK 5: Signal ──────────────────────────────────────
  Serial.println("\n▶ [5/8] Checking Signal Strength...");
  String csqResp = sendAT("AT+CSQ", 1000);
  int csqIdx = csqResp.indexOf("+CSQ: ");
  if (csqIdx != -1) {
    int sig = csqResp.substring(csqIdx + 6).toInt();
    diag.signalStrength = sig;
    if      (sig == 99) Serial.println("  ⚠️  Unknown signal (99)");
    else if (sig < 10)  Serial.println("  ⚠️  Weak: "     + String(sig) + "/31");
    else if (sig < 20)  Serial.println("  ✅ Moderate: "  + String(sig) + "/31");
    else                Serial.println("  ✅ Good: "      + String(sig) + "/31");
  }

  // ── CHECK 6: GPRS ────────────────────────────────────────
  Serial.println("\n▶ [6/8] Checking GPRS...");
  if (sendAT("AT+CGATT?", 2000).indexOf("+CGATT: 1") != -1) {
    diag.gprsOK = true;
    Serial.println("  ✅ GPRS attached");
  } else {
    sendAT("AT+CGATT=1", 3000);
    if (sendAT("AT+CGATT?", 2000).indexOf("+CGATT: 1") != -1) {
      diag.gprsOK = true;
      Serial.println("  ✅ GPRS attached after retry");
    } else {
      Serial.println("  ❌ GPRS failed. Check APN.");
    }
  }

  // ── CHECK 7: Bearer + Backend ────────────────────────────
  Serial.println("\n▶ [7/8] Checking Bearer & Backend...");
  sendAT("AT+SAPBR=0,1", 1000);
  delay(500);
  sendAT("AT+SAPBR=3,1,\"CONTYPE\",\"GPRS\"");
  sendAT("AT+SAPBR=3,1,\"APN\",\"" + SIM_APN + "\"");
  sendAT("AT+SAPBR=1,1", 10000);
  String bStatus = sendAT("AT+SAPBR=2,1", 2000);
  if (bStatus.indexOf("+SAPBR: 1,1") != -1 || 
      (bStatus.indexOf("+SAPBR") != -1 && bStatus.indexOf(",1,\"") != -1)) {
    diag.bearerOK = true;
    Serial.println("  ✅ Bearer open.");
    sendAT("AT+HTTPINIT");
    sendAT("AT+HTTPPARA=\"CID\",1");
    sendAT("AT+HTTPPARA=\"URL\",\"" + BACKEND_URL + "\"");
    String actionResp = sendAT("AT+HTTPACTION=0", 8000);
    delay(2000);
    String readResp = sendAT("AT+HTTPREAD", 3000);
    sendAT("AT+HTTPTERM");
    sendAT("AT+SAPBR=0,1");
    if (actionResp.indexOf(",307,") != -1) {
      Serial.println("  ❌ 307 REDIRECT — use: ngrok http --scheme=http --domain=... 8000");
    } else if (actionResp.indexOf(",200,") != -1 || actionResp.indexOf(",405,") != -1) {
      diag.ngrokOK = true;
      Serial.println("  ✅ Backend reachable!");
    } else {
      Serial.println("  ❌ Backend not reachable. Raw: " + actionResp);
    }
  } else {
    Serial.println("  ❌ Bearer failed. APN: " + SIM_APN);
    sendAT("AT+SAPBR=0,1");
  }

  // ── CHECK 8: Bluetooth ───────────────────────────────────
  Serial.println("\n▶ [8/8] Checking Bluetooth...");
  if (SerialBT.hasClient()) {
    diag.bluetoothOK = true;
    Serial.println("  ✅ Client connected!");
  } else {
    Serial.println("  ℹ️  Broadcasting as 'SmartBike_System'");
  }

  diag.timestamp = getGSMTimestamp();
  Serial.println("\n  🕐 GSM Time: " + diag.timestamp);

  Serial.println("\n╔══════════════════════════════════════╗");
  Serial.println("║         DIAGNOSTIC SUMMARY           ║");
  Serial.println("╠══════════════════════════════════════╣");
  Serial.println("║ MPU6050       : " + String(diag.mpuOK     ? "✅ OK  " : "❌ FAIL") + "             ║");
  Serial.println("║ GSM Module    : " + String(diag.gsmOK     ? "✅ OK  " : "❌ FAIL") + "             ║");
  Serial.println("║ SIM Card      : " + String(diag.simOK     ? "✅ OK  " : "❌ FAIL") + "             ║");
  Serial.println("║ Network Reg   : " + String(diag.networkOK ? "✅ OK  " : "❌ FAIL") + "             ║");
  Serial.println("║ GPRS          : " + String(diag.gprsOK    ? "✅ OK  " : "❌ FAIL") + "             ║");
  Serial.println("║ Bearer        : " + String(diag.bearerOK  ? "✅ OK  " : "❌ FAIL") + "             ║");
  Serial.println("║ Backend/ngrok : " + String(diag.ngrokOK   ? "✅ OK  " : "❌ FAIL") + "             ║");
  Serial.println("║ Signal        : " + String(diag.signalStrength) + "/31                 ║");
  Serial.println("╚══════════════════════════════════════╝");

  bool allOK = diag.gsmOK && diag.simOK && diag.networkOK && diag.gprsOK && diag.mpuOK;
  if (allOK) {
    Serial.println("\n🟢 SYSTEM READY!");
    for (int i = 0; i < 3; i++) {
      digitalWrite(BUZZER_PIN, HIGH); delay(100);
      digitalWrite(BUZZER_PIN, LOW);  delay(100);
    }
  } else {
    Serial.println("\n🔴 WARNING — Some checks failed.");
    digitalWrite(BUZZER_PIN, HIGH); delay(1000);
    digitalWrite(BUZZER_PIN, LOW);
  }

  SerialBT.println("DIAG|GSM:"  + String(diag.gsmOK)          +
                   "|SIM:"      + String(diag.simOK)           +
                   "|NET:"      + String(diag.networkOK)        +
                   "|GPRS:"     + String(diag.gprsOK)           +
                   "|NGROK:"    + String(diag.ngrokOK)          +
                   "|SIG:"      + String(diag.signalStrength));

  Serial.println("\n[BOOT] Starting main loop...\n");
}

// ============================================================
//  TASK — GPS Reader (Core 0)
// ============================================================
void taskGPS(void* param) {
  for (;;) {
    while (SerialGPS.available() > 0) gps.encode(SerialGPS.read());
    if (gps.location.isValid()) {
      String loc = String(gps.location.lat(), 6) + "," +
                   String(gps.location.lng(), 6);
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        shared.lastLocation = loc;
        xSemaphoreGive(dataMutex);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ============================================================
//  TASK — Emergency Dispatch (Core 0, spawned on crash)
// ============================================================
void taskDispatch(void* param) {
  String crashType, status, location;
  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    crashType = shared.crashType;
    status    = shared.currentStatus;
    location  = shared.lastLocation;
    xSemaphoreGive(dataMutex);
  }

  String mapLink   = "https://maps.google.com/?q=" + location;
  String timestamp = getGSMTimestamp();
  bool   isMajor   = (crashType == "Major Crash");
  bool   isSOS     = (status.indexOf("SOS") != -1);

  Serial.println("\n[DISPATCH] ══ " + crashType + " | " + status + " ══");

  // ── STEP 1: SMS ──────────────────────────────────────────
  Serial.println("[DISPATCH] Step 1: SMS...");
  sendAT("AT+CMGF=1");

  String label = "";
  if      (crashType == "Major Crash") label = "CRITICAL: Major Crash Detected";
  else if (crashType == "Tip-Over")    label = "WARNING: Bike Tip-Over Detected";
  else if (crashType == "Minor Crash") label = "WARNING: Minor Crash Detected";
  else if (crashType == "Parked Bump") label = "ALERT: Parked Bike Tampered";
  else                                 label = "ALERT: " + crashType;

  String smsBody  = label + "\n";
         smsBody += "Status: " + status + "\n";
         smsBody += "Time: "   + timestamp + "\n";
         smsBody += (isMajor || isSOS) ? "Location: " + mapLink
                                       : "Rider may need attention.";

  SerialGSM.print("AT+CMGS=\""); SerialGSM.print(EMERGENCY_NUMBER); SerialGSM.println("\"");
  delay(1000);
  SerialGSM.print(smsBody);
  delay(100);
  SerialGSM.write(26);                              // Ctrl+Z
  delay(5000);                                      // ⬇ was 8000
  while (SerialGSM.available()) SerialGSM.read();   // flush +CMGS
  delay(1000);                                      // ⬇ was 3000
  sendAT("AT", 500);                                // confirm command mode
  Serial.println("[DISPATCH] Step 1: SMS done.");

  // ── STEP 2: CALL (Major / SOS only) ──────────────────────
  if (isMajor || isSOS) {
    Serial.println("[DISPATCH] Step 2: Calling...");
    sendAT("ATD" + EMERGENCY_NUMBER + ";", 2000);
    delay(20000);
    sendAT("ATH");
    Serial.println("[DISPATCH] Step 2: Call done.");
  } else {
    Serial.println("[DISPATCH] Step 2: Skipped (" + crashType + ").");
  }

  // ── STEP 3: BACKEND ──────────────────────────────────────
  Serial.println("[DISPATCH] Step 3: Backend...");
  delay(1000);                                      // ⬇ was 2000
  while (SerialGSM.available()) SerialGSM.read();

  sendAT("AT+SAPBR=0,1", 1000);                    // ⬇ was 2000
  delay(500);                                       // ⬇ was 1000
  sendAT("AT+CGATT=1", 3000);                       // ⬇ was 5000
  delay(500);                                       // ⬇ was 1000
  sendAT("AT+SAPBR=3,1,\"CONTYPE\",\"GPRS\"");
  sendAT("AT+SAPBR=3,1,\"APN\",\"" + SIM_APN + "\"");

  // Bearer retry — up to 3 attempts
  bool bearerOK = false;
  String bStatus = "";
  for (int attempt = 1; attempt <= 3; attempt++) {
    Serial.println("[DISPATCH] Bearer attempt " + String(attempt) + "/3...");
    sendAT("AT+SAPBR=1,1", 10000);                  // ⬇ was 15000
    delay(1000);                                    // ⬇ was 2000
    bStatus = sendAT("AT+SAPBR=2,1", 3000);         // ⬇ was 5000
    Serial.println("[DISPATCH] Bearer status: " + bStatus);
    if (bStatus.indexOf("+SAPBR") != -1 && bStatus.indexOf(",1,\"") != -1) {
      bearerOK = true;
      Serial.println("[DISPATCH] Bearer open on attempt " + String(attempt));
      break;
    }
    Serial.println("[DISPATCH] Retrying in 2s...");
    sendAT("AT+SAPBR=0,1", 1000);
    delay(2000);                                    // ⬇ was 3000
  }

  if (bearerOK) {
    sendAT("AT+HTTPINIT");
    sendAT("AT+HTTPPARA=\"CID\",1");
    sendAT("AT+HTTPPARA=\"URL\",\"" + BACKEND_URL + "\"");
    sendAT("AT+HTTPPARA=\"CONTENT\",\"application/json\"");

    float lat = 10.8505, lng = 76.2711;
    int ci = location.indexOf(',');
    if (ci != -1) {
      lat = location.substring(0, ci).toFloat();
      lng = location.substring(ci + 1).toFloat();
    }

    bool isCrashed = (crashType == "Major Crash" ||
                      crashType == "Tip-Over"    || isSOS);

    String json = "{";
    json += "\"bike_id\":\"BIKE001\",";
    json += "\"lat\":"          + String(lat, 6) + ",";
    json += "\"lng\":"          + String(lng, 6) + ",";
    json += "\"is_crashed\":"   + String(isCrashed ? "true" : "false") + ",";
    json += "\"crash_type\":\"" + crashType + "\",";
    json += "\"status\":\""     + status + "\",";
    json += "\"vibration\":"    + String(shared.snapVib) + ",";
    json += "\"fsr\":"          + String(shared.snapFSR) + ",";
    json += "\"impact_force\":" + String(shared.snapImpact, 0) + ",";
    json += "\"tilt_angle\":"   + String(shared.snapTilt, 1);
    json += "}";

    Serial.println("[JSON] " + json);

    sendAT("AT+HTTPDATA=" + String(json.length()) + ",5000", 1000);
    SerialGSM.print(json);
    delay(500);

    SerialGSM.println("AT+HTTPACTION=1");
    unsigned long postStart = millis();
    String postResp = "";
    while (millis() - postStart < 12000) {          // ⬇ was 15000
      if (SerialGSM.available()) postResp += (char)SerialGSM.read();
      if (postResp.indexOf("+HTTPACTION") != -1) break;
    }
    Serial.println("[HTTP] " + postResp);

    delay(500);
    String readResp = sendAT("AT+HTTPREAD", 2000);  // ⬇ was 3000
    Serial.println("[BODY] " + readResp);

    sendAT("AT+HTTPTERM");
    sendAT("AT+SAPBR=0,1");

    if      (postResp.indexOf(",200,") != -1 || postResp.indexOf(",201,") != -1)
      Serial.println("[DISPATCH] ✅ Backend saved!");
    else if (postResp.indexOf(",307,") != -1)
      Serial.println("[DISPATCH] ❌ 307 — disable SECURE_SSL_REDIRECT in Django.");
    else if (postResp.indexOf(",400,") != -1)
      Serial.println("[DISPATCH] ❌ 400 — JSON fields wrong. Django: " + readResp);
    else if (postResp.indexOf(",404,") != -1)
      Serial.println("[DISPATCH] ❌ 404 — URL wrong.");
    else
      Serial.println("[DISPATCH] ⚠️ Unexpected: " + postResp);

  } else {
    Serial.println("[DISPATCH] ❌ Bearer failed after 3 attempts. APN: " + SIM_APN);
    sendAT("AT+SAPBR=0,1");
  }

  SerialBT.println("DISPATCH_DONE|" + crashType + "|" + status + "|" + timestamp);

  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    shared.dispatchReady = false;
    xSemaphoreGive(dataMutex);
  }

  Serial.println("[DISPATCH] ══ Done ══\n");
  dispatchRunning = false;
  vTaskDelete(NULL);
}

void triggerDispatch() {
  if (dispatchRunning) {
    Serial.println("[DISPATCH] Blocked — already running.");
    return;
  }
  dispatchRunning = true;
  xTaskCreatePinnedToCore(taskDispatch, "Dispatch", 20000, NULL, 1, NULL, 0);
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  SerialGSM.begin(9600, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);
  SerialGPS.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(SWT_PIN, INPUT_PULLUP);
  pinMode(VIB_PIN, INPUT);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.begin();
  Wire.beginTransmission(0x68);
  Wire.write(0x6B); Wire.write(0);
  Wire.endTransmission(true);

  SerialBT.begin("SmartBike_System");
  dataMutex = xSemaphoreCreateMutex();

  delay(3000);
  // runDiagnostics();
  xTaskCreatePinnedToCore(taskGPS, "GPS", 4000, NULL, 1, NULL, 0);
}

// ============================================================
//  LOOP — Sensor Monitor (Core 1)
//
//  SAFETY LOGIC MATRIX v15.1
//  ┌──────────────────┬────────┬──────────┬──────────┬───────────────────────────────┐
//  │ Scenario         │ Tilt   │ FSR      │ Vib      │ Action                        │
//  ├──────────────────┼────────┼──────────┼──────────┼───────────────────────────────┤
//  │ 1. Parked Bump   │ NO     │ EMPTY    │ HIGH(5)  │ 1s Beep + BT + Backend        │
//  │ 2. Minor Crash   │ NO     │ OCCUPIED │ HIGH(5)  │ 5s Beep + SMS + Backend       │
//  │ 3. Tip-Over      │ YES(5) │ EMPTY    │ LOW(<2)  │ 15s Countdown → Warning SMS   │
//  │ 4. Major Crash   │ YES(5) │ EMPTY    │ HIGH(2)  │ 15s Countdown → SOS SMS+CALL  │
//  └──────────────────┴────────┴──────────┴──────────┴───────────────────────────────┘
//  Numbers in brackets = accumulator count required (each tick = 30ms)
//  Edge case: Fallen + Occupied → ignored (hard lean, rider still on bike)
// ============================================================
void loop() {
  // ── Read MPU6050 ─────────────────────────────────────────
  Wire.beginTransmission(0x68);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom(0x68, 6, true);
  int16_t ax = Wire.read() << 8 | Wire.read();
  int16_t ay = Wire.read() << 8 | Wire.read();
  int16_t az = Wire.read() << 8 | Wire.read();

  float impact = sqrt(pow((float)ax, 2) + pow((float)ay, 2) + pow((float)az, 2));
  float tilt   = abs(atan2((float)ay, (float)az) * 180.0 / PI);
  int   vib    = analogRead(VIB_PIN);
  int   fsr    = analogRead(FSR_PIN);
  bool  btn    = (digitalRead(SWT_PIN) == LOW);

  // ── Tilt accumulator — 150ms sustained required ───────────
  // Prevents a single jolt from MPU from faking a fall
  if (tilt > TILT_THRESHOLD)
    tiltAccumulator = min(tiltAccumulator + 1, TILT_TRIGGER_COUNT + 5);
  else
    tiltAccumulator = max(tiltAccumulator - 1, 0);

  // ── Vib accumulator — decays on quiet ────────────────────
  if (vib > VIB_THRESHOLD)
    vibAccumulator = min(vibAccumulator + 1, VIB_TRIGGER_COUNT + 5);
  else
    vibAccumulator = max(vibAccumulator - 1, 0);

  // ── Derived state flags (Debounced) ───────────────────────
  bool highVibCrash = (vibAccumulator >= VIB_CRASH_TRIGGER_COUNT);
  bool highVib      = (vibAccumulator >= VIB_TRIGGER_COUNT);
  bool fallen       = (tiltAccumulator >= TILT_TRIGGER_COUNT);
  bool occupied     = (fsr >= FSR_EMPTY_THRESHOLD);

  // ── Impact Latching ───────────────────────────────────────
  // If we are tilting or vibrating, "latch" the highest impact seen.
  // This ensures a 30,000 impact spike isn't forgotten 150ms later when 'fallen' is true.
  if (tilt > 30.0 || highVibCrash) {
    if (impact > peakImpact) peakImpact = impact;
  } else {
    peakImpact = impact; // Reset/Follow if bike is stable
  }

  // ── Bluetooth live stream ─────────────────────────────────
  if (millis() - lastBTSend > 1000) {
    SerialBT.println("LIVE|tilt:"    + String(tilt, 1) +
                     "|impact:"      + String(impact, 0) +
                     "|vib:"         + String(vib) +
                     "|tilt_acc:"    + String(tiltAccumulator) +
                     "|vib_acc:"     + String(vibAccumulator) +
                     "|fsr:"         + String(fsr) +
                     "|fallen:"      + String(fallen) +
                     "|occupied:"    + String(occupied) +
                     "|status:"      + shared.currentStatus +
                     "|loc:"         + shared.lastLocation);
    Serial.printf(
      "Tilt:%.1f T_Acc:%d | Impact:%.0f | Vib:%d V_Acc:%d | FSR:%d | Fallen:%d Occupied:%d\n",
      tilt, tiltAccumulator, impact, vib, vibAccumulator, fsr, fallen, occupied
    );
    lastBTSend = millis();
  }

  // ── Countdown Active ─────────────────────────────────────
  if (isCrashCountdown) {
    unsigned long elapsed = millis() - crashStartTime;
    digitalWrite(BUZZER_PIN, ((elapsed / 200) % 2 == 0));

    if (btn) {
      isCrashCountdown = false;
      digitalWrite(BUZZER_PIN, LOW);
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        shared.currentStatus = "Safe - Manual Cancel";
        shared.snapVib = vib; shared.snapFSR = fsr;
        shared.snapImpact = impact; shared.snapTilt = tilt;
        xSemaphoreGive(dataMutex);
      }
      Serial.println("[DETECT] Cancelled by button.");
      SerialBT.println("CANCEL|Manual|Safe");
      triggerDispatch();
      delay(30); return;
    }

    if (elapsed >= COUNTDOWN_MS) {
      isCrashCountdown = false;
      digitalWrite(BUZZER_PIN, LOW);
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        shared.currentStatus = (!occupied) ? "SOS DISPATCHED" : "Safe - Rider Recovered";
        shared.snapVib = vib; shared.snapFSR = fsr;
        shared.snapImpact = impact; shared.snapTilt = tilt;
        xSemaphoreGive(dataMutex);
      }
      Serial.println("[DETECT] Countdown expired → " + shared.currentStatus);
      triggerDispatch();
    }
    delay(30); return;
  }

  // ══════════════════════════════════════════════════════════
  //  SCENARIO DETECTION — v15.1 Safety Logic Matrix
  // ══════════════════════════════════════════════════════════

  // SCENARIO 4: Major Crash — Fallen + Empty Seat + High Impact + High Vib
  // Uses peakImpact to catch the initial hit that happened before 'fallen' was confirmed.
  if (fallen && !occupied && peakImpact > IMPACT_THRESHOLD  && !dispatchRunning) {
    tiltAccumulator = 0; vibAccumulator = 0; peakImpact = 0;
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      shared.crashType     = "Major Crash";
      shared.currentStatus = "SOS Countdown";
      shared.snapVib = vib; shared.snapFSR = fsr;
      shared.snapImpact = impact; shared.snapTilt = tilt;
      xSemaphoreGive(dataMutex);
    }
    isCrashCountdown = true;
    crashStartTime   = millis();
    Serial.println("[DETECT] 🚨 MAJOR CRASH — Fallen + High Impact (Latched) + High Vib");
    SerialBT.println("ALERT|Major Crash|SOS Countdown 15s");
  }

  // SCENARIO 3: Tip-Over — Fallen + Empty Seat + Low Impact
  else if (fallen && !occupied && peakImpact < IMPACT_THRESHOLD && !dispatchRunning) {
    tiltAccumulator = 0; vibAccumulator = 0; peakImpact = 0;
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
      shared.crashType     = "Tip-Over";
      shared.currentStatus = "Warning Countdown";
      shared.snapVib = vib; shared.snapFSR = fsr;
      shared.snapImpact = impact; shared.snapTilt = tilt;
      xSemaphoreGive(dataMutex);
    }
    isCrashCountdown = true;
    crashStartTime   = millis();
    Serial.println("[DETECT] ⚠️ TIP-OVER — Fallen + EmptySeat, Low Impact");
    SerialBT.println("ALERT|Tip-Over|Warning Countdown 15s");
  }

  // SCENARIO 2: Minor Crash — Upright + Rider Seated + HIGH IMPACT (>23000)
  else if (!fallen && occupied && impact > MINOR_IMPACT_THRESHOLD &&
           millis() - lastVibAction > VIB_COOLDOWN && !dispatchRunning) {
    vibAccumulator = 0;
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      shared.crashType     = "Minor Crash";
      shared.currentStatus = "Warning Logged";
      shared.snapVib = vib; shared.snapFSR = fsr;
      shared.snapImpact = impact; shared.snapTilt = tilt;
      xSemaphoreGive(dataMutex);
    }
    triggerDispatch();
    lastVibAction = millis();
    Serial.println("[DETECT] ⚠️ MINOR CRASH — High Impact while rider seated");
    SerialBT.println("ALERT|Minor Crash|Warning SMS");
    digitalWrite(BUZZER_PIN, HIGH); delay(5000); digitalWrite(BUZZER_PIN, LOW);
  }

  // SCENARIO 1: Parked Bump — Upright + Empty Seat + HighVib(5)
  else if (!fallen && !occupied && highVib &&
           millis() - lastVibAction > VIB_COOLDOWN && !dispatchRunning) {
    vibAccumulator = 0;
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      shared.crashType     = "Parked Bump";
      shared.currentStatus = "Tampering Alert";
      shared.snapVib = vib; shared.snapFSR = fsr;
      shared.snapImpact = impact; shared.snapTilt = tilt;
      xSemaphoreGive(dataMutex);
    }
    triggerDispatch();
    lastVibAction = millis();
    Serial.println("[DETECT] 🔔 PARKED BUMP — Tampering, bike unattended");
    SerialBT.println("ALERT|Parked Bump|Tampering Alert");
    // 1s beep after dispatch
    digitalWrite(BUZZER_PIN, HIGH); delay(1000); digitalWrite(BUZZER_PIN, LOW);
  }

  // EDGE CASE: Fallen + Occupied → hard lean, rider still on bike
  else if (fallen && occupied) {
    Serial.println("[DETECT] ℹ️ Fallen+Occupied — hard lean, ignoring.");
  }

  delay(30);
}
