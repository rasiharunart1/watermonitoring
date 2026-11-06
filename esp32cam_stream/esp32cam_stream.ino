/*
 * ESP32-CAM (AI Thinker) - Stream Kamera & REST API untuk FastAPI Backend.
 * - Board: "AI Thinker ESP32-CAM". PSRAM aktif. Baud 115200.
 * - Library: WiFiManager (tzapu) via Library Manager.
 *
 * Endpoint Tersedia:
 *   Port 80 (REST API & Web UI):
 *     - GET  /         : Dashboard web visual + kontrol flash & status API
 *     - GET  /capture  : Snapshot 1 frame JPEG mentah (CORS enabled, untuk FastAPI snapshot / AI CV)
 *     - GET  /status   : Status perangkat JSON (IP, RSSI, PSRAM, Heap, Flash)
 *     - GET  /flash    : Kontrol flash LED (?state=1, ?state=0, atau ?toggle=1)
 *     - GET  /reset    : Reset konfigurasi WiFi (WiFiManager)
 *   Port 81 (Streaming):
 *     - GET  /stream   : MJPEG stream murni (multipart/x-mixed-replace, CORS enabled)
 *
 * Catatan Arsitektur FastAPI:
 *   FastAPI dapat melakukan proxy stream port 81 via StreamingResponse,
 *   serta mengambil snapshot JPEG on-demand via port 80 (/capture) untuk
 *   diproses OpenCV / YOLO / disimpan ke cloud storage.
 */
#include "esp_camera.h"
#include <WiFiManager.h>
#include <WebServer.h>
#include "img_converters.h"

// Pin AI Thinker ESP32-CAM
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// Pin Flash LED bawaan AI Thinker ESP32-CAM
#define FLASH_LED_PIN      4

WebServer portal(80);       // Port 80: REST API (/capture, /status, /flash) + UI
WebServer streamer(81);     // Port 81: MJPEG Stream (terpisah agar tidak blocking REST)
camera_fb_t *fb = NULL;

bool camInit() {
  camera_config_t cfg = {};
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.ledc_timer   = LEDC_TIMER_0;
  cfg.pin_pwdn     = PWDN_GPIO_NUM;
  cfg.pin_reset    = RESET_GPIO_NUM;
  cfg.pin_xclk     = XCLK_GPIO_NUM;
  cfg.pin_sscb_sda = SIOD_GPIO_NUM;
  cfg.pin_sscb_scl = SIOC_GPIO_NUM;
  cfg.pin_d7       = Y9_GPIO_NUM;
  cfg.pin_d6       = Y8_GPIO_NUM;
  cfg.pin_d5       = Y7_GPIO_NUM;
  cfg.pin_d4       = Y6_GPIO_NUM;
  cfg.pin_d3       = Y5_GPIO_NUM;
  cfg.pin_d2       = Y4_GPIO_NUM;
  cfg.pin_d1       = Y3_GPIO_NUM;
  cfg.pin_d0       = Y2_GPIO_NUM;
  cfg.pin_vsync    = VSYNC_GPIO_NUM;
  cfg.pin_href     = HREF_GPIO_NUM;
  cfg.pin_pclk     = PCLK_GPIO_NUM;
  cfg.xclk_freq_hz = 20000000;
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.frame_size   = FRAMESIZE_QVGA;   // Fallback jika tanpa PSRAM (320x240)
  cfg.jpeg_quality = 12;               // 0-63 (makin kecil makin bagus kompresinya)
  cfg.fb_count     = 1;
  cfg.fb_location  = CAMERA_FB_IN_DRAM;

  // Jika board memiliki PSRAM, tingkatkan resolusi & buffer ganda (smooth streaming)
  if (psramFound()) {
    cfg.frame_size  = FRAMESIZE_SVGA;  // 800x600 (ideal untuk stream & AI)
    cfg.jpeg_quality = 10;
    cfg.fb_count    = 2;
    cfg.fb_location = CAMERA_FB_IN_PSRAM;
  }
  return esp_camera_init(&cfg) == ESP_OK;
}

// Handler Dashboard Web & Dokumentasi API pada Port 80
void handleRoot() {
  String ip = WiFi.localIP().toString();
  String html = "<!DOCTYPE html><html lang='id'><head>"
    "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32-CAM Water Monitoring</title>"
    "<style>"
    "body{margin:0;background:#0d1117;color:#c9d1d9;font-family:system-ui,-apple-system,sans-serif;padding:16px;text-align:center}"
    "h2{color:#58a6ff;margin-bottom:8px}"
    ".stream-box{display:inline-block;max-width:800px;width:100%;border-radius:8px;overflow:hidden;box-shadow:0 4px 14px rgba(0,0,0,0.5);background:#161b22}"
    "img{width:100%;height:auto;display:block}"
    ".btn-group{margin:16px 0;display:flex;gap:8px;justify-content:center;flex-wrap:wrap}"
    "a.btn,button.btn{background:#238636;color:#fff;border:none;padding:8px 14px;border-radius:6px;cursor:pointer;text-decoration:none;font-weight:600;font-size:14px}"
    "a.btn:hover,button.btn:hover{background:#2ea043}"
    ".btn-secondary{background:#30363d!important;color:#c9d1d9!important}"
    ".btn-secondary:hover{background:#3c444d!important}"
    ".btn-danger{background:#da3633!important}"
    ".btn-danger:hover{background:#f85149!important}"
    ".info-card{max-width:800px;margin:16px auto;background:#161b22;padding:14px;border-radius:8px;text-align:left;border:1px solid #30363d;font-size:13px;line-height:1.6}"
    "code{background:#21262d;padding:2px 6px;border-radius:4px;color:#79c0ff;font-family:monospace}"
    "</style></head><body>"
    "<h2>Node ESP32-CAM Water Monitoring</h2>"
    "<p style='color:#8b949e;margin-top:0'>Terhubung ke WiFi: <b>" + WiFi.SSID() + "</b> | IP: <code>" + ip + "</code></p>"
    "<div class='stream-box'>"
    "<img src='http://" + ip + ":81/stream' alt='Live Stream'>"
    "</div>"
    "<div class='btn-group'>"
    "<a class='btn' href='http://" + ip + "/capture' target='_blank'>Ambil Snapshot (JPEG)</a>"
    "<button class='btn btn-secondary' onclick=\"fetch('/flash?toggle=1').then(r=>r.json()).then(d=>alert('Flash LED: '+(d.flash?'NYALA':'MATI')))\">Toggle Flash LED</button>"
    "<a class='btn btn-secondary' href='http://" + ip + "/status' target='_blank'>Status JSON</a>"
    "<a class='btn btn-danger' href='http://" + ip + "/reset' onclick=\"return confirm('Yakin ingin reset koneksi WiFi?')\">Reset WiFi</a>"
    "</div>"
    "<div class='info-card'>"
    "<b>Endpoint Integrasi FastAPI Backend:</b><br>"
    "• <b>Live Stream MJPEG:</b> <code>http://" + ip + ":81/stream</code> (digunakan oleh FastAPI <code>StreamingResponse</code>)<br>"
    "• <b>Snapshot Still Frame:</b> <code>http://" + ip + "/capture</code> (digunakan oleh FastAPI untuk inspeksi AI/OpenCV)<br>"
    "• <b>Device Telemetry Status:</b> <code>http://" + ip + "/status</code><br>"
    "• <b>Kontrol Flash LED:</b> <code>http://" + ip + "/flash?state=1</code> (1=ON, 0=OFF)<br>"
    "</div>"
    "</body></html>";
  portal.send(200, "text/html", html);
}

// Handler Snapshot JPEG Tunggal (Untuk FastAPI / OpenCV / AI Image Processing)
void handleCapture() {
  camera_fb_t *frame = esp_camera_fb_get();
  if (!frame) {
    portal.sendHeader("Access-Control-Allow-Origin", "*");
    portal.send(503, "application/json", "{\"error\":\"Kamera gagal mengambil frame\"}");
    return;
  }
  portal.sendHeader("Access-Control-Allow-Origin", "*");
  portal.sendHeader("Content-Disposition", "inline; filename=capture.jpg");
  portal.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  portal.setContentLength(frame->len);
  portal.send(200, "image/jpeg", "");

  WiFiClient client = portal.client();
  if (client.connected()) {
    client.write(frame->buf, frame->len);
  }
  esp_camera_fb_return(frame);
}

// Handler Status Perangkat dalam JSON (Telemetri Node Kamera)
void handleStatus() {
  String json = "{";
  json += "\"status\":\"ok\",";
  json += "\"device\":\"ESP32-CAM (AI Thinker)\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"psram\":" + String(psramFound() ? "true" : "false") + ",";
  json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"flash_led\":" + String(digitalRead(FLASH_LED_PIN) == HIGH ? "true" : "false") + ",";
  json += "\"endpoints\":{";
  json += "\"stream\":\"http://" + WiFi.localIP().toString() + ":81/stream\",";
  json += "\"capture\":\"http://" + WiFi.localIP().toString() + "/capture\",";
  json += "\"status\":\"http://" + WiFi.localIP().toString() + "/status\",";
  json += "\"flash\":\"http://" + WiFi.localIP().toString() + "/flash?state=1|0\"";
  json += "}}";

  portal.sendHeader("Access-Control-Allow-Origin", "*");
  portal.send(200, "application/json", json);
}

// Handler Flash LED (GPIO 4)
void handleFlash() {
  if (portal.hasArg("state")) {
    String st = portal.arg("state");
    bool on = (st == "1" || st.equalsIgnoreCase("on") || st.equalsIgnoreCase("true"));
    digitalWrite(FLASH_LED_PIN, on ? HIGH : LOW);
  } else if (portal.hasArg("toggle")) {
    digitalWrite(FLASH_LED_PIN, !digitalRead(FLASH_LED_PIN));
  }
  bool current = (digitalRead(FLASH_LED_PIN) == HIGH);
  portal.sendHeader("Access-Control-Allow-Origin", "*");
  portal.send(200, "application/json", "{\"flash\":" + String(current ? "true" : "false") + "}");
}

// Handler Reset Konfigurasi WiFi
void handleReset() {
  portal.sendHeader("Access-Control-Allow-Origin", "*");
  portal.send(200, "text/html", "WiFi direset. Silakan hubungkan ke AP 'ESP32CAM-Setup' (192.168.4.1)");
  delay(1500);
  WiFiManager wm;
  wm.resetSettings();
  ESP.restart();
}

// Handler Stream MJPEG Bersih pada Port 81
void handleStream() {
  WiFiClient client = streamer.client();
  client.print("HTTP/1.1 200 OK\r\n"
               "Access-Control-Allow-Origin: *\r\n"
               "Content-Type: multipart/x-mixed-replace;boundary=frame\r\n"
               "Cache-Control: no-cache, private\r\n"
               "Pragma: no-cache\r\n\r\n");

  while (client.connected()) {
    fb = esp_camera_fb_get();
    if (!fb) {
      delay(10);
      continue;
    }
    client.print("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + String(fb->len) + "\r\n\r\n");
    client.write(fb->buf, fb->len);
    client.print("\r\n");
    esp_camera_fb_return(fb);
    fb = NULL;

    // Yield ke background task ESP32 FreeRTOS agar WDT tidak terpicu
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void setup() {
  Serial.begin(115200);

  // Inisialisasi Flash LED (matikan saat boot)
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  if (!camInit()) {
    Serial.println("Kamera gagal init. Cek wiring, konektor pita kamera, atau board PSRAM.");
    delay(3000);
    ESP.restart();
  }

  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);

  if (WiFi.SSID() == "" || WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Koneksi WiFi belum ada / gagal. Buka AP: ESP32CAM-Setup (pass: 12345678)");
    wm.autoConnect("ESP32CAM-Setup", "12345678");
  }

  Serial.println("\n--- ESP32-CAM Siap ---");
  Serial.print("IP Address : http://"); Serial.println(WiFi.localIP());
  Serial.print("Stream URL : http://"); Serial.print(WiFi.localIP()); Serial.println(":81/stream");
  Serial.print("Capture URL: http://"); Serial.print(WiFi.localIP()); Serial.println("/capture");
  Serial.print("Status URL : http://"); Serial.print(WiFi.localIP()); Serial.println("/status");

  // Endpoint Port 80 (REST + UI)
  portal.on("/", HTTP_GET, handleRoot);
  portal.on("/capture", HTTP_GET, handleCapture);
  portal.on("/status", HTTP_GET, handleStatus);
  portal.on("/flash", HTTP_GET, handleFlash);
  portal.on("/reset", HTTP_GET, handleReset);
  portal.begin();

  // Endpoint Port 81 (MJPEG Stream)
  streamer.on("/stream", HTTP_GET, handleStream);
  streamer.begin();
}

void loop() {
  portal.handleClient();
  streamer.handleClient();
}

