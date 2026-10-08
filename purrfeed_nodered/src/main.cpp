/*
 * PurrFeed - Smart Cat Feeder
 * ESP32 + MQTT (broker.emqx.io) + Node-RED
 *
 * Pin aktual:
 *   - Servo MG996R  → GPIO 18
 *   - HX711 DT      → GPIO 4
 *   - HX711 SCK     → GPIO 5
 *   - IR Sensor     → GPIO 33
 *   - LED            → GPIO 13
 */

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include <HX711.h>

// ─── WiFi Config ─────────────────────────────────────────────────
const char* WIFI_SSID     = "Olaf";
const char* WIFI_PASSWORD = "1234567890";

// ─── MQTT Config ─────────────────────────────────────────────────
const char* MQTT_BROKER = "broker.emqx.io";
const int   MQTT_PORT   = 1883;
const char* MQTT_CLIENT = "purrfeed_esp32_001";

const char* TOPIC_BERAT        = "purrfeed/berat";
const char* TOPIC_STATUS       = "purrfeed/status";
const char* TOPIC_LOG          = "purrfeed/log";
const char* TOPIC_FEED         = "purrfeed/feed";
const char* TOPIC_TARGET       = "purrfeed/target";
const char* TOPIC_JADWAL_JAM   = "purrfeed/jadwal/jam";
const char* TOPIC_JADWAL_MENIT = "purrfeed/jadwal/menit";

// ─── Pin Definitions ─────────────────────────────────────────────
#define PIN_SERVO      18
#define PIN_HX711_DT   14
#define PIN_HX711_SCK  12
#define PIN_IR         33
#define PIN_LED        13

// ─── Servo config ────────────────────────────────────────────────
#define SERVO_STOP  90
#define SERVO_RUN    0

// ─── Kalibrasi HX711 ─────────────────────────────────────────────
#define CALIBRATION_FACTOR 420.0f

// ─── Global Objects ──────────────────────────────────────────────
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
Servo        servo;
HX711        loadcell;

// ─── State ───────────────────────────────────────────────────────
float targetBerat    = 50.0f;
bool  sedangMemberi  = false;
bool  forceStop      = false;
bool  irSebelumnya   = false;
bool  loadcellOK     = false;

unsigned long lastPublish = 0;
const unsigned long PUBLISH_INTERVAL = 2000;

// ─── Utilitas ────────────────────────────────────────────────────
void servoStop() { servo.write(SERVO_STOP); }
void servoRun()  { servo.write(SERVO_RUN);  }

// LED nyala = sistem normal/aktif, mati = idle/masalah
void setLED(bool nyala) {
  digitalWrite(PIN_LED, nyala ? HIGH : LOW);
}

// ─── Proses Memberi Makan ────────────────────────────────────────
void berMakan() {
  if (sedangMemberi) {
    Serial.println("[FEED] Sudah dalam proses, diabaikan.");
    return;
  }
  sedangMemberi = true;
  forceStop     = false;
  Serial.println("[FEED] Mulai memberi makan...");
  mqtt.publish(TOPIC_LOG, "Mulai memberi makan");

  setLED(true);
  servoRun();

  unsigned long startTime = millis();
  const unsigned long TIMEOUT_MS = 30000;

  while (true) {
    mqtt.loop();

    if (forceStop) {
      servoStop();
      setLED(false);
      Serial.println("[FEED] Force Stop!");
      mqtt.publish(TOPIC_LOG, "Force Stop");
      break;
    }

    if (millis() - startTime > TIMEOUT_MS) {
      servoStop();
      setLED(false);
      Serial.println("[FEED] Timeout!");
      mqtt.publish(TOPIC_LOG, "Timeout");
      break;
    }

    if (loadcellOK) {
      float berat = loadcell.get_units(5);
      if (berat < 0) berat = 0;
      Serial.printf("[FEED] Berat: %.1f / target: %.1f g\n", berat, targetBerat);
      if (berat >= targetBerat) {
        servoStop();
        setLED(false);
        Serial.println("[FEED] Target tercapai!");
        mqtt.publish(TOPIC_LOG, "Selesai makan");
        break;
      }
    } else {
      Serial.println("[FEED] Loadcell tidak tersedia, tunggu timeout...");
    }

    delay(200);
  }

  sedangMemberi = false;
}

// ─── MQTT Callback ───────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.printf("[MQTT] %s → %s\n", topic, msg.c_str());

  if (strcmp(topic, TOPIC_FEED) == 0) {
    if (msg == "1")      berMakan();
    else if (msg == "0") { forceStop = true; Serial.println("[MQTT] Force Stop diterima."); }
  } else if (strcmp(topic, TOPIC_TARGET) == 0) {
    targetBerat = msg.toFloat();
    Serial.printf("[CONFIG] Target berat: %.1f g\n", targetBerat);
  } else if (strcmp(topic, TOPIC_JADWAL_JAM) == 0) {
    Serial.printf("[CONFIG] Jadwal jam: %s\n", msg.c_str());
  } else if (strcmp(topic, TOPIC_JADWAL_MENIT) == 0) {
    Serial.printf("[CONFIG] Jadwal menit: %s\n", msg.c_str());
  }
}

// ─── Koneksi WiFi ────────────────────────────────────────────────
void connectWiFi() {
  Serial.printf("[WiFi] Menghubungkan ke %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500); Serial.print("."); retry++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Terhubung! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] Gagal terhubung, lanjut tanpa WiFi.");
  }
}

// ─── Koneksi MQTT ────────────────────────────────────────────────
void connectMQTT() {
  int retry = 0;
  while (!mqtt.connected() && retry < 5) {
    Serial.print("[MQTT] Menghubungkan...");
    if (mqtt.connect(MQTT_CLIENT)) {
      Serial.println(" OK!");
      mqtt.subscribe(TOPIC_FEED);
      mqtt.subscribe(TOPIC_TARGET);
      mqtt.subscribe(TOPIC_JADWAL_JAM);
      mqtt.subscribe(TOPIC_JADWAL_MENIT);
    } else {
      Serial.printf(" Gagal (rc=%d), coba lagi...\n", mqtt.state());
      delay(3000);
      retry++;
    }
  }
}

// ─── Setup ───────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== PurrFeed Smart Cat Feeder ===");

  pinMode(PIN_IR,  INPUT_PULLUP); // IR sensor aktif LOW, pullup internal
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  servo.attach(PIN_SERVO);
  servoStop();
  Serial.println("[SERVO] Inisialisasi selesai, posisi stop.");

  Serial.println("[HX711] Inisialisasi...");
  loadcell.begin(PIN_HX711_DT, PIN_HX711_SCK);
  if (loadcell.wait_ready_timeout(3000)) {
    loadcell.set_scale(CALIBRATION_FACTOR);
    loadcell.tare();
    loadcellOK = true;
    Serial.println("[HX711] OK, timbangan di-tare.");
  } else {
    loadcellOK = false;
    Serial.println("[HX711] Tidak ditemukan! Lanjut tanpa loadcell.");
  }

  connectWiFi();
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  connectMQTT();

  setLED(false);
  Serial.println("[SYSTEM] Sistem siap!");
}

// ─── Loop ────────────────────────────────────────────────────────
void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!mqtt.connected())             connectMQTT();
  mqtt.loop();

  unsigned long now = millis();

  if (now - lastPublish >= PUBLISH_INTERVAL) {
    lastPublish = now;

    // Berat
    if (loadcellOK) {
      float berat = loadcell.get_units(5);
      if (berat < 0) berat = 0;
      char buf[16];
      snprintf(buf, sizeof(buf), "%.1f", berat);
      mqtt.publish(TOPIC_BERAT, buf);
      Serial.printf("[SENSOR] Berat: %s g\n", buf);
    } else {
      mqtt.publish(TOPIC_BERAT, "0.0");
    }

    // IR sensor — aktif LOW (LOW = ada objek, HIGH = tidak ada)
    bool irSekarang = digitalRead(PIN_IR) == LOW;
    if (irSekarang) {
      mqtt.publish(TOPIC_STATUS, "Cat Detected");
      Serial.println("[IR] Cat Detected");
    } else {
      mqtt.publish(TOPIC_STATUS, "No Cat Detected");
      Serial.println("[IR] No Cat Detected");
    }
    irSebelumnya = irSekarang;
  }

  delay(50);
}