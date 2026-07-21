"""
TomAItrix – Local Inference Server for ESP32 Pesticide Rover
============================================================
Endpoints:
  GET  /                   – Health check
  POST /predict            – Accept a JPEG image, return disease prediction JSON
  GET  /stream             – Proxy MJPEG stream from ESP32-CAM
  GET  /events             – SSE stream of real-time prediction results
  POST /rover/command      – Forward movement/spray command to ESP32 control board
  GET  /status             – Current rover + inference status

Run with:
  uvicorn main:app --host 0.0.0.0 --port 8000 --reload
"""

import os
import io
import json
import time
import asyncio
import logging
import socket
import threading
from typing import AsyncGenerator

import numpy as np
from PIL import Image

try:
    import onnxruntime as ort
except ImportError:
    ort = None

import httpx
from fastapi import FastAPI, File, UploadFile, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import StreamingResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
log = logging.getLogger("tomatrix")

# ---------------------------------------------------------------------------
# App setup
# ---------------------------------------------------------------------------
app = FastAPI(title="TomAItrix Rover Inference Server", version="2.0.0")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# Serve the local dashboard from ../dashboard/
DASHBOARD_DIR = os.path.join(os.path.dirname(__file__), "..", "dashboard")
if os.path.isdir(DASHBOARD_DIR):
    app.mount("/dashboard", StaticFiles(directory=DASHBOARD_DIR, html=True), name="dashboard")

# ---------------------------------------------------------------------------
# Model + Labels
# ---------------------------------------------------------------------------
BASE_DIR    = os.path.dirname(os.path.abspath(__file__))
LABELS_PATH = os.path.join(BASE_DIR, "labels.json")
MODEL_PATH  = os.path.join(BASE_DIR, "tomato_disease_detector.onnx")

LABELS: dict = {}
if os.path.exists(LABELS_PATH):
    with open(LABELS_PATH, "r") as f:
        LABELS = json.load(f)

ort_session = None
model_loaded = False

if ort is not None and os.path.exists(MODEL_PATH):
    try:
        ort_session = ort.InferenceSession(MODEL_PATH)
        model_loaded = True
        log.info("ONNX model loaded: %s", MODEL_PATH)
    except Exception as e:
        log.error("Failed to load ONNX model: %s", e)
else:
    log.warning("ONNX model or runtime not available – prediction disabled")

# ---------------------------------------------------------------------------
# Disease advice dictionary
# ---------------------------------------------------------------------------
ADVICE_DICT = {
    "Tomato___Bacterial_spot": {
        "stage": "Early to Mid Stage",
        "cause": "Caused by Xanthomonas bacteria, favored by high moisture and warm temperatures.",
        "prevention": "Apply copper-based fungicides. Avoid overhead watering.",
        "severity": "medium",
    },
    "Tomato___Early_blight": {
        "stage": "Mid to Late Stage",
        "cause": "Caused by the fungus Alternaria solani. Spores spread during warm, wet weather.",
        "prevention": "Prune lower leaves, use crop rotation and protective fungicides.",
        "severity": "medium",
    },
    "Tomato___Late_blight": {
        "stage": "Rapid / Late Stage",
        "cause": "Caused by Phytophthora infestans. Destroys plants in days in cool wet conditions.",
        "prevention": "Destroy infected plants immediately. Apply systemic fungicides.",
        "severity": "high",
    },
    "Tomato___Leaf_Mold": {
        "stage": "Mid Stage",
        "cause": "Fungal infection from Passalora fulva in high-humidity environments.",
        "prevention": "Keep humidity below 85%. Space plants and prune heavily.",
        "severity": "medium",
    },
    "Tomato___Septoria_leaf_spot": {
        "stage": "Early to Mid Stage",
        "cause": "Septoria lycopersici attacks oldest leaves first via rain splash.",
        "prevention": "Mulch to prevent soil splashing. Remove affected leaves.",
        "severity": "medium",
    },
    "Tomato___Spider_mites Two-spotted_spider_mite": {
        "stage": "Any Stage",
        "cause": "Tiny arachnids sucking plant sap in hot, dry conditions.",
        "prevention": "Introduce predatory mites or spray horticultural oils.",
        "severity": "low",
    },
    "Tomato___Target_Spot": {
        "stage": "Mid Stage",
        "cause": "Corynespora cassiicola fungus under high humidity.",
        "prevention": "Improve airflow. Remove plant debris.",
        "severity": "medium",
    },
    "Tomato___Tomato_Yellow_Leaf_Curl_Virus": {
        "stage": "Systemic Viral Stage",
        "cause": "Begomovirus transmitted by silverleaf whitefly.",
        "prevention": "Use resistant varieties. Control whitefly with sticky traps.",
        "severity": "high",
    },
    "Tomato___Tomato_mosaic_virus": {
        "stage": "Systemic Viral Stage",
        "cause": "Highly contagious virus spread by contaminated hands and tools.",
        "prevention": "Wash hands before touching plants. Disinfect tools.",
        "severity": "high",
    },
    "Tomato___healthy": {
        "stage": "Healthy",
        "cause": "Excellent growing conditions and good genetics.",
        "prevention": "Maintain current routine. Consistent watering and fertilization.",
        "severity": "none",
    },
}

# ---------------------------------------------------------------------------
# Rover configuration (set your ESP32 IPs here)
# ---------------------------------------------------------------------------
ESP32_CAM_URL     = os.getenv("ESP32_CAM_URL",     "http://192.168.1.101")   # port 80 + 81
ESP32_CONTROL_IP  = os.getenv("ESP32_CONTROL_IP",  "192.168.1.102")
ESP32_CONTROL_PORT = int(os.getenv("ESP32_CONTROL_PORT", "4210"))

# ---------------------------------------------------------------------------
# Shared state (in-process; replace with Redis for multi-worker setups)
# ---------------------------------------------------------------------------
latest_result: dict = {
    "prediction": "Initialising...",
    "confidence": 0.0,
    "stage": "--",
    "cause": "--",
    "prevention": "--",
    "severity": "none",
    "timestamp": time.time(),
}
alert_log: list[dict] = []          # rolling log of disease alerts
sse_subscribers: list[asyncio.Queue] = []

# ---------------------------------------------------------------------------
# Inference helpers
# ---------------------------------------------------------------------------

def softmax(x: np.ndarray) -> np.ndarray:
    e_x = np.exp(x - np.max(x, axis=1, keepdims=True))
    return e_x / e_x.sum(axis=1, keepdims=True)


def preprocess(image: Image.Image) -> np.ndarray:
    image = image.resize((128, 128), Image.Resampling.BILINEAR).convert("RGB")
    arr = np.array(image, dtype=np.float32) / 255.0
    arr = np.transpose(arr, (2, 0, 1))
    return np.expand_dims(arr, axis=0)


def run_inference(image: Image.Image) -> dict:
    """Run ONNX inference and return a structured result dict."""
    if not model_loaded or ort_session is None:
        raise RuntimeError("Model not loaded")

    tensor = preprocess(image)
    input_name = ort_session.get_inputs()[0].name
    outputs    = ort_session.run(None, {input_name: tensor})
    probs      = softmax(outputs[0])

    idx        = int(np.argmax(probs, axis=1)[0])
    confidence = float(probs[0][idx])
    label_key  = str(idx)

    disease_name = LABELS.get(label_key, "Unknown Disease")
    clean_name   = (
        disease_name
        .replace("Tomato___", "")
        .replace("Two-spotted_spider_mite", "")
        .replace("_", " ")
        .strip()
    )

    advice = ADVICE_DICT.get(disease_name, {
        "stage": "Unknown", "cause": "N/A",
        "prevention": "Consult an agricultural expert.", "severity": "unknown",
    })

    return {
        "prediction":  clean_name,
        "disease_key": disease_name,
        "confidence":  confidence,
        "class_id":    idx,
        "stage":       advice["stage"],
        "cause":       advice["cause"],
        "prevention":  advice["prevention"],
        "severity":    advice["severity"],
        "timestamp":   time.time(),
    }


def broadcast_sse(data: dict):
    """Push result to all SSE subscribers."""
    global latest_result, alert_log
    latest_result = data

    if data.get("severity") not in ("none", "unknown"):
        alert_log.append(data)
        if len(alert_log) > 100:
            alert_log = alert_log[-100:]

    payload = "data: " + json.dumps(data) + "\n\n"
    dead = []
    for q in sse_subscribers:
        try:
            q.put_nowait(payload)
        except asyncio.QueueFull:
            dead.append(q)
    for q in dead:
        sse_subscribers.remove(q)

# ---------------------------------------------------------------------------
# Background stream-predict loop
# ---------------------------------------------------------------------------
_stream_task: asyncio.Task | None = None


async def stream_predict_loop():
    """Continuously pull frames from ESP32-CAM and run inference."""
    log.info("[STREAM-PREDICT] Starting background loop → %s", ESP32_CAM_URL)
    async with httpx.AsyncClient(timeout=10) as client:
        while True:
            try:
                capture_url = f"{ESP32_CAM_URL}/capture"
                resp = await client.get(capture_url)
                if resp.status_code == 200:
                    image = Image.open(io.BytesIO(resp.content))
                    result = run_inference(image)
                    broadcast_sse(result)
                    log.info("[PREDICT] %s %.1f%%", result["prediction"], result["confidence"] * 100)
                else:
                    log.warning("[STREAM-PREDICT] Bad status %d from ESP32", resp.status_code)
            except Exception as e:
                log.warning("[STREAM-PREDICT] %s", e)
            await asyncio.sleep(2)   # poll every 2 seconds


@app.on_event("startup")
async def startup_event():
    global _stream_task
    _stream_task = asyncio.create_task(stream_predict_loop())
    log.info("Background stream-predict loop started")


# ---------------------------------------------------------------------------
# Routes
# ---------------------------------------------------------------------------

@app.get("/")
def health():
    return {
        "status": "ok",
        "model_loaded": model_loaded,
        "esp32_cam": ESP32_CAM_URL,
        "esp32_control": f"{ESP32_CONTROL_IP}:{ESP32_CONTROL_PORT}",
        "dashboard": "/dashboard",
    }


@app.post("/predict")
async def predict_image(file: UploadFile = File(...)):
    """Accept a JPEG upload and return disease prediction."""
    if not model_loaded:
        raise HTTPException(503, "Model not loaded")
    try:
        data  = await file.read()
        image = Image.open(io.BytesIO(data)).convert("RGB")
        result = run_inference(image)
        broadcast_sse(result)
        return result
    except Exception as e:
        raise HTTPException(500, str(e))


@app.get("/stream")
async def proxy_stream():
    """Proxy the ESP32-CAM MJPEG stream to the dashboard."""
    stream_url = f"{ESP32_CAM_URL}:81/stream"

    async def generator():
        async with httpx.AsyncClient(timeout=None) as client:
            async with client.stream("GET", stream_url) as resp:
                async for chunk in resp.aiter_bytes(4096):
                    yield chunk

    return StreamingResponse(
        generator(),
        media_type="multipart/x-mixed-replace; boundary=123456789000000000000987654321",
        headers={"Access-Control-Allow-Origin": "*"},
    )


@app.get("/events")
async def sse_events():
    """Server-Sent Events stream of real-time disease predictions."""
    queue: asyncio.Queue = asyncio.Queue(maxsize=20)
    sse_subscribers.append(queue)

    async def event_generator() -> AsyncGenerator[str, None]:
        # Send current state immediately on connect
        yield "data: " + json.dumps(latest_result) + "\n\n"
        try:
            while True:
                try:
                    msg = await asyncio.wait_for(queue.get(), timeout=30)
                    yield msg
                except asyncio.TimeoutError:
                    yield ": heartbeat\n\n"   # keep connection alive
        except asyncio.CancelledError:
            pass
        finally:
            if queue in sse_subscribers:
                sse_subscribers.remove(queue)

    return StreamingResponse(
        event_generator(),
        media_type="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "X-Accel-Buffering": "no",
            "Access-Control-Allow-Origin": "*",
        },
    )


class RoverCommand(BaseModel):
    command: str   # FORWARD | BACKWARD | LEFT | RIGHT | STOP | SPRAY | STOP_SPRAY | SPEED:<n>


@app.post("/rover/command")
def rover_command(body: RoverCommand):
    """Send a UDP command to the ESP32 rover control board."""
    cmd = body.command.upper().strip()
    valid = {"FORWARD", "BACKWARD", "LEFT", "RIGHT", "STOP", "SPRAY", "STOP_SPRAY"}
    if cmd not in valid and not cmd.startswith("SPEED:"):
        raise HTTPException(400, f"Unknown command: {cmd}")

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(1)
        sock.sendto(cmd.encode(), (ESP32_CONTROL_IP, ESP32_CONTROL_PORT))
        sock.close()
        log.info("[ROVER CMD] %s → %s:%d", cmd, ESP32_CONTROL_IP, ESP32_CONTROL_PORT)
        return {"sent": cmd, "to": f"{ESP32_CONTROL_IP}:{ESP32_CONTROL_PORT}"}
    except Exception as e:
        raise HTTPException(503, f"Could not reach rover: {e}")


@app.get("/status")
def status():
    return {
        "model_loaded": model_loaded,
        "latest_prediction": latest_result,
        "alert_count": len(alert_log),
        "recent_alerts": alert_log[-5:],
        "sse_subscribers": len(sse_subscribers),
        "esp32_cam_url": ESP32_CAM_URL,
        "esp32_control": f"{ESP32_CONTROL_IP}:{ESP32_CONTROL_PORT}",
    }


@app.get("/alerts")
def get_alerts(limit: int = 20):
    return {"alerts": alert_log[-limit:]}
