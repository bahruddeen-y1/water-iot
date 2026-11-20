// ============================================================
//  config.h — ตั้งค่า WiFi และ Firebase
//  แก้ไขไฟล์นี้ให้ตรงกับข้อมูลของคุณ
//  *** ห้ามแชร์ไฟล์นี้บน GitHub หรือที่สาธารณะ ***
// ============================================================

#ifndef CONFIG_H
#define CONFIG_H

// ============================================================
//  WiFi
// ============================================================
#define WIFI_COUNT 1

const char* WIFI_SSID_LIST[WIFI_COUNT] = {
  "Steve"
};

const char* WIFI_PASS_LIST[WIFI_COUNT] = {
  "________"
};

// ============================================================
//  Firebase Realtime Database
//  วิธีหาค่า:
//    HOST → Firebase Console > Project > Realtime Database > URL
//           (ไม่ต้องใส่ https:// และ ลบ / ท้ายออก)
//    AUTH → Firebase Console > Project Settings > Service Accounts
//           > Database secrets (Legacy token)
//
//  ⚠️ สำคัญ: ต้องเลือก HOST ให้ตรงกับ region ที่สร้าง database
//     ถ้าตอนสร้าง database เลือก asia-southeast1 (Singapore)
//     ให้คอมเมนต์บรรทัด firebaseio.com แล้วใช้บรรทัดล่างแทน
// ============================================================
// เลือกใช้บรรทัดที่ตรงกับ region ของ database (เช็คใน Firebase Console → Realtime Database → ดู URL)
#define FIREBASE_HOST   ""
// #define FIREBASE_HOST "realtimedbproject-29431-default-rtdb.asia-southeast1.firebasedatabase.app"

#define FIREBASE_AUTH   ""

// ============================================================
//  Discord Webhook
//  วิธีหา URL:
//    Discord → คลิกขวาที่ channel → Edit Channel
//    → Integrations → Webhooks → New Webhook → Copy Webhook URL
//  ESP32 จะส่งแจ้งเตือนเมื่อ:
//    - ระบบเริ่มทำงาน (startup)
//    - EC > 2000 µS/cm
//    - อุณหภูมิ > 30°C
//    (cooldown 5 นาที/ประเภท กันสแปม)
// ============================================================
#define DISCORD_WEBHOOK_URL ""

// ============================================================
//  การ Calibrate เซนเซอร์ EC
//  ขั้นตอน Calibrate:
//    1. จุ่มเซนเซอร์ใน solution ที่รู้ค่า EC แน่นอน (เช่น 1413 µS/cm)
//    2. อ่านค่า ADC ที่ได้จาก Serial Monitor
//    3. คำนวณ: EC_CALIBRATION_FACTOR = ค่า EC จริง / ค่าแรงดัน (V)
//    ตัวอย่าง: 1413 / 0.9V = 1570 (ค่าตัวอย่าง ต้องวัดจริง)
// ============================================================
#define EC_CALIBRATION_FACTOR  1500.0   // µS/cm per Volt — แก้หลัง Calibrate

#endif // CONFIG_H
