# FastAPI Backend untuk Water Monitoring & ESP32-CAM

Backend REST API & WebSocket berbasis FastAPI untuk menerima telemetri dari node sensor ESP32, menyiarkan live stream MJPEG & snapshot dari ESP32-CAM, serta mengontrol aktuator relay & flash LED.

## Fitur Utama

1. **Proxy Live Stream MJPEG (`GET /api/v1/camera/stream`):**
   - Menghubungkan video feed port 81 ESP32-CAM ke frontend/dashboard tanpa masalah CORS.
   - Menggunakan async generator `httpx` & `StreamingResponse`.
2. **Snapshot Frame Kamera (`GET /api/v1/camera/snapshot`):**
   - Mengambil satu frame still JPEG untuk ditampilkan di UI atau diproses pustaka AI/OpenCV.
3. **Penerima Telemetri Sensor (`POST /api/v1/sensors/readings`):**
   - Menerima pH, turbidity (kekeruhan), suhu air (DS18B20), dan status relay dari ESP32.
   - Validasi data otomatis berbasis Pydantic.
   - Evaluasi otomatis status baku mutu kualitas air (`EXCELLENT`, `ACCEPTABLE`, `WARNING`, `CRITICAL`).
4. **WebSocket Live Broadcast (`/ws/sensors`):**
   - Menyiarkan data pembacaan sensor realtime ke web dashboard seketika saat data diterima dari ESP32.
5. **Riwayat Sensor Berpaginasi (`GET /api/v1/sensors/readings?limit=50&offset=0`):**
   - Mengambil histori telemetri terurut dari yang terbaru.
6. **Kontrol Perangkat:**
   - Kontrol aktuator relay (`POST /api/v1/sensors/relay`).
   - Kontrol lampu flash LED ESP32-CAM GPIO 4 (`POST /api/v1/camera/flash`).

## Cara Menjalankan

### 1. Buat Virtual Environment & Install Dependensi

```bash
cd fastapi_backend
python -m venv venv

# Windows:
venv\Scripts\activate

# Linux/macOS:
source venv/bin/activate

pip install -r requirements.txt
```

### 2. Konfigurasi Variabel Lingkungan (.env)

Salin `.env.example` menjadi `.env`:

```bash
# Windows (PowerShell):
Copy-Item .env.example .env

# Linux/macOS:
cp .env.example .env
```

Sesuaikan `ESP32_CAM_IP` dengan IP yang didapat oleh board ESP32-CAM dari router/WiFi Anda (lihat di Serial Monitor saat boot).

### 3. Jalankan Server FastAPI

```bash
uvicorn main:app --host 0.0.0.0 --port 8000 --reload
```

Dokumentasi interaktif OpenAPI Swagger UI akan otomatis tersedia di:
- **Swagger UI:** `http://localhost:8000/docs`
- **ReDoc:** `http://localhost:8000/redoc`
