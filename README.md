# 🌦️ Stasiun Observasi Meteorologi Digital - BMKG Deli Serdang

Sistem pemantauan cuaca otomatis (Automatic Weather Station) berbasis mikrokontroler NodeMCU ESP8266 yang terintegrasi dengan sensor meteorologi, Firebase Realtime Database, bot notifikasi Telegram, dan dashboard visualisasi operasional bertaraf BMKG.

---

## 📌 Fitur Utama

- **Pemantauan Suhu & Kelembapan**: Pembacaan suhu bola kering dan kelembapan relatif secara real-time via sensor SHT31-D, serta kalkulasi otomatis suhu bola basah (*Wet Bulb Temperature*) menggunakan persamaan empiris Stull.
- **Perhitungan Barometrik Standar BMKG**:
  - **QFE**: Tekanan udara permukaan stasiun terkalibrasi barometer standar Vaisala PTB330 BMKG (elevasi 89.5 m dpl dengan offset kalibrasi -2.38 hPa).
  - **QFF**: Reduksi tekanan udara ke permukaan laut meteorologi dengan memperhitungkan suhu aktual stasiun.
  - **QNH**: Tekanan standar atmosfer penerbangan internasional (ICAO Standard Atmosphere 15°C).
- **Penakar Hujan Tipping Bucket**:
  - Resolusi 0.2 mm per tip.
  - Dilengkapi filter ganda (*hardware debounce* jeda 400 ms dan validasi kestabilan tegangan 30 ms) guna mencegah pencatatan palsu akibat getaran tiang atau hembusan angin kencang.
  - Akumulasi curah hujan otomatis per siklus real-time, periode 3 jam (10.00, 13.00, 16.00, 19.00 WIB), dan siklus harian 24 jam BMKG (pukul 07.00 – 07.00 WIB).
- **Suhu Ekstrem Harian**:
  - Suhu Maksimum harian (periode pengamatan siang 07.00 – 19.00 WIB).
  - Suhu Minimum harian (periode pengamatan malam 19.00 – 07.00 WIB).
- **Integrasi Bot Telegram Interaktif**:
  - `/info` : Menampilkan ringkasan seluruh parameter sensor, kualitas sinyal Wi-Fi, dan sisa memori RAM perangkat.
  - `/reset_hujan` : Mengenolkan akumulasi curah hujan dan tip count di RAM alat sekaligus menyelaraskannya ke Firebase.
  - `/reboot` : Melakukan restart modul ESP8266 secara jarak jauh (*remote*).
  - Notifikasi otomatis saat mendeteksi hujan, suhu ekstrem melebihi batas, atau jika sensor mengalami kegagalan baca.

---

## 🛠️ Konfigurasi Rangkaian Pin (Hardware Wiring)

| Komponen / Sensor | Pin Modul Sensor | Pin NodeMCU ESP8266 | Keterangan |
| :--- | :---: | :---: | :--- |
| **SHT31-D** | VCC | 3V3 | Catu daya modul 3.3V |
| **SHT31-D** | GND | GND | Ground sistem bersama |
| **SHT31-D** | SDA | D2 (GPIO 4) | Jalur komunikasi data I2C |
| **SHT31-D** | SCL | D1 (GPIO 5) | Jalur komunikasi clock I2C |
| **BMP180** | VCC | 3V3 | Catu daya modul 3.3V |
| **BMP180** | GND | GND | Ground sistem bersama |
| **BMP180** | SDA | D2 (GPIO 4) | Jalur paralel I2C bus |
| **BMP180** | SCL | D1 (GPIO 5) | Jalur paralel I2C bus |
| **Tipping Bucket** | Signal (Reed Switch) | D5 (GPIO 14) | Mode Interrupt `FALLING` dengan *Internal Pull-Up* |
| **Tipping Bucket** | Ground | GND | Ground sistem bersama |

---

## 📂 Struktur Proyek

```text
stasiun_meteo_bmkg/
├── stasiun_meteo_bmkg.ino    # Firmware utama ESP8266
├── config.example.h          # Template konfigurasi kosong untuk publik
├── config.h                  # Kredensial asli pribadi
├── index.html                # Web dashboard interaktif BMKG
└── README.md                 # Dokumentasi panduan operasional sistem
