/*
 * Water Monitoring ESP32
 * - PH analog: pin 34 (ADC, 3.3V)
 * - Turbidity analog: pin 35
 * - DS18B20: pin 4
 * - Menu serial 115200: pilih sensor, kalibrasi 2 titik (slope).
 * Kalibrasi disimpan di Preferences (NVS), tetap ada setelah reset.
 * ponytail: ADC internal ESP32 cukup untuk demo; ganti ADS1115 bila butuh presisi tinggi.
 *
 * Library (Library Manager): OneWire, DallasTemperature.
 */

// hasil 
// == Kalibrasi PH (2 titik dinamis) ==
// Gunakan larutan buffer yang tersedia, misalnya pH 7 dan pH 9.
// Ukur nilai aktual setiap larutan dengan pH meter konvensional.
// Bilas probe dengan1 air bersih dan keringkan dengan ditepuk pelan sebelum pindah larutan.
// Celupkan probe ke larutan titik-1.
// Ketik y + Enter saat bacaan ESP32 stabil:
// V1 = 2.2605
// Masukkan hasil pH meter konvensional untuk titik-1:
// Bilas probe, lalu celupkan ke larutan titik-2.
// Ketik y + Enter saat bacaan ESP32 stabil:
// V2 = 1.9148
// Masukkan hasil pH meter konvensional untuk titik-2:
// Kalibrasi PH tersimpan. slope=-6.6252 offset=21.7661

// Gunakan larutan buffer yang tersedia, misalnya pH 7 dan pH 9.
// Ukur nilai aktual setiap larutan dengan pH meter konvensional.
// Bilas probe dengan air bersih dan keringkan dengan ditepuk pelan sebelum pindah larutan.
// Celupkan probe ke larutan titik-1.
// Ketik y + Enter saat bacaan ESP32 stabil:
// V1 = 2.3666
// Masukkan hasil pH meter konvensional untuk titik-1:
// Bilas probe, lalu celupkan ke larutan titik-2.
// Ketik y + Enter saat bacaan ESP32 stabil:
// V2 = 1.9534
// Masukkan hasil pH meter konvensional untuk titik-2:
// Kalibrasi PH tersimpan. slope=-7.1642 offset=22.5446

#include <OneWire.h>
#include <DallasTemperature.h>
#include <Preferences.h>

#define PH_PIN    34
#define TURB_PIN  35
#define TEMP_PIN  4
#define RELAY_PIN 27
#define ADC_VREF  3.3f
#define ADC_MAX   4095.0f

// y = slope * volt + offset (kalibrasi 2 titik)
struct Cal { float slope, offset; bool valid; };
Cal calPH = {1.0, 0.0, false}, calTurb = {1.0, 0.0, false};

Preferences prefs;
OneWire oneWire(TEMP_PIN);
DallasTemperature ds18b20(&oneWire);

float readVolt(int pin) { return analogRead(pin) * ADC_VREF / ADC_MAX; }

float applyCal(Cal c, float volt) { return c.slope * volt + c.offset; }

float avgVolt(int pin) {  // 64 sampel untuk buang noise
  float s = 0;
  for (int i = 0; i < 64; i++) { s += readVolt(pin); delay(5); }
  return s / 64.0f;
}

bool waitYes() {
  while (!Serial.available()) delay(10);
  String s = Serial.readStringUntil('\n'); s.trim();
  return s.equalsIgnoreCase("y");
}

float readFloat(const char *prompt) {
  while (true) {
    Serial.println(prompt);
    while (!Serial.available()) delay(10);
    String s = Serial.readStringUntil('\n'); s.trim();
    if (s.length() > 0) {
      float v = s.toFloat();
      if (v != 0 || s.indexOf('0') >= 0) return v;
    }
    Serial.println("Input salah, ulangi.");
  }
}

float readPHReference(const char *prompt) {
  while (true) {
    float value = readFloat(prompt);
    if (value > 0.0f && value <= 14.0f) return value;
    Serial.println("Nilai pH harus lebih besar dari 0 dan maksimal 14.");
  }
}

float captureShortVoltage() {
  unsigned long lastPrint = millis() - 500;
  while (true) {
    if (millis() - lastPrint >= 500) {
      Serial.print("Tegangan realtime PH_PIN: ");
      Serial.print(readVolt(PH_PIN), 4);
      Serial.println(" V | ketik y + Enter untuk ambil rata-rata");
      lastPrint = millis();
    }

    if (Serial.available()) {
      String input = Serial.readStringUntil('\n');
      input.trim();
      if (input.equalsIgnoreCase("y")) return avgVolt(PH_PIN);
      Serial.println("Ketik y + Enter setelah tegangan stabil.");
    }
    delay(10);
  }
}

void saveCal(const char *key, Cal &c) {
  prefs.begin("cal", false);
  prefs.putFloat((String(key) + "m").c_str(), c.slope);
  prefs.putFloat((String(key) + "b").c_str(), c.offset);
  prefs.putBool ((String(key) + "ok").c_str(), true);
  prefs.end();
}

// Kalibrasi 2 titik: y1@v1, y2@v2 -> slope, offset
void calibrate(const char *name, const char *key, int pin, Cal &c) {
  float v1, y1, v2, y2;
  Serial.println();
  Serial.print("== Kalibrasi "); Serial.println(name);
  Serial.println("Celupkan sensor ke LARUTAN TITIK-1 (referensi rendah, mis. air jernih NTU 0).");
  Serial.println("Ketik y + Enter saat bacaan stabil:");
  while (!waitYes()) {}
  v1 = avgVolt(pin);
  Serial.print("V1 = "); Serial.println(v1, 4);
  y1 = readFloat("Masukkan nilai referensi titik-1:");

  Serial.println("Celupkan sensor ke LARUTAN TITIK-2 (referensi tinggi, mis. air keruh NTU diketahui).");
  Serial.println("Ketik y + Enter saat bacaan stabil:");
  while (!waitYes()) {}
  v2 = avgVolt(pin);
  Serial.print("V2 = "); Serial.println(v2, 4);
  y2 = readFloat("Masukkan nilai referensi titik-2:");

  if (fabsf(v2 - v1) < 0.001f) {  // guard pembagi nol
    Serial.println("ERROR: tegangan kedua titik hampir sama. Kalibrasi dibatalkan.");
    return;
  }
  c.slope  = (y2 - y1) / (v2 - v1);
  c.offset = y1 - c.slope * v1;
  c.valid  = true;
  saveCal(key, c);
  Serial.print("Tersimpan. slope="); Serial.print(c.slope, 4);
  Serial.print(" offset="); Serial.println(c.offset, 4);
}

void calibratePH() {
  float v1, y1, v2, y2;
  Serial.println();
  Serial.println("== Kalibrasi PH (2 titik dinamis) ==");
  Serial.println("Gunakan larutan buffer yang tersedia, misalnya pH 7 dan pH 9.");
  Serial.println("Ukur nilai aktual setiap larutan dengan pH meter konvensional.");
  Serial.println("Bilas probe dengan air bersih dan keringkan dengan ditepuk pelan sebelum pindah larutan.");

  Serial.println("Celupkan probe ke larutan titik-1.");
  Serial.println("Ketik y + Enter saat bacaan ESP32 stabil:");
  while (!waitYes()) {}
  v1 = avgVolt(PH_PIN);
  Serial.print("V1 = "); Serial.println(v1, 4);
  y1 = readPHReference("Masukkan hasil pH meter konvensional untuk titik-1:");

  Serial.println("Bilas probe, lalu celupkan ke larutan titik-2.");
  Serial.println("Ketik y + Enter saat bacaan ESP32 stabil:");
  while (!waitYes()) {}
  v2 = avgVolt(PH_PIN);
  Serial.print("V2 = "); Serial.println(v2, 4);
  y2 = readPHReference("Masukkan hasil pH meter konvensional untuk titik-2:");

  if (fabsf(v2 - v1) < 0.001f) {
    Serial.println("ERROR: tegangan kedua titik hampir sama. Kalibrasi dibatalkan.");
    return;
  }
  calPH.slope  = (y2 - y1) / (v2 - v1);
  calPH.offset = y1 - calPH.slope * v1;
  calPH.valid  = true;
  saveCal("ph", calPH);
  Serial.print("Kalibrasi PH tersimpan. slope="); Serial.print(calPH.slope, 4);
  Serial.print(" offset="); Serial.println(calPH.offset, 4);
}

// Kalibrasi offset PH: short kabel prob (tengah BNC) ke GND board.
// Simpan offset agar short -> pH target, slope tetap dari kalibrasi 2 titik.
void calShortPH() {
  Serial.println();
  Serial.println("== Kalibrasi offset PH (short probe ke GND) ==");
  Serial.println("Lepas prob dari air. Short pin tengah BNC prob ke GND board sensor.");
  Serial.println("Tegangan akan dibaca realtime; tidak perlu multimeter.");
  float v = captureShortVoltage();
  Serial.print("V short = "); Serial.println(v, 4);
  if (v < 1.0f || v > 4.0f) {
    Serial.println("WARNING: V short jauh dari 2.5V. Cek kabel short / wiring modul.");
  }
  float targetPH = readPHReference("Masukkan pH target untuk kondisi short (biasanya 7.00):");
  calPH.offset = targetPH - calPH.slope * v;
  calPH.valid = true;
  saveCal("ph", calPH);
  Serial.print("Offset baru = "); Serial.println(calPH.offset, 4);
  Serial.println("Idealnya V short mendekati 2.5V; kalau jauh, putar trimpot offset di modul.");
}

void loadCal() {  prefs.begin("cal", true);
  if (prefs.getBool("phok", false)) {
    calPH.slope = prefs.getFloat("phm", 1.0); calPH.offset = prefs.getFloat("phb", 0.0); calPH.valid = true;
  }
  if (prefs.getBool("tbok", false)) {
    calTurb.slope = prefs.getFloat("tbm", 1.0); calTurb.offset = prefs.getFloat("tbb", 0.0); calTurb.valid = true;
  }
  prefs.end();
}

void setRelay(bool on) {
  // Modul relay umum memakai trigger aktif-LOW.
  digitalWrite(RELAY_PIN, on ? LOW : HIGH);
  Serial.print("Relay state: ");
  Serial.println(on ? "ON" : "OFF");
}

void printMenu() {
  Serial.println();
  Serial.println("===== MENU =====");
  Serial.println("1. Baca semua sensor berulang (ketik stop untuk berhenti)");
  Serial.println("2. Kalibrasi PH (2 titik dinamis)");
  Serial.println("3. Kalibrasi Turbidity (2 titik)");
  Serial.println("4. Reset kalibrasi");
  Serial.println("5. Test relay (ON/OFF bergantian 3x)");
  Serial.println("6. Kalibrasi offset PH (short probe ke GND)");
  Serial.println("Pilih nomor + Enter:");
}

void printSensorReadings() {
  ds18b20.requestTemperatures();
  float t = ds18b20.getTempCByIndex(0);
  Serial.println("--- HASIL ---");
  if (t != DEVICE_DISCONNECTED_C) {
    Serial.print("Suhu: "); Serial.print(t, 2); Serial.println(" C");
  } else {
    Serial.println("Suhu: DS18B20 tidak terdeteksi");
  }

  Serial.print("PH: ");
  if (calPH.valid) Serial.println(applyCal(calPH, avgVolt(PH_PIN)), 2);
  else {
    Serial.print("BELUM KALIBRASI, volt=");
    Serial.println(avgVolt(PH_PIN), 3);
  }

  Serial.print("Turbidity: ");
  if (calTurb.valid) Serial.println(applyCal(calTurb, avgVolt(TURB_PIN)), 1);
  else {
    Serial.print("BELUM KALIBRASI, volt=");
    Serial.println(avgVolt(TURB_PIN), 3);
  }
}

void monitorSensors() {
  Serial.println("Mode baca berulang aktif. Ketik stop + Enter untuk kembali ke menu.");
  while (true) {
    printSensorReadings();
    Serial.println("Pembacaan berikutnya dalam 2 detik...");

    unsigned long started = millis();
    while (millis() - started < 2000) {
      if (Serial.available()) {
        String command = Serial.readStringUntil('\n');
        command.trim();
        if (command.equalsIgnoreCase("stop")) {
          Serial.println("Mode baca berulang dihentikan.");
          return;
        }
      }
      delay(10);
    }
  }
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  pinMode(RELAY_PIN, OUTPUT);
  setRelay(false);
  ds18b20.begin();
  loadCal();
  Serial.println("\nWater Monitoring ESP32 siap.");
}

void loop() {
  printMenu();
  while (!Serial.available()) delay(10);
  String in = Serial.readStringUntil('\n'); in.trim();
  char c = in.charAt(0);
  switch (c) {
    case '1': monitorSensors(); break;
    case '2': calibratePH(); break;
    case '3': calibrate("TURBIDITY", "tb", TURB_PIN, calTurb); break;
    case '4':
      prefs.begin("cal", false); prefs.clear(); prefs.end();
      calPH.valid = calTurb.valid = false;
      Serial.println("Kalibrasi dihapus.");
      break;
    case '5':
      Serial.println("Test relay: klik 3x, 1 detik tiap klik.");
      for (int i = 0; i < 3; i++) {
        setRelay(true);  delay(1000);
        setRelay(false); delay(1000);
      }
      Serial.println("Test relay selesai.");
      break;
    case '6': calShortPH(); break;
    default: Serial.println("Pilihan tidak ada."); break;
  }
}
