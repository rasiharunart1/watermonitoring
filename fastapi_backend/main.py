"""
FastAPI Backend untuk Sistem Water Monitoring & ESP32-CAM
=========================================================
Arsitektur API:
  - Telemetri Sensor: REST POST dari ESP32 Sensor (/api/v1/sensors/readings)
  - Live Realtime Data: WebSocket Broadcast (/ws/sensors)
  - Riwayat Sensor: REST GET berpaginasi (/api/v1/sensors/readings)
  - Kamera Proxy MJPEG: StreamingResponse async dari ESP32-CAM (/api/v1/camera/stream)
  - Kamera Snapshot: Pengambilan frame JPEG tunggal (/api/v1/camera/snapshot)
  - Kontrol Perangkat: Relay (/api/v1/sensors/relay) & Flash LED (/api/v1/camera/flash)
"""

import os
import asyncio
from datetime import datetime, timezone
from typing import List, Optional, Dict, Any
from collections import deque
from contextlib import asynccontextmanager

from dotenv import load_dotenv
import httpx
from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Query, Path, HTTPException, status
from fastapi.responses import StreamingResponse, Response, JSONResponse
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field

# Load konfigurasi dari file .env jika ada
load_dotenv()

ESP32_CAM_IP = os.getenv("ESP32_CAM_IP", "192.168.1.51")
ESP32_CAM_PORT = int(os.getenv("ESP32_CAM_PORT", "80"))
ESP32_CAM_STREAM_PORT = int(os.getenv("ESP32_CAM_STREAM_PORT", "81"))
MAX_HISTORY = int(os.getenv("MAX_HISTORY", "1000"))

# Shared HTTP Client async dengan connection pooling
http_client: Optional[httpx.AsyncClient] = None

@asynccontextmanager
async def lifespan(app: FastAPI):
    global http_client
    # Timeout 10s untuk koneksi, namun streaming menggunakan timeout None
    http_client = httpx.AsyncClient(timeout=10.0)
    yield
    if http_client:
        await http_client.aclose()

app = FastAPI(
    title="Water Monitoring & ESP32-CAM API",
    description="Backend API FastAPI untuk monitoring kualitas air (pH, Turbidity, Suhu) dan streaming ESP32-CAM",
    version="1.0.0",
    lifespan=lifespan,
)

# Konfigurasi CORS (Cross-Origin Resource Sharing)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# ==============================================================================
# SCHEMAS (Pydantic Data Contracts)
# ==============================================================================

class SensorReadingCreate(BaseModel):
    """Payload pembacaan sensor yang dikirim oleh ESP32."""
    temperature: float = Field(..., description="Suhu air dalam Celsius dari sensor DS18B20")
    ph: float = Field(..., ge=0.0, le=14.0, description="Nilai pH terkalibrasi (rentang 0-14)")
    turbidity: float = Field(..., ge=0.0, description="Tingkat kekeruhan air dalam NTU terkalibrasi")
    relay: bool = Field(default=False, description="Status aktif aktuator relay pompa/solenoid")
    device_id: str = Field(default="esp32-sensor-01", description="Identifier node sensor")
    raw_voltages: Optional[Dict[str, float]] = Field(
        default=None,
        description="Voltase mentah ADC sebelum dikalibrasi (opsional untuk audit/re-kalibrasi)"
    )

class SensorReadingResponse(BaseModel):
    """Representasi data sensor lengkap untuk klien frontend."""
    id: int
    timestamp: datetime
    device_id: str
    temperature: float
    ph: float
    turbidity: float
    relay: bool
    water_quality_status: str
    raw_voltages: Optional[Dict[str, float]] = None

class PaginatedReadingsResponse(BaseModel):
    data: List[SensorReadingResponse]
    total: int
    limit: int
    offset: int

class RelayControlRequest(BaseModel):
    state: bool = Field(..., description="True untuk ON (menyala), False untuk OFF (mati)")
    reason: Optional[str] = Field(None, description="Alasan pengubahan status relay")

class FlashControlRequest(BaseModel):
    state: bool = Field(..., description="True untuk ON, False untuk OFF")

class StandardAPIResponse(BaseModel):
    success: bool
    message: str
    data: Optional[Dict[str, Any]] = None

# ==============================================================================
# IN-MEMORY DATA STORE & WEBSOCKET MANAGER
# ==============================================================================

# Ring buffer in-memory untuk menyimpan riwayat pembacaan terakhir
readings_history: deque = deque(maxlen=MAX_HISTORY)
latest_reading: Optional[SensorReadingResponse] = None
reading_counter: int = 0
current_relay_state: bool = False

class ConnectionManager:
    """Mengelola koneksi WebSocket untuk live broadcast data ke web dashboard."""
    def __init__(self):
        self.active_connections: List[WebSocket] = []

    async def connect(self, websocket: WebSocket):
        await websocket.accept()
        self.active_connections.append(websocket)

    def disconnect(self, websocket: WebSocket):
        if websocket in self.active_connections:
            self.active_connections.remove(websocket)

    async def broadcast_json(self, data: dict):
        for connection in list(self.active_connections):
            try:
                await connection.send_json(data)
            except Exception:
                self.disconnect(connection)

ws_manager = ConnectionManager()

def evaluate_water_quality(ph: float, turbidity: float) -> str:
    """
    Evaluasi standar kualitas air sederhana:
      - pH normal: 6.5 - 8.5 (Standar baku mutu air minum Permenkes)
      - Turbidity normal: < 5.0 NTU (air jernih) s/d 25 NTU (air layak)
    """
    if 6.5 <= ph <= 8.5 and turbidity < 5.0:
        return "EXCELLENT"
    elif 6.0 <= ph <= 9.0 and turbidity <= 25.0:
        return "ACCEPTABLE"
    elif ph < 6.0 or ph > 9.0 or turbidity > 50.0:
        return "CRITICAL"
    return "WARNING"

# ==============================================================================
# ENDPOINTS: HEALTH & ROOT
# ==============================================================================

@app.get("/health", tags=["System"])
async def health_check():
    return {
        "status": "healthy",
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "esp32_cam_target": f"http://{ESP32_CAM_IP}:{ESP32_CAM_PORT}",
        "active_ws_clients": len(ws_manager.active_connections),
        "total_readings_stored": len(readings_history),
    }

# ==============================================================================
# ENDPOINTS: SENSOR TELEMETRY & DATA (REST + WEBSOCKET)
# ==============================================================================

@app.post(
    "/api/v1/sensors/readings",
    response_model=SensorReadingResponse,
    status_code=status.HTTP_201_CREATED,
    tags=["Sensors"]
)
async def receive_sensor_reading(payload: SensorReadingCreate):
    """
    Menerima telemetri pembacaan sensor dari node ESP32.
    - Menyimpan ke riwayat (history)
    - Melakukan evaluasi otomatis kualitas air
    - Menyiarkan data secara realtime ke seluruh klien WebSocket
    """
    global reading_counter, latest_reading, current_relay_state

    reading_counter += 1
    current_relay_state = payload.relay

    reading_data = SensorReadingResponse(
        id=reading_counter,
        timestamp=datetime.now(timezone.utc),
        device_id=payload.device_id,
        temperature=payload.temperature,
        ph=payload.ph,
        turbidity=payload.turbidity,
        relay=payload.relay,
        water_quality_status=evaluate_water_quality(payload.ph, payload.turbidity),
        raw_voltages=payload.raw_voltages
    )

    latest_reading = reading_data
    readings_history.append(reading_data)

    # Siarkan ke semua web dashboard yang terhubung ke WebSocket
    asyncio.create_task(ws_manager.broadcast_json(reading_data.model_dump(mode="json")))

    return reading_data

@app.get(
    "/api/v1/sensors/latest",
    response_model=SensorReadingResponse,
    tags=["Sensors"]
)
async def get_latest_sensor_reading():
    """Mengambil pembacaan data sensor paling mutakhir."""
    if latest_reading is None:
        raise HTTPException(
            status_code=status.HTTP_404_NOT_FOUND,
            detail="Belum ada data sensor yang diterima dari ESP32"
        )
    return latest_reading

@app.get(
    "/api/v1/sensors/readings",
    response_model=PaginatedReadingsResponse,
    tags=["Sensors"]
)
async def get_sensor_history(
    limit: int = Query(50, ge=1, le=500, description="Jumlah data per halaman"),
    offset: int = Query(0, ge=0, description="Offset data")
):
    """Mengambil riwayat data sensor dengan paginasi."""
    history_list = list(readings_history)
    total = len(history_list)
    # Urutkan dari data paling baru (reverse)
    reversed_history = list(reversed(history_list))
    paginated = reversed_history[offset : offset + limit]

    return PaginatedReadingsResponse(
        data=paginated,
        total=total,
        limit=limit,
        offset=offset
    )

@app.post(
    "/api/v1/sensors/relay",
    response_model=StandardAPIResponse,
    tags=["Sensors"]
)
async def control_relay(cmd: RelayControlRequest):
    """
    Mengubah status target relay (aktuator pompa/filter/solenoid).
    Status ini dapat di-poll oleh ESP32 atau dikirim balik saat ESP32 POST telemetri.
    """
    global current_relay_state
    current_relay_state = cmd.state
    return StandardAPIResponse(
        success=True,
        message=f"Relay berhasil diatur ke {'ON' if cmd.state else 'OFF'}",
        data={"relay_state": current_relay_state, "reason": cmd.reason}
    )

@app.get(
    "/api/v1/sensors/relay",
    tags=["Sensors"]
)
async def get_relay_state():
    """Mendapatkan status relay terkini."""
    return {"relay": current_relay_state}

@app.websocket("/ws/sensors")
async def websocket_sensors_endpoint(websocket: WebSocket):
    """
    Kanal WebSocket untuk dashboard frontend.
    Begitu terhubung, klien akan menerima data terakhir seketika,
    kemudian menerima broadcast setiap ada telemetri baru dari ESP32.
    """
    await ws_manager.connect(websocket)
    try:
        if latest_reading:
            await websocket.send_json(latest_reading.model_dump(mode="json"))
        # Tunggu pesan dari klien (atau ping heartbeat)
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        ws_manager.disconnect(websocket)
    except Exception:
        ws_manager.disconnect(websocket)

# ==============================================================================
# ENDPOINTS: ESP32-CAM INTEGRATION (STREAM & SNAPSHOT PROXY)
# ==============================================================================

@app.get("/api/v1/camera/stream", tags=["Camera"])
async def stream_camera():
    """
    Proxy live MJPEG stream dari ESP32-CAM (Port 81).
    Keuntungan:
      - Menghindari isu CORS di browser
      - Klien web tidak perlu mengakses IP internal ESP32-CAM langsung
      - Hemat beban koneksi pada ESP32-CAM
    """
    target_url = f"http://{ESP32_CAM_IP}:{ESP32_CAM_STREAM_PORT}/stream"

    async def mjpeg_generator():
        # Gunakan client terpisah dengan timeout tak terbatas untuk streaming
        async with httpx.AsyncClient(timeout=None) as stream_client:
            try:
                async with stream_client.stream("GET", target_url) as response:
                    if response.status_code != 200:
                        yield b""
                        return
                    async for chunk in response.aiter_bytes():
                        yield chunk
            except httpx.RequestError as exc:
                print(f"[Stream Error] Gagal terhubung ke ESP32-CAM di {target_url}: {exc}")
                yield b""

    return StreamingResponse(
        mjpeg_generator(),
        media_type="multipart/x-mixed-replace;boundary=frame",
        headers={
            "Cache-Control": "no-cache, private",
            "Pragma": "no-cache",
        }
    )

@app.get("/api/v1/camera/snapshot", tags=["Camera"])
async def get_camera_snapshot():
    """
    Mengambil satu frame still snapshot JPEG on-demand dari ESP32-CAM (Port 80 /capture).
    Dapat digunakan untuk:
      - Ditampilkan pada widget web dashboard
      - Diproses oleh pustaka Computer Vision (OpenCV, YOLO, deteksi kejernihan air)
      - Disimpan ke direktori lokal atau AWS S3 / MinIO
    """
    target_url = f"http://{ESP32_CAM_IP}:{ESP32_CAM_PORT}/capture"
    try:
        assert http_client is not None
        res = await http_client.get(target_url, timeout=5.0)
        if res.status_code != 200:
            raise HTTPException(
                status_code=res.status_code,
                detail=f"ESP32-CAM mengembalikan kode {res.status_code}"
            )
        return Response(content=res.content, media_type="image/jpeg")
    except httpx.RequestError as exc:
        raise HTTPException(
            status_code=status.HTTP_502_BAD_GATEWAY,
            detail=f"Tidak dapat menghubungi ESP32-CAM di {target_url}: {str(exc)}"
        )

@app.get("/api/v1/camera/status", tags=["Camera"])
async def get_camera_status():
    """Mengambil status telemetri perangkat kamera (heap, RSSI, PSRAM)."""
    target_url = f"http://{ESP32_CAM_IP}:{ESP32_CAM_PORT}/status"
    try:
        assert http_client is not None
        res = await http_client.get(target_url, timeout=3.0)
        return res.json()
    except httpx.RequestError as exc:
        raise HTTPException(
            status_code=status.HTTP_502_BAD_GATEWAY,
            detail=f"Gagal mengambil status ESP32-CAM: {str(exc)}"
        )

@app.post("/api/v1/camera/flash", tags=["Camera"])
async def control_camera_flash(cmd: FlashControlRequest):
    """Mengontrol lampu Flash LED (GPIO 4) pada AI Thinker ESP32-CAM."""
    state_param = "1" if cmd.state else "0"
    target_url = f"http://{ESP32_CAM_IP}:{ESP32_CAM_PORT}/flash?state={state_param}"
    try:
        assert http_client is not None
        res = await http_client.get(target_url, timeout=3.0)
        return res.json()
    except httpx.RequestError as exc:
        raise HTTPException(
            status_code=status.HTTP_502_BAD_GATEWAY,
            detail=f"Gagal mengontrol flash ESP32-CAM: {str(exc)}"
        )

if __name__ == "__main__":
    import uvicorn
    uvicorn.run("main:app", host="0.0.0.0", port=8000, reload=True)
