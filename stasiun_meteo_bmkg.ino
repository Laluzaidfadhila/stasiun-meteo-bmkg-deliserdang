#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <Adafruit_BMP085.h>
#include <time.h>

// Memanggil parameter kredensial dari file lokal config.h
#include "config.h"

// ==========================================
// 1. PARAMETER AMBANG BATAS & BOT TELEGRAM
// ==========================================
const float TEMP_HIGH_THRESHOLD = 35.0;
const float HUM_LOW_THRESHOLD   = 30.0;

bool isTempAlertSent = false;
bool isSensorErrorAlertSent = false;
bool isRainAlertSent = false;

float lastNotifiedRain = -1.0;

long lastUpdateId = 0;
unsigned long lastBotCheckMillis = 0;
unsigned long lastFirebaseMillis = 0;

// ==========================================
// 2. RAIN GAUGE & ANTI-NOISE FILTER (BMKG)
// ==========================================
#define TIPPING_BUCKET_PIN 14 // GPIO14 = Pin D5 NodeMCU
const float mmPerTip = 0.2;

volatile int tipCountTotal = 0;
volatile int tipCount3Hour = 0;
volatile int tipCount24Hour = 0;

volatile unsigned long lastInterruptTime = 0;
volatile bool pendingTip = false;

// Ketinggian elevasi barometer Vaisala Stasiun Deli Serdang (Meter dpl)
const float ELEVASI_STASIUN_METER = 89.5; 
// Offset kalibrasi perbandingan sensor BMP180 terhadap Barometer Vaisala PTB330
const float OFFSET_KALIBRASI_QFE  = -2.38;

void ICACHE_RAM_ATTR countRain() {
  unsigned long currentTime = millis();
  // Filter 1: Jeda minimal 400ms antar ketukan fisik jungkitan
  if (currentTime - lastInterruptTime > 400) {
    pendingTip = true;
    lastInterruptTime = currentTime;
  }
}

// Filter 2: Stabilisasi tegangan 30ms menyaring noise/getaran angin
void processRainValidation() {
  if (pendingTip) {
    delay(30);
    if (digitalRead(TIPPING_BUCKET_PIN) == LOW) {
      noInterrupts();
      tipCountTotal++;
      tipCount3Hour++;
      tipCount24Hour++;
      pendingTip = false;
      interrupts();
    } else {
      pendingTip = false;
    }
  }
}

// ==========================================
// 3. SENSOR SHT31, BMP180, & WAKTU (NTP)
// ==========================================
Adafruit_SHT31 sht31 = Adafruit_SHT31();
Adafruit_BMP085 bmp;

const long gmtOffset_sec = 25200; // GMT+7 WIB
const int daylightOffset_sec = 0;

int lastRecordedHour = -1;
int last3HourRecorded = -1;
bool is24HourRecorded = false;

String currentObsDate = "";
float bmkgMaxSuhu = -999.0;
float bmkgMinSuhu = 999.0;

unsigned long lastNtpSyncMillis = 0;
const unsigned long NTP_SYNC_INTERVAL = 12 * 3600 * 1000UL;

int getWifiQuality(long rssi) {
  if (rssi <= -100) return 0;
  if (rssi >= -50) return 100;
  return 2 * (rssi + 100);
}

// Formula Stull untuk Perhitungan Bola Basah (Tw)
float calculateWetBulb(float t, float rh) {
  if (rh < 0 || rh > 100) return 0.0;
  return t * atan(0.151977 * sqrt(rh + 8.313659)) +
         atan(t + rh) -
         atan(rh - 1.676331) +
         0.00391838 * pow(rh, 1.5) * atan(0.023101 * rh) -
         4.686035;
}

// Rumus Barometrik Standar BMKG: QFE, QFF, & QNH Terkalibrasi
void hitungTekananBMKG(float suhuKering, float &qfe, float &qff, float &qnh) {
  float rawPressure = bmp.readPressure() / 100.0F;
  // 1. QFE (Tekanan Stasiun Terkalibrasi Vaisala)
  qfe = rawPressure + OFFSET_KALIBRASI_QFE;

  // 2. QFF (Meteorologi - Memperhitungkan Suhu Permukaan Riil)
  if (!isnan(suhuKering) && suhuKering > -50.0) {
    float tKelvin = suhuKering + 273.15 + (0.0065 * (ELEVASI_STASIUN_METER / 2.0));
    qff = qfe * exp((0.034163 * ELEVASI_STASIUN_METER) / tKelvin);
  } else {
    qff = qfe * exp((0.034163 * ELEVASI_STASIUN_METER) / 288.15);
  }

  // 3. QNH (Aviation - Standar Atmosfer ICAO 15°C)
  qnh = qfe * pow((1.0 - (0.0065 * ELEVASI_STASIUN_METER / 288.15)), -5.25588);
}

// ==========================================
// 4. REST API FIREBASE ENGINE
// ==========================================
bool sendFirebaseRequest(String path, String jsonPayload, String method, String &responseStr) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(512, 512);

  HTTPClient http;
  String fullURL = FIREBASE_HOST + path + ".json?auth=" + FIREBASE_AUTH;

  if (http.begin(client, fullURL)) {
    http.setTimeout(4000);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Connection", "close");

    int httpCode = (method == "GET") ? http.GET() : http.sendRequest(method.c_str(), jsonPayload);
    bool success = (httpCode > 0 && (httpCode == 200 || httpCode == 204));
    
    if (success) {
      responseStr = http.getString();
    }
    
    http.end();
    client.stop();
    return success;
  }
  return false;
}

bool sendFirebaseRequest(String path, String jsonPayload, String method) {
  String dummy;
  return sendFirebaseRequest(path, jsonPayload, method, dummy);
}

// ==========================================
// 5. ENGINE TELEGRAM BOT
// ==========================================
void sendTelegramMessage(String message) {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(512, 512);

  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/sendMessage";

  if (http.begin(client, url)) {
    http.addHeader("Content-Type", "application/json");
    String payload = "{\"chat_id\":\"" + String(CHAT_ID) + "\",\"text\":\"" + message + "\",\"parse_mode\":\"Markdown\"}";
    http.POST(payload);
    http.end();
  }
  client.stop();
}

void checkTelegramCommands() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  client.setBufferSizes(1024, 1024);

  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/getUpdates";
  
  if (lastUpdateId > 0) {
    url += "?offset=" + String(lastUpdateId + 1) + "&limit=1&timeout=0";
  } else {
    url += "?offset=-1&limit=1&timeout=0";
  }

  if (http.begin(client, url)) {
    http.setTimeout(3000);
    int httpCode = http.GET();
    
    if (httpCode == 200) {
      String response = http.getString();
      
      int updateIdPos = response.lastIndexOf("\"update_id\":");
      if (updateIdPos != -1) {
        int commaPos = response.indexOf(",", updateIdPos);
        int bracePos = response.indexOf("}", updateIdPos);
        int endPos = (commaPos != -1 && commaPos < bracePos) ? commaPos : bracePos;
        
        long newUpdateId = response.substring(updateIdPos + 12, endPos).toInt();

        if (lastUpdateId == 0) {
          lastUpdateId = newUpdateId;
          http.end();
          client.stop();
          return;
        }

        if (newUpdateId > lastUpdateId) {
          lastUpdateId = newUpdateId;

          int textPos = response.lastIndexOf("\"text\":\"");
          if (textPos != -1) {
            int textEnd = response.indexOf("\"", textPos + 8);
            String command = response.substring(textPos + 8, textEnd);
            
            if (command.indexOf("@") != -1) {
              command = command.substring(0, command.indexOf("@"));
            }

            if (command == "/start") {
              String msg = "👋 *Selamat datang di Sangkar Deli Serdang Bot!*\n\n";
              msg += "📌 *Daftar Perintah:*\n";
              msg += "• `/info` - Cek status perangkat & data sensor.\n";
              msg += "• `/reset_hujan` - Nol-kan akumulasi curah hujan & Firebase.\n";
              msg += "• `/reboot` - Restart ESP8266.\n";
              msg += "• `/start` - Bantuan.";
              sendTelegramMessage(msg);
            } 
            else if (command == "/info") {
              float t = sht31.readTemperature();
              float h = sht31.readHumidity();
              float wetBulb = calculateWetBulb(t, h);
              
              float qfe = 0.0, qff = 0.0, qnh = 0.0;
              hitungTekananBMKG(t, qfe, qff, qnh);

              noInterrupts();
              int tip24 = tipCount24Hour;
              interrupts();
              float rain24 = tip24 * mmPerTip;

              long rssi = WiFi.RSSI();
              uint32_t freeRam = ESP.getFreeHeap();

              struct tm timeinfo;
              getLocalTime(&timeinfo);
              char timeStr[25];
              strftime(timeStr, sizeof(timeStr), "%d/%m/%Y, %H.%M.%S", &timeinfo);

              String msg = "🤖 *STATUS STASIUN METEO DELI SERDANG*\n";
              msg += "-----------------------------------------\n";
              msg += "🔴 Status Perangkat: *ONLINE* 🟢\n";
              msg += "📶 Sinyal Wi-Fi: *" + String(rssi) + " dBm* (" + String(getWifiQuality(rssi)) + "%)\n";
              msg += "🧠 RAM Bebas: *" + String(freeRam / 1024.0, 1) + " KB*\n\n";
              msg += "📊 *DATA SENSOR:*\n";
              msg += "🕒 Waktu: `" + String(timeStr) + "`\n";
              msg += "🌡️ Bola Kering: *" + String(t, 1) + " °C*\n";
              msg += "💧 Kelembapan (RH): *" + String(h, 1) + " %*\n";
              msg += "🟢 Bola Basah: *" + String(wetBulb, 1) + " °C*\n";
              msg += "🔵 QFE: *" + String(qfe, 1) + " hPa*\n";
              msg += "🟣 QFF: *" + String(qff, 1) + " hPa*\n";
              msg += "✈️ QNH: *" + String(qnh, 1) + " hPa*\n";
              msg += "☔ Hujan 24H: *" + String(rain24, 2) + " mm*";

              sendTelegramMessage(msg);
            } 
            else if (command == "/reset_hujan") {
              noInterrupts();
              tipCountTotal  = 0;
              tipCount3Hour  = 0;
              tipCount24Hour = 0;
              lastNotifiedRain = 0.0;
              isRainAlertSent  = false;
              interrupts();

              sendFirebaseRequest("cuaca", "{\"rainfall\":0.0,\"tipCount\":0}", "PATCH");[cite: 1]
              sendTelegramMessage("☔ *RESET BERHASIL*\nCurah hujan dan tip count berhasil di-nol-kan di sistem & Firebase.");
            }
            else if (command == "/reboot") {
              sendTelegramMessage("🔄 *PERINGATAN REBOOT*\nPerangkat ESP8266 sedang melakukan restart...");
              delay(1000);
              ESP.restart();
            }
          }
        }
      }
    }
    http.end();
  }
  client.stop();
}

// ==========================================
// 6. SINKRONISASI DATA AWAL & NTP
// ==========================================
void fetchTodayRainFromFirebase() {
  String response;
  if (sendFirebaseRequest("cuaca", "", "GET", response)) {
    if (response != "null" && response.length() > 0) {
      int rainIdx = response.indexOf("\"rainfall\":");
      if (rainIdx != -1) {
        float lastRain = response.substring(rainIdx + 11).toFloat();
        noInterrupts();
        tipCount24Hour = round(lastRain / mmPerTip);
        interrupts();
        
        lastNotifiedRain = lastRain;
        if (lastRain > 0.0) {
          isRainAlertSent = true; 
        }
      }
    }
  }
}

void fetchTodayEkstremFromFirebase(String dateStr) {
  String response;
  if (sendFirebaseRequest("logs_ekstrem_bmkg/" + dateStr, "", "GET", response)) {
    if (response != "null" && response.length() > 0) {
      int maxIdx = response.indexOf("\"maxSuhu\":");
      if (maxIdx != -1) {
        bmkgMaxSuhu = response.substring(maxIdx + 10).toFloat();
      }
      int minIdx = response.indexOf("\"minSuhu\":");
      if (minIdx != -1) {
        bmkgMinSuhu = response.substring(minIdx + 10).toFloat();
      }
    }
  }
}

void syncNtpTime() {
  Serial.print("\n[⏰] Menyinkronkan waktu NTP");
  configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org", "time.nist.gov", "id.pool.ntp.org");
  
  // Menunggu sinkronisasi waktu valid (maksimal 10 detik)
  struct tm timeinfo;
  int retry = 0;
  while (!getLocalTime(&timeinfo) && retry < 20) {
    delay(500);
    Serial.print(".");
    retry++;
  }
  
  if (retry < 20) {
    char timeStr[25];
    strftime(timeStr, sizeof(timeStr), "%d/%m/%Y %H:%M:%S", &timeinfo);
    Serial.println("\n[✔] Waktu Tersinkron: " + String(timeStr) + " WIB");
  } else {
    Serial.println("\n[!] Waktu NTP tertunda, akan disinkronkan kembali di loop...");
  }
  lastNtpSyncMillis = millis();
}

void connectWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.print("Menghubungkan ke Wi-Fi");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int counter = 0;
    while (WiFi.status() != WL_CONNECTED && counter < 20) {
      delay(500);
      Serial.print(".");
      counter++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n[✔] Wi-Fi Terhubung! IP: " + WiFi.localIP().toString());
      syncNtpTime();
    }
  }
}

// ==========================================
// 7. SETUP UTAMA
// ==========================================
void setup() {
  Serial.begin(115200);
  Wire.begin(4, 5); // SDA = Pin D2, SCL = Pin D1

  pinMode(TIPPING_BUCKET_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TIPPING_BUCKET_PIN), countRain, FALLING);

  if (!sht31.begin(0x44)) {
    Serial.println("[!] Sensor SHT31 Tidak Ditemukan!");
  } else {
    Serial.println("[✔] Sensor SHT31 Siap.");
  }

  if (!bmp.begin()) {
    Serial.println("[!] Sensor BMP180 Tidak Ditemukan!");
  } else {
    Serial.println("[✔] Sensor BMP180 Terkalibrasi Siap.");
  }

  connectWiFi();
  fetchTodayRainFromFirebase();
  sendTelegramMessage("🟢 *STASIUN METEO DELI SERDANG*\nPerangkat ESP8266 + BMP180 terkalibrasi berhasil *ONLINE*.");
}

// ==========================================
// 8. LOOP UTAMA
// ==========================================
void loop() {
  yield();
  connectWiFi();
  processRainValidation();

  if (millis() - lastNtpSyncMillis >= NTP_SYNC_INTERVAL) {
    syncNtpTime();
  }

  // 1. Cek Telegram Bot setiap 1.5 detik
  if (millis() - lastBotCheckMillis >= 1500) {
    checkTelegramCommands();
    lastBotCheckMillis = millis();
  }

  // 2. Transmisi ke Firebase setiap 5 detik
  if (millis() - lastFirebaseMillis >= 5000) {
    lastFirebaseMillis = millis();

    noInterrupts();
    int currentTip24 = tipCount24Hour;
    int currentTip3  = tipCount3Hour;
    interrupts();

    float rain24h = currentTip24 * mmPerTip;
    float rain3h  = currentTip3 * mmPerTip;

    float t = sht31.readTemperature();
    float h = sht31.readHumidity();
    float qfe = 0.0, qff = 0.0, qnh = 0.0;
    hitungTekananBMKG(t, qfe, qff, qnh);

    if (isnan(h) || isnan(t) || t < -20 || t > 80 || h < 0 || h > 100) {
      if (!isSensorErrorAlertSent) {
        sendTelegramMessage("⚠️ *ALERT: SENSOR EROR*\nSensor SHT31 tidak terdeteksi!");
        isSensorErrorAlertSent = true;
      }
      return;
    } else {
      isSensorErrorAlertSent = false;
    }

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
      return;
    }

    char dateStr[12];
    char timeStr[25];
    strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &timeinfo);
    strftime(timeStr, sizeof(timeStr), "%d/%m/%Y, %H.%M.%S", &timeinfo);

    int currentHour = timeinfo.tm_hour;
    int currentMin  = timeinfo.tm_min;

    if (currentObsDate != String(dateStr)) {
      currentObsDate = String(dateStr);
      bmkgMaxSuhu = -999.0;
      bmkgMinSuhu = 999.0;

      fetchTodayEkstremFromFirebase(currentObsDate);

      String initPayload = "{\"tanggal\":\"" + currentObsDate + "\"}";
      sendFirebaseRequest("logs_ekstrem_bmkg/" + currentObsDate, initPayload, "PATCH");
    }

    long rssi = WiFi.RSSI();
    int wifiQuality = getWifiQuality(rssi);
    uint32_t freeRam = ESP.getFreeHeap();
    time_t nowEpoch = time(nullptr);

    // Update real-time ke nodus cuaca
    String realTimePayload = "{\"suhu\":" + String(t, 1) +
                             ",\"kelembapan\":" + String(h, 1) +
                             ",\"qfe\":" + String(qfe, 1) +
                             ",\"qff\":" + String(qff, 1) +
                             ",\"qnh\":" + String(qnh, 1) +
                             ",\"tipCount\":" + String(currentTip24) +
                             ",\"rainfall\":" + String(rain24h, 2) +
                             ",\"freeRam\":" + String(freeRam) +
                             ",\"rssi\":" + String(rssi) +
                             ",\"wifiQuality\":" + String(wifiQuality) +
                             ",\"timestamp\":" + String(nowEpoch) + "}";
    sendFirebaseRequest("cuaca", realTimePayload, "PUT");

    // Output status monitor Serial Monitor
    Serial.println("\n--------------------------------------------------");
    Serial.println("[✔] Transmisi Data: " + String(timeStr));
    Serial.printf("    Suhu: %.1f C | RH: %.1f %% | Wet: %.1f C\n", t, h, calculateWetBulb(t, h));
    Serial.printf("    QFE: %.1f hPa | QFF: %.1f hPa | QNH: %.1f hPa\n", qfe, qff, qnh);
    Serial.printf("    Hujan 24H: %.2f mm (Tip: %d) | RAM: %.1f KB\n", rain24h, currentTip24, freeRam / 1024.0);

    // Peringatan deteksi hujan Telegram
    if (rain24h > 0.0 && rain24h > lastNotifiedRain) {
      String rainMsg = "🌧️ *PERINGATAN HUJAN TERDETEKSI*\n🕒 Waktu: `" + String(timeStr) + "`\n☔ Curah Hujan: *" + String(rain24h, 2) + " mm*";
      sendTelegramMessage(rainMsg);
      lastNotifiedRain = rain24h;
      isRainAlertSent = true;
    } else if (rain24h == 0.0 && isRainAlertSent) {
      sendTelegramMessage("☀️ *CURAH HUJAN TERHENTI*\n🕒 Waktu: `" + String(timeStr) + "`");
      lastNotifiedRain = 0.0;
      isRainAlertSent = false;
    }

    // Peringatan suhu ekstrem Telegram
    if (t >= TEMP_HIGH_THRESHOLD || h <= HUM_LOW_THRESHOLD) {
      if (!isTempAlertSent) {
        String alertMsg = "🔥 *PERINGATAN EKSTREM*\n🕒 Waktu: `" + String(timeStr) + "`\n🌡️ Suhu: *" + String(t, 1) + " °C*";
        sendTelegramMessage(alertMsg);
        isTempAlertSent = true;
      }
    } else {
      isTempAlertSent = false;
    }

    // 1. Pencatatan pengamatan jam-jaman (Menit 00)
    if (currentMin == 0 && lastRecordedHour != currentHour) {
      lastRecordedHour = currentHour;
      float wetBulb = calculateWetBulb(t, h);

      String hourlyPayload = "{\"waktu\":\"" + String(timeStr) +
                             "\",\"suhuKering\":" + String(t, 1) +
                             ",\"kelembapan\":" + String(h, 1) +
                             ",\"suhuBasah\":" + String(wetBulb, 1) +
                             ",\"qfe\":" + String(qfe, 1) +
                             ",\"qff\":" + String(qff, 1) +
                             ",\"qnh\":" + String(qnh, 1) +
                             ",\"curahHujan\":" + String(rain24h, 2) + "}";
      
      sendFirebaseRequest("logs_pengamatan_jam", hourlyPayload, "POST");
    }

    // 2. Pencatatan hujan 3 jam-jaman (10.00, 13.00, 16.00, 19.00 WIB)
    if ((currentHour == 10 || currentHour == 13 || currentHour == 16 || currentHour == 19) && currentMin == 0 && last3HourRecorded != currentHour) {
      last3HourRecorded = currentHour;
      
      String payload3H = "{\"waktu\":\"" + String(timeStr) +
                         "\",\"jamObservasi\":\"" + String(currentHour) + ".00 WIB" +
                         "\",\"curahHujan3Jam\":" + String(rain3h, 2) + "}";
      sendFirebaseRequest("logs_hujan_3jam", payload3H, "POST");

      noInterrupts();
      tipCount3Hour = 0;
      interrupts();
    }

    // 3. Rekapitulasi 24 jam BMKG (Pukul 07.00 WIB)
    if (currentHour == 7 && currentMin == 0) {
      if (!is24HourRecorded) {
        is24HourRecorded = true;

        String payload24H = "{\"tanggal\":\"" + String(dateStr) +
                            "\",\"waktuCetak\":\"" + String(timeStr) +
                            "\",\"totalHujan24Jam\":" + String(rain24h, 2) + "}";
        sendFirebaseRequest("logs_hujan_24jam/" + String(dateStr), payload24H, "PATCH");

        noInterrupts();
        tipCount24Hour = 0;
        interrupts();
        lastNotifiedRain = 0.0;
        isRainAlertSent = false;
      }
    } else {
      is24HourRecorded = false;
    }

    // 4. Rekapitulasi Suhu Ekstrem Harian
    if (currentHour >= 7 && currentHour < 19) {
      if (t > bmkgMaxSuhu) {
        bmkgMaxSuhu = t;
        String maxPayload = "{\"tanggal\":\"" + String(dateStr) + "\",\"maxSuhu\":" + String(bmkgMaxSuhu, 1) + "}";
        sendFirebaseRequest("logs_ekstrem_bmkg/" + String(dateStr), maxPayload, "PATCH");
      }
    } else {
      if (t < bmkgMinSuhu) {
        bmkgMinSuhu = t;
        String minPayload = "{\"tanggal\":\"" + String(dateStr) + "\",\"minSuhu\":" + String(bmkgMinSuhu, 1) + "}";
        sendFirebaseRequest("logs_ekstrem_bmkg/" + String(dateStr), minPayload, "PATCH");
      }
    }
  }
}