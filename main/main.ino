
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <FirebaseESP32.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <time.h>

// ---- ไฟล์ config แยกต่างหาก (ดู config.h) ----
#include "config.h"

// ============================================================
//  พิน GPIO
// ============================================================
#define PIN_TEMP_SENSOR   4     // DS18B20 → GPIO4 (ต้องมีตัวต้านทาน 4.7kΩ pull-up)
#define PIN_EC_SENSOR     34    // EC Sensor อนาล็อก → GPIO34 (ADC1)
#define PIN_PUMP_PURE     26    // Relay ปั๊มน้ำบริสุทธิ์ → GPIO26
#define PIN_PUMP_COOL     27    // Relay ปั๊มน้ำหล่อเย็น  → GPIO27
#define PIN_STATUS_LED    2     // LED สถานะบนบอร์ด (built-in)

// ============================================================
//  ค่า threshold — โหลดจาก Firebase /settings (ค่าเริ่มต้นใช้ถ้ายังไม่มีในฐานข้อมูล)
// ============================================================
float EC_MAX_THRESHOLD      = 2000.0;
float EC_NORMAL_THRESHOLD   = 1500.0;
float TEMP_MAX_THRESHOLD    = 30.0;
float TEMP_NORMAL_THRESHOLD = 28.0;

// ============================================================
//  ระยะเวลา (มิลลิวินาที)
// ============================================================
#define INTERVAL_READ_SENSOR   5000    // อ่านค่าเซนเซอร์ทุก 5 วินาที
#define INTERVAL_SEND_FIREBASE 10000   // ส่งข้อมูลขึ้น Firebase ทุก 10 วินาที
#define INTERVAL_BLINK_NORMAL  2000    // LED กะพริบช้า = ทำงานปกติ
#define INTERVAL_BLINK_WARN    300     // LED กะพริบเร็ว = มีปัญหา
#define INTERVAL_DISCORD_NOTIFY 300000  // ส่ง Discord ซ้ำได้ทุก 5 นาที (กัน spam)

// ============================================================
//  ออบเจกต์ฮาร์ดแวร์
// ============================================================
OneWire           oneWire(PIN_TEMP_SENSOR);
DallasTemperature tempSensor(&oneWire);

FirebaseData      fbData;
FirebaseAuth      fbAuth;
FirebaseConfig    fbConfig;

// ============================================================
//  ตัวแปรสถานะ
// ============================================================
float   currentEC           = 0.0;
float   currentTemp         = 0.0;
bool    pumpPureState       = false;   // true = เปิด
bool    pumpCoolState       = false;
bool    manualModeActive    = false;   // true = ผู้ใช้ควบคุมเอง
bool    wifiConnected       = false;

unsigned long lastReadTime       = 0;
unsigned long lastSendTime       = 0;
unsigned long lastBlinkTime      = 0;
unsigned long lastWifiCheck      = 0;
unsigned long lastNtpCheck       = 0;
unsigned long lastSettingsLoad   = 0;
unsigned long wifiDownSince      = 0;
bool          ledState           = false;
bool          ntpSynced          = false;
bool          ecAlertActive      = false;
bool          tempAlertActive    = false;

// Discord — cooldown (ตั้งให้แจ้งทันทีรอบแรกโดยไม่ต้องรอ 5 นาที)
unsigned long lastDiscordEC      = (unsigned long)(-INTERVAL_DISCORD_NOTIFY);
unsigned long lastDiscordTemp    = (unsigned long)(-INTERVAL_DISCORD_NOTIFY);
unsigned long lastDiscordEcOk    = 0;   // กัน spam alert "กลับสู่ปกติ"
unsigned long lastDiscordTempOk  = 0;

#define INTERVAL_DISCORD_OK   60000     // กลับสู่ปกติ — ส่งซ้ำได้ทุก 1 นาที
#define INTERVAL_WIFI_CHECK   10000     // ตรวจ WiFi ทุก 10 วิ
#define INTERVAL_NTP_RETRY    60000     // ลอง NTP sync ใหม่ทุก 1 นาที (ถ้ายังไม่ sync)
#define WIFI_DOWN_TIMEOUT     30000     // WiFi หลุด 30 วิ → reconnect

// ============================================================
//  โปรโตไทป์ฟังก์ชัน
// ============================================================
void connectWiFi();
void connectFirebase();
void readSensors();
float readEC();
float readTemperature();
void autoControl();
void setPump(int pin, bool &state, bool turnOn, const char* name);
void sendToFirebase();
void checkManualCommand();
void blinkLED();
String getTimestamp();
void sendDiscord(String message);
void loadSettings();
void wifiWatchdog(unsigned long now);
void ntpRetryIfNeeded(unsigned long now);
bool trySyncNtp();

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n========================================");
  Serial.println("  ระบบตรวจสอบคุณภาพน้ำ v1.0");
  Serial.println("  ESP32 Water Quality Monitor");
  Serial.println("========================================\n");

  // ตั้งค่าพิน
  pinMode(PIN_PUMP_PURE, OUTPUT);
  pinMode(PIN_PUMP_COOL,  OUTPUT);
  pinMode(PIN_STATUS_LED, OUTPUT);
  digitalWrite(PIN_PUMP_PURE, LOW);  // relay active-LOW → LOW = ปิดปั๊ม
  digitalWrite(PIN_PUMP_COOL,  LOW);
  digitalWrite(PIN_STATUS_LED, LOW);

  // เริ่มเซนเซอร์อุณหภูมิ
  tempSensor.begin();
  Serial.println("[SENSOR] DS18B20 เริ่มต้นแล้ว");

  // เชื่อมต่อ WiFi
  connectWiFi();

  // ตั้งเวลาจาก NTP (มี server fallback 2 ตัว)
  configTime(25200, 0, "pool.ntp.org", "time.google.com");
  Serial.print("[TIME] กำลังซิงค์เวลา NTP");
  for (int i = 0; i < 20 && !ntpSynced; i++) {
    if (trySyncNtp()) break;
    delay(500);
    Serial.print(".");
  }
  if (!ntpSynced)
    Serial.println("\n[TIME] ซิงค์ไม่สำเร็จ — loop จะ retry ทุก 1 นาที");

  // เชื่อมต่อ Firebase
  connectFirebase();

  // โหลดค่า threshold จาก Firebase + ตั้ง timer กันโหลดซ้ำในรอบแรก
  if (Firebase.ready()) {
    loadSettings();
    lastSettingsLoad = millis();
  }

  Serial.println("\n[READY] ระบบพร้อมทำงานแล้ว\n");

  // แจ้งเตือน Discord ว่าระบบเริ่มทำงานแล้ว
  if (wifiConnected) {
    String startMsg = "✅ ระบบตรวจสอบคุณภาพน้ำเริ่มทำงานแล้ว\n";
    startMsg += "📡 IP: " + WiFi.localIP().toString() + "\n";
    startMsg += "🕐 " + getTimestamp();
    sendDiscord(startMsg);
  }
}

// ============================================================
//  LOOP หลัก
// ============================================================
void loop() {
  unsigned long now = millis();

  // --- WiFi watchdog: ตรวจสถานะทุก 10 วิ + reconnect ถ้าหลุด > 30 วิ ---
  if (now - lastWifiCheck >= INTERVAL_WIFI_CHECK) {
    lastWifiCheck = now;
    wifiWatchdog(now);
  }

  // --- NTP retry: ถ้ายังไม่ sync ลองทุก 1 นาที ---
  if (!ntpSynced && wifiConnected && (now - lastNtpCheck >= INTERVAL_NTP_RETRY)) {
    lastNtpCheck = now;
    ntpRetryIfNeeded(now);
  }

  // --- อ่านค่าเซนเซอร์ทุก INTERVAL_READ_SENSOR ---
  if (now - lastReadTime >= INTERVAL_READ_SENSOR) {
    lastReadTime = now;
    readSensors();

    if (!manualModeActive) {
      autoControl();  // ควบคุมอัตโนมัติตามค่าเซนเซอร์
    }
  }

  // --- ส่งข้อมูลขึ้น Firebase ทุก INTERVAL_SEND_FIREBASE ---
  if (now - lastSendTime >= INTERVAL_SEND_FIREBASE) {
    lastSendTime = now;
    if (Firebase.ready()) {
      checkManualCommand();
      sendToFirebase();
    }
  }

  // --- โหลด settings จาก Firebase ทุก 30 วินาที ---
  if (Firebase.ready() && (now - lastSettingsLoad >= 30000)) {
    lastSettingsLoad = now;
    loadSettings();
  }

  // --- กะพริบ LED แสดงสถานะ ---
  blinkLED();
}

// ============================================================
//  เชื่อมต่อ WiFi (เลือกสัญญาณแรงสุด)
// ============================================================
void connectWiFi() {
  Serial.println("[WiFi] กำลังสแกนเครือข่าย...");
  int found = WiFi.scanNetworks();

  // จับคู่กับ SSID ที่กำหนดใน config.h และเลือกตัวที่สัญญาณดีสุด
  int    bestIndex  = -1;
  int    bestRSSI   = -999;
  String bestPass   = "";

  for (int i = 0; i < found; i++) {
    for (int j = 0; j < WIFI_COUNT; j++) {
      if (WiFi.SSID(i) == WIFI_SSID_LIST[j] && WiFi.RSSI(i) > bestRSSI) {
        bestRSSI  = WiFi.RSSI(i);
        bestIndex = i;
        bestPass  = WIFI_PASS_LIST[j];
        break;
      }
    }
  }

  if (bestIndex == -1) {
    Serial.println("[WiFi] ไม่พบเครือข่ายที่รู้จัก — ทำงานแบบ offline");
    wifiConnected = false;
    return;
  }

  String ssid = WiFi.SSID(bestIndex);
  Serial.printf("[WiFi] เชื่อมต่อกับ '%s' (RSSI: %d dBm)\n", ssid.c_str(), bestRSSI);

  WiFi.begin(ssid.c_str(), bestPass.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.printf("\n[WiFi] เชื่อมต่อสำเร็จ! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    wifiConnected = false;
    Serial.println("\n[WiFi] เชื่อมต่อไม่สำเร็จ — ทำงานแบบ offline");
  }
}

// ============================================================
//  เชื่อมต่อ Firebase
// ============================================================
void connectFirebase() {
  if (!wifiConnected) return;

  fbConfig.host           = FIREBASE_HOST;
  fbConfig.signer.tokens.legacy_token = FIREBASE_AUTH;

  Firebase.begin(&fbConfig, &fbAuth);
  Firebase.reconnectWiFi(true);

  // ทดสอบการเชื่อมต่อ
  if (Firebase.ready()) {
    Serial.println("[Firebase] เชื่อมต่อสำเร็จ!");
    Firebase.setString(fbData, "/system/status", "online");
  } else {
    Serial.printf("[Firebase] เชื่อมต่อไม่สำเร็จ: %s\n", fbData.errorReason().c_str());
  }
}

// ============================================================
//  อ่านค่าเซนเซอร์ทั้งหมด
// ============================================================
void readSensors() {
  currentEC   = readEC();
  currentTemp = readTemperature();

  Serial.printf("[SENSOR] EC: %.1f µS/cm | Temp: %.2f °C\n", currentEC, currentTemp);
}

// ============================================================
//  อ่านค่า EC
//  สูตร: แปลง ADC (0-4095) → แรงดัน → µS/cm
//  ปรับค่า EC_CALIBRATION_FACTOR ตามการ Calibrate จริง
// ============================================================
float readEC() {
  // อ่าน ADC 15 ครั้ง → ตัด 5 ค่าหัวท้าย (trimmed mean) — เสถียรกว่า average ตรง ๆ ที่ noise สูงๆ จะดึงค่าเพี้ยน
  const int SAMPLES = 15;
  const int TRIM    = 3;  // ตัด 3 ค่าต่ำสุด + 3 ค่าสูงสุด → เฉลี่ย 9 ค่ากลาง
  int readings[SAMPLES];
  for (int i = 0; i < SAMPLES; i++) readings[i] = analogRead(PIN_EC_SENSOR);

  // bubble sort ง่าย ๆ (15 ตัวเล็ก ไม่ต้อง qsort)
  for (int i = 0; i < SAMPLES - 1; i++)
    for (int j = i + 1; j < SAMPLES; j++)
      if (readings[j] < readings[i]) { int t = readings[i]; readings[i] = readings[j]; readings[j] = t; }

  long sum = 0;
  for (int i = TRIM; i < SAMPLES - TRIM; i++) sum += readings[i];
  float adcAvg = (float)sum / (SAMPLES - 2 * TRIM);

  float voltage = (adcAvg / 4095.0) * 3.3;
  float ec = voltage * EC_CALIBRATION_FACTOR;
  if (ec < 0) ec = 0;
  return ec;
}

// ============================================================
//  อ่านค่าอุณหภูมิ DS18B20
// ============================================================
float readTemperature() {
  tempSensor.requestTemperatures();
  float temp = tempSensor.getTempCByIndex(0);

  if (temp == DEVICE_DISCONNECTED_C) {
    Serial.println("[WARN] DS18B20 ไม่ตอบสนอง — ตรวจสอบการต่อสาย");
    return -999.0;
  }
  return temp;
}

// ============================================================
//  ควบคุมปั๊มอัตโนมัติตามค่าเซนเซอร์
// ============================================================
void autoControl() {
  unsigned long now = millis();

  // --- ปั๊มน้ำบริสุทธิ์ (ลด EC) ---
  if (currentEC > EC_MAX_THRESHOLD) {
    if (!pumpPureState)
      setPump(PIN_PUMP_PURE, pumpPureState, true, "ปั๊มน้ำบริสุทธิ์");
    // แจ้งทันทีครั้งแรก และทุก 5 นาทีถ้ายังผิดปกติ
    if (wifiConnected && (!ecAlertActive || (now - lastDiscordEC >= INTERVAL_DISCORD_NOTIFY))) {
      ecAlertActive = true;
      lastDiscordEC = now;
      String msg = "⚠️ [แจ้งเตือน] ค่า EC สูงเกินเกณฑ์\n";
      msg += "📊 EC: " + String(currentEC, 1) + " µS/cm";
      msg += " (เกณฑ์สูงสุด " + String(EC_MAX_THRESHOLD, 0) + " µS/cm)\n";
      msg += "🔵 ปั๊มน้ำบริสุทธิ์ทำงานอยู่\n";
      msg += "🕐 " + getTimestamp();
      sendDiscord(msg);
    }
  } else if (currentEC < EC_NORMAL_THRESHOLD && pumpPureState) {
    setPump(PIN_PUMP_PURE, pumpPureState, false, "ปั๊มน้ำบริสุทธิ์");
    ecAlertActive = false;
    if (wifiConnected && (now - lastDiscordEcOk >= INTERVAL_DISCORD_OK)) {
      lastDiscordEcOk = now;
      String msg = "✅ EC กลับสู่ระดับปกติ\n";
      msg += "📊 EC: " + String(currentEC, 1) + " µS/cm\n";
      msg += "🔵 ปิดปั๊มน้ำบริสุทธิ์แล้ว\n";
      msg += "🕐 " + getTimestamp();
      sendDiscord(msg);
    }
  }

  // --- ปั๊มน้ำหล่อเย็น (ลดอุณหภูมิ) ---
  if (currentTemp > TEMP_MAX_THRESHOLD) {
    if (!pumpCoolState)
      setPump(PIN_PUMP_COOL, pumpCoolState, true, "ปั๊มหล่อเย็น");
    // แจ้งทันทีครั้งแรก และทุก 5 นาทีถ้ายังผิดปกติ
    if (wifiConnected && (!tempAlertActive || (now - lastDiscordTemp >= INTERVAL_DISCORD_NOTIFY))) {
      tempAlertActive = true;
      lastDiscordTemp = now;
      String msg = "🌡️ [แจ้งเตือน] อุณหภูมิสูงเกินเกณฑ์\n";
      msg += "🌡️ อุณหภูมิ: " + String(currentTemp, 1) + " °C";
      msg += " (เกณฑ์สูงสุด " + String(TEMP_MAX_THRESHOLD, 1) + " °C)\n";
      msg += "🟠 ปั๊มหล่อเย็นทำงานอยู่\n";
      msg += "🕐 " + getTimestamp();
      sendDiscord(msg);
    }
  } else if (currentTemp < TEMP_NORMAL_THRESHOLD && currentTemp != -999.0 && pumpCoolState) {
    setPump(PIN_PUMP_COOL, pumpCoolState, false, "ปั๊มหล่อเย็น");
    tempAlertActive = false;
    if (wifiConnected && (now - lastDiscordTempOk >= INTERVAL_DISCORD_OK)) {
      lastDiscordTempOk = now;
      String msg = "✅ อุณหภูมิกลับสู่ระดับปกติ\n";
      msg += "🌡️ อุณหภูมิ: " + String(currentTemp, 1) + " °C\n";
      msg += "🟠 ปิดปั๊มหล่อเย็นแล้ว\n";
      msg += "🕐 " + getTimestamp();
      sendDiscord(msg);
    }
  }
}

// ============================================================
//  สั่งเปิด/ปิดปั๊ม
//  Relay ส่วนใหญ่ทำงานแบบ Active-LOW: LOW = เปิด, HIGH = ปิด
// ============================================================
void setPump(int pin, bool &state, bool turnOn, const char* name) {
  state = turnOn;
  digitalWrite(pin, turnOn ? LOW : HIGH);  // Active-LOW relay
  Serial.printf("[PUMP] %s → %s\n", name, turnOn ? "เปิด" : "ปิด");
}

// ============================================================
//  ตรวจสอบคำสั่ง Manual จาก Firebase (ส่งมาจากเว็บ)
// ============================================================
void checkManualCommand() {
  // ตรวจ manual_mode — อ่านสำเร็จเท่านั้นถึงจะอัปเดต (ถ้า network glitch ก็คงค่าเดิม)
  if (Firebase.getBool(fbData, "/control/manual_mode")) {
    manualModeActive = fbData.boolData();
  } else {
    Serial.printf("[Manual] อ่าน manual_mode ไม่ได้: %s\n", fbData.errorReason().c_str());
    return;  // อ่านไม่ได้ — ข้ามเช็คคำสั่งปั๊มไปด้วย กันสับสน
  }

  if (!manualModeActive) return;

  // ปั๊มน้ำบริสุทธิ์: ทำตาม cmd ถ้าต่างจาก state ปัจจุบัน
  // เคลียร์เฉพาะเมื่อ cmd=true (กันเขียน setBool(false) ทุก 10 วิ ทั้งที่ค่าเป็น false อยู่แล้ว)
  if (Firebase.getBool(fbData, "/control/pump_pure/command")) {
    bool cmd = fbData.boolData();
    if (cmd != pumpPureState)
      setPump(PIN_PUMP_PURE, pumpPureState, cmd, "ปั๊มน้ำบริสุทธิ์ [Manual]");
    if (cmd) Firebase.setBool(fbData, "/control/pump_pure/command", false);
  }

  // ปั๊มหล่อเย็น
  if (Firebase.getBool(fbData, "/control/pump_cool/command")) {
    bool cmd = fbData.boolData();
    if (cmd != pumpCoolState)
      setPump(PIN_PUMP_COOL, pumpCoolState, cmd, "ปั๊มหล่อเย็น [Manual]");
    if (cmd) Firebase.setBool(fbData, "/control/pump_cool/command", false);
  }
}

// ============================================================
//  ส่งข้อมูลขึ้น Firebase Realtime Database
// ============================================================
void sendToFirebase() {
  String ts = getTimestamp();

  // ===== 1. ข้อมูล Real-time (อัปเดตทับทุกครั้ง) =====
  FirebaseJson realtimeJson;
  realtimeJson.set("ec",            currentEC);
  realtimeJson.set("temperature",   currentTemp);
  realtimeJson.set("pump_pure",     pumpPureState);
  realtimeJson.set("pump_cool",     pumpCoolState);
  realtimeJson.set("manual_mode",   manualModeActive);
  realtimeJson.set("timestamp",     ts);
  realtimeJson.set("wifi_rssi",     (int)WiFi.RSSI());

  if (Firebase.updateNode(fbData, "/realtime", realtimeJson)) {
    Serial.println("[Firebase] ✓ อัปเดตข้อมูล Real-time สำเร็จ");
  } else {
    Serial.printf("[Firebase] ✗ Real-time ล้มเหลว: %s\n", fbData.errorReason().c_str());
    return;
  }

  // ===== อัปเดต system status ทุกรอบ =====
  FirebaseJson systemJson;
  systemJson.set("status",    "online");
  systemJson.set("last_seen", ts);
  Firebase.updateNode(fbData, "/system", systemJson);

  // ===== 2. ข้อมูลประวัติ (Push ทุกครั้งไม่ทับของเดิม) =====
  // เก็บเฉพาะทุก 1 นาที (ปรับได้ตามต้องการ)
  // ⚠️ ข้ามถ้า NTP ยังไม่ซิงค์ (timestamp = "millis_xxx") — กัน path ที่ dashboard อ่านไม่เจอ
  static unsigned long lastHistoryTime = 0;
  unsigned long now = millis();

  if (now - lastHistoryTime >= 60000) {
    lastHistoryTime = now;

    if (ts.startsWith("millis_")) {
      Serial.println("[Firebase] ข้ามบันทึกประวัติ — NTP ยังไม่ซิงค์");
      return;
    }

    FirebaseJson historyJson;
    historyJson.set("ec",          currentEC);
    historyJson.set("temperature", currentTemp);
    historyJson.set("pump_pure",   pumpPureState);
    historyJson.set("pump_cool",   pumpCoolState);
    historyJson.set("timestamp",   ts);

    String historyPath = "/history/" + ts.substring(0, 10);  // /history/2025-01-15
    if (Firebase.pushJSON(fbData, historyPath, historyJson)) {
      Serial.println("[Firebase] ✓ บันทึกประวัติสำเร็จ");
    } else {
      Serial.printf("[Firebase] ✗ ประวัติล้มเหลว: %s\n", fbData.errorReason().c_str());
    }
  }
}

// ============================================================
//  สร้าง Timestamp รูปแบบ ISO 8601 (UTC+7)
// ============================================================
String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    // ถ้าได้เวลาไม่ได้ ใช้ millis() แทน
    return "millis_" + String(millis());
  }

  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
  return String(buf);
}

// ============================================================
//  ส่งแจ้งเตือน Discord Webhook
//  API: POST <DISCORD_WEBHOOK_URL>
//  Body: JSON { "content": "ข้อความ" }
// ============================================================
void sendDiscord(String message) {
  if (!wifiConnected) {
    Serial.println("[Discord] ข้าม — ไม่มี WiFi");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, DISCORD_WEBHOOK_URL);
  http.addHeader("Content-Type", "application/json");

  // escape อักขระพิเศษเพื่อไม่ให้ JSON พัง
  message.replace("\\", "\\\\");
  message.replace("\"", "\\\"");
  message.replace("\n", "\\n");
  String body = "{\"content\":\"" + message + "\"}";

  int code = http.POST(body);
  if (code == 204 || code == 200) {
    Serial.println("[Discord] ✓ ส่งแจ้งเตือนสำเร็จ");
  } else {
    Serial.printf("[Discord] ✗ ส่งไม่สำเร็จ HTTP %d: %s\n",
                  code, http.getString().c_str());
  }
  http.end();
}

// ============================================================
//  โหลดค่า threshold จาก Firebase /settings
// ============================================================
void loadSettings() {
  float ecMax = EC_MAX_THRESHOLD, ecNormal = EC_NORMAL_THRESHOLD;
  float tMax = TEMP_MAX_THRESHOLD, tNormal = TEMP_NORMAL_THRESHOLD;

  if (Firebase.getFloat(fbData, "/settings/ec_max"))     ecMax    = fbData.floatData();
  if (Firebase.getFloat(fbData, "/settings/ec_normal"))  ecNormal = fbData.floatData();
  if (Firebase.getFloat(fbData, "/settings/temp_max"))   tMax     = fbData.floatData();
  if (Firebase.getFloat(fbData, "/settings/temp_normal")) tNormal = fbData.floatData();

  // Validate ก่อน apply — ถ้าค่าผิด (ec_normal >= ec_max ฯลฯ) คงค่าเดิมไว้ กันระบบเพี้ยน
  if (ecMax > 0 && ecMax <= 20000 && ecNormal >= 0 && ecNormal < ecMax &&
      tMax > 0 && tMax <= 100 && tNormal >= 0 && tNormal < tMax) {
    EC_MAX_THRESHOLD      = ecMax;
    EC_NORMAL_THRESHOLD   = ecNormal;
    TEMP_MAX_THRESHOLD    = tMax;
    TEMP_NORMAL_THRESHOLD = tNormal;
    Serial.printf("[Settings] EC: %.0f / %.0f µS/cm | Temp: %.1f / %.1f °C\n",
      EC_MAX_THRESHOLD, EC_NORMAL_THRESHOLD, TEMP_MAX_THRESHOLD, TEMP_NORMAL_THRESHOLD);
  } else {
    Serial.println("[Settings] ค่าจาก Firebase ผิดเกณฑ์ — ใช้ค่าเดิม");
  }
}

// ============================================================
//  WiFi Watchdog — ถ้าหลุด > 30 วิ → reconnect
// ============================================================
void wifiWatchdog(unsigned long now) {
  bool nowConnected = (WiFi.status() == WL_CONNECTED);
  wifiConnected = nowConnected;

  if (nowConnected) {
    wifiDownSince = 0;
    return;
  }
  if (wifiDownSince == 0) {
    wifiDownSince = now;
    Serial.println("[WiFi] ขาดการเชื่อมต่อ");
    return;
  }
  if (now - wifiDownSince >= WIFI_DOWN_TIMEOUT) {
    Serial.println("[WiFi] ขาดเกิน 30 วิ — กำลัง reconnect...");
    WiFi.disconnect();
    delay(100);
    connectWiFi();
    wifiDownSince = wifiConnected ? 0 : now;
  }
}

// ============================================================
//  NTP — ลอง sync ใหม่ + ถ้าได้แล้ว set ntpSynced
// ============================================================
bool trySyncNtp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100)) return false;
  // ตรวจว่าได้เวลาจริง (ไม่ใช่ 1970)
  if (timeinfo.tm_year < (2024 - 1900)) return false;
  ntpSynced = true;
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
  Serial.printf("\n[TIME] ซิงค์สำเร็จ: %s\n", buf);
  return true;
}

void ntpRetryIfNeeded(unsigned long now) {
  Serial.println("[TIME] กำลังลอง NTP ใหม่...");
  configTime(25200, 0, "pool.ntp.org", "time.google.com");
  delay(200);
  trySyncNtp();
}

// ============================================================
//  LED กะพริบแสดงสถานะ
//  ช้า (2 วิ) = ปกติ | เร็ว (300 มิลลิ) = มีปัญหา/เกินค่า
// ============================================================
void blinkLED() {
  bool hasAlert = (currentEC > EC_MAX_THRESHOLD) || 
                  (currentTemp > TEMP_MAX_THRESHOLD && currentTemp != -999.0);

  unsigned long interval = hasAlert ? INTERVAL_BLINK_WARN : INTERVAL_BLINK_NORMAL;
  unsigned long now      = millis();
  if (now - lastBlinkTime >= interval) {
    lastBlinkTime = now;
    ledState = !ledState;
    digitalWrite(PIN_STATUS_LED, ledState);
  }
}
