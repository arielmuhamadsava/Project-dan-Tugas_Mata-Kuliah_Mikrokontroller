#include <Arduino.h>
#include <Wire.h>
#include <DHT.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ============================================================================
// 1. KONFIGURASI PIN HARDWARE
// ============================================================================
#define PIN_MQ2_ANALOG    4     // Pembagi tegangan A0 MQ-2
#define PIN_DHT           5     // Pin Data Sensor DHT22
#define PIN_PWM_KIPAS     6     // Pin Trigger/PWM Modul MOSFET
#define PIN_SAKLAR_TURBO  7     // Saklar 3-Mode Posisi II (Turbo - Aktif LOW)
#define PIN_OLED_SDA      8     // I2C SDA Layar OLED
#define PIN_OLED_SCL      9     // I2C SCL Layar OLED
#define PIN_BUZZER        10    // Pin Positif (+) Buzzer Alarm
#define PIN_POTENSIO      11    // Kaki Tengah (Wiper) Potensiometer 10k
#define PIN_SAKLAR_MANUAL 12    // Saklar 3-Mode Posisi I (Manual - Aktif LOW)

// ============================================================================
// 2. PARAMETER AMBANG BATAS (THRESHOLDS) & PWM
// ============================================================================
// Ambang Batas Asap (Nilai ADC 0 - 4095)
const int BATAS_ASAP_SEDANG        = 600;   // Kipas 50%
const int BATAS_ASAP_BANYAK        = 1200;  // Kipas 75%
const int BATAS_ASAP_BAHAYA        = 3000;  // Kipas 100% + Buzzer

// Ambang Batas Suhu Dapur (Celcius)
const float BATAS_SUHU_SEDANG      = 36.0f; // Kipas 50%
const float BATAS_SUHU_TINGGI      = 40.0f; // Kipas 75%
const float BATAS_SUHU_BAHAYA      = 42.0f; // Kipas 100% + Buzzer

// Konfigurasi Hardware PWM (LEDC ESP32)
const int CHANNEL_PWM_KIPAS        = 0;     // Channel internal LEDC (0-7)
const int FREKUENSI_PWM_KIPAS      = 500;   // 500 Hz (Ideal untuk modul MOSFET)
const int RESOLUSI_PWM_BIT         = 8;     // 8-bit (Nilai 0 - 255)

// Nilai Duty Cycle PWM (Skala 0 - 255)
const int PWM_MATI                 = 0;     // 0%
const int PWM_50_PERSEN            = 128;   // ~50%
const int PWM_75_PERSEN            = 191;   // ~75%
const int PWM_100_PERSEN           = 255;   // 100%

// Pengaturan Filter & Pewaktu (Millisecond)
const float BOBOT_FILTER_BARU      = 0.25f; // Filter penghalus bacaan MQ-2
const unsigned long INTERVAL_UTAMA = 300;   // Jeda baca MQ-2, Saklar & OLED (300ms)
const unsigned long INTERVAL_DHT   = 2000;  // Jeda khusus DHT22 (2 detik)

// ============================================================================
// 3. INISIALISASI OBJEK & VARIABEL GLOBAL
// ============================================================================
#define LEBAR_LAYAR  128
#define TINGGI_LAYAR 64
#define RESET_OLED   -1
Adafruit_SSD1306 oled(LEBAR_LAYAR, TINGGI_LAYAR, &Wire, RESET_OLED);

#define TIPE_DHT DHT22
DHT dht(PIN_DHT, TIPE_DHT);

// Variabel Data Sistem
float suhuDapur           = 0.0f;
float kelembapanDapur     = 0.0f;
int   asapMentah          = 0;
int   asapTerfilter       = 0;
int   nilaiPotensio       = 0;
int   pwmKipas            = 0;
int   persenKipas         = 0;
bool  statusBuzzer        = false;
String teksMode           = "INIT";

// Variabel Pewaktu Non-Blocking
unsigned long waktuTerakhirUtama = 0;
unsigned long waktuTerakhirDHT   = 0;

// Deklarasi Fungsi Prototype
void bacaSensorDHT();
void bacaInputAnalog();
void tentukanModeDanKipas();
void terapkanOutputHardware();
void perbaruiLayarOLED();
void cetakSerialMonitor();
void tampilkanLayarPembuka();

// ============================================================================
// 4. FUNGSI SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);

  // Konfigurasi Mode Pin Input/Output
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_SAKLAR_TURBO, INPUT_PULLUP);
  pinMode(PIN_SAKLAR_MANUAL, INPUT_PULLUP);
  pinMode(PIN_POTENSIO, INPUT);
  pinMode(PIN_MQ2_ANALOG, INPUT);

  // Setup Hardware PWM (Kompatibel dengan Core ESP32 2.x & 3.x)
  #if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    ledcAttach(PIN_PWM_KIPAS, FREKUENSI_PWM_KIPAS, RESOLUSI_PWM_BIT);
    ledcWrite(PIN_PWM_KIPAS, PWM_MATI);
  #else
    ledcSetup(CHANNEL_PWM_KIPAS, FREKUENSI_PWM_KIPAS, RESOLUSI_PWM_BIT);
    ledcAttachPin(PIN_PWM_KIPAS, CHANNEL_PWM_KIPAS);
    ledcWrite(CHANNEL_PWM_KIPAS, PWM_MATI);
  #endif
  
  digitalWrite(PIN_BUZZER, LOW);

  // Mulai Sensor DHT22 & Komunikasi I2C OLED
  dht.begin();
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);

  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[ERROR] Layar OLED SSD1306 tidak terdeteksi!");
    while (true) {
      delay(100);
    }
  }

  // Ambil nilai awal MQ-2 agar filter langsung stabil sejak awal
  asapTerfilter = analogRead(PIN_MQ2_ANALOG);

  tampilkanLayarPembuka();
  Serial.println("=== SISTEM SMART KITCHEN EXHAUST SIAP ===");
}

// ============================================================================
// 5. FUNGSI LOOP UTAMA
// ============================================================================
void loop() {
  unsigned long waktuSekarang = millis();

  // A. Baca Sensor Suhu DHT22 setiap 2 Detik
  if (waktuSekarang - waktuTerakhirDHT >= INTERVAL_DHT) {
    waktuTerakhirDHT = waktuSekarang;
    bacaSensorDHT();
  }

  // B. Baca MQ-2, Saklar, Kontrol Kipas & Layar setiap 300 ms
  if (waktuSekarang - waktuTerakhirUtama >= INTERVAL_UTAMA) {
    waktuTerakhirUtama = waktuSekarang;

    bacaInputAnalog();
    tentukanModeDanKipas();
    terapkanOutputHardware();
    perbaruiLayarOLED();
    cetakSerialMonitor();
  }
}

// ============================================================================
// 6. DEFINISI FUNGSI MODULAR
// ============================================================================

void bacaSensorDHT() {
  float bacaSuhu = dht.readTemperature();
  float bacaLembap = dht.readHumidity();

  if (!isnan(bacaSuhu))   suhuDapur = bacaSuhu;
  if (!isnan(bacaLembap)) kelembapanDapur = bacaLembap;
}

void bacaInputAnalog() {
  asapMentah = analogRead(PIN_MQ2_ANALOG);
  
  // Exponential Moving Average agar pembacaan asap naik/turun secara halus
  asapTerfilter = (int)((asapTerfilter * (1.0f - BOBOT_FILTER_BARU)) + (asapMentah * BOBOT_FILTER_BARU));

  nilaiPotensio = analogRead(PIN_POTENSIO);
}

void tentukanModeDanKipas() {
  bool saklarTurboAktif  = (digitalRead(PIN_SAKLAR_TURBO) == LOW);
  bool saklarManualAktif = (digitalRead(PIN_SAKLAR_MANUAL) == LOW);

  if (saklarTurboAktif) {
    // --- MODE TURBO (100%) ---
    teksMode     = "TURBO (100%)";
    pwmKipas     = PWM_100_PERSEN;
    persenKipas  = 100;
    statusBuzzer = false;
  } 
  else if (saklarManualAktif) {
    // --- MODE MANUAL (POTENSIOMETER) ---
    teksMode     = "MANUAL";
    pwmKipas     = map(nilaiPotensio, 0, 4095, 0, 255);
    persenKipas  = map(pwmKipas, 0, 255, 0, 100);
    statusBuzzer = false;
  } 
  else {
    // --- MODE OTOMATIS BERTINGKAT (3 LEVEL) ---
    // Cek dari kondisi paling kritis (Bahaya -> Tinggi -> Sedang -> Aman)
    
    if (asapTerfilter >= BATAS_ASAP_BAHAYA || suhuDapur >= BATAS_SUHU_BAHAYA) {
      // LEVEL 3: BAHAYA (Kipas 100% + Buzzer Bunyi)
      if (asapTerfilter >= BATAS_ASAP_BAHAYA && suhuDapur >= BATAS_SUHU_BAHAYA) {
        teksMode = "BAHAYA: ASAP+SUHU";
      } else if (asapTerfilter >= BATAS_ASAP_BAHAYA) {
        teksMode = "BAHAYA: ASAP!";
      } else {
        teksMode = "BAHAYA: SUHU!";
      }
      pwmKipas     = PWM_100_PERSEN;
      persenKipas  = 100;
      statusBuzzer = true;
    } 
    else if (asapTerfilter >= BATAS_ASAP_BANYAK || suhuDapur >= BATAS_SUHU_TINGGI) {
      // LEVEL 2: BANYAK / TINGGI (Kipas 75%, Buzzer Mati)
      if (asapTerfilter >= BATAS_ASAP_BANYAK) {
        teksMode = "AUTO: ASAP BANYAK";
      } else {
        teksMode = "AUTO: SUHU TINGGI";
      }
      pwmKipas     = PWM_75_PERSEN;
      persenKipas  = 75;
      statusBuzzer = false;
    } 
    else if (asapTerfilter >= BATAS_ASAP_SEDANG || suhuDapur >= BATAS_SUHU_SEDANG) {
      // LEVEL 1: SEDANG (Kipas 50%, Buzzer Mati)
      if (asapTerfilter >= BATAS_ASAP_SEDANG) {
        teksMode = "AUTO: ASAP SEDANG";
      } else {
        teksMode = "AUTO: SUHU SEDANG";
      }
      pwmKipas     = PWM_50_PERSEN;
      persenKipas  = 50;
      statusBuzzer = false;
    } 
    else {
      // LEVEL 0: AMAN (Kipas Mati, Buzzer Mati)
      teksMode     = "AUTO: AMAN";
      pwmKipas     = PWM_MATI;
      persenKipas  = 0;
      statusBuzzer = false;
    }
  }
}

void terapkanOutputHardware() {
  #if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
    ledcWrite(PIN_PWM_KIPAS, pwmKipas);
  #else
    ledcWrite(CHANNEL_PWM_KIPAS, pwmKipas);
  #endif

  digitalWrite(PIN_BUZZER, statusBuzzer ? HIGH : LOW);
}

void perbaruiLayarOLED() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  // Baris 1: Status Mode Spesifik
  oled.setCursor(0, 0);
  oled.println(teksMode);
  oled.drawLine(0, 10, 128, 10, SSD1306_WHITE);

  // Baris 2: Suhu & Kelembapan
  oled.setCursor(0, 15);
  oled.print("Suhu : ");
  oled.print(suhuDapur, 1);
  oled.print("C (");
  oled.print(kelembapanDapur, 0);
  oled.println("%)");

  // Baris 3: Nilai Asap Terfilter
  oled.setCursor(0, 28);
  oled.print("Asap : ");
  oled.print(asapTerfilter);
  oled.println(" ADC");

  // Baris 4: Persentase Kipas & Status Buzzer
  oled.setCursor(0, 42);
  oled.print("Kipas: ");
  oled.print(persenKipas);
  oled.print("% ");
  if (statusBuzzer) {
    oled.println("[ALARM!]");
  } else {
    oled.println("");
  }

  // Baris 5: Progress Bar Visual Kecepatan Kipas
  oled.drawRect(0, 54, 128, 8, SSD1306_WHITE);
  int lebarBar = map(persenKipas, 0, 100, 0, 124);
  oled.fillRect(2, 56, lebarBar, 4, SSD1306_WHITE);

  oled.display();
}

void cetakSerialMonitor() {
  Serial.print("Status: ");      Serial.print(teksMode);
  Serial.print(" | Asap(M/F): "); Serial.print(asapMentah);
  Serial.print("/");             Serial.print(asapTerfilter);
  Serial.print(" | Suhu: ");     Serial.print(suhuDapur, 1); Serial.print("C");
  Serial.print(" | Kipas: ");    Serial.print(persenKipas);  Serial.print("%");
  Serial.print(" (PWM:");        Serial.print(pwmKipas);     Serial.println(")");
}

void tampilkanLayarPembuka() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(10, 15);
  oled.println("SMART EXHAUST FAN");
  oled.setCursor(25, 35);
  oled.println("Sistem Siap!");
  oled.display();
  delay(1500);
}
