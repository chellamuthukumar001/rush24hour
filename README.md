# TomAItrix – ESP32 Pesticide Rover 🌿🤖

Real-time tomato disease detection rover using ESP32-CAM video streaming and on-device ONNX inference.

---

## System Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         TOMATO FARMLAND                         │
│                                                                 │
│  ┌─────────────┐        ┌──────────────┐                        │
│  │ ESP32-CAM   │──WiFi──▶ esp32/rover_ │                        │
│  │ (AI-Thinker)│        │ camera.ino   │                        │
│  │  OV2640 cam │        │              │                        │
│  │  MJPEG @81  │        └──────────────┘                        │
│  └─────────────┘                                               │
│         │  JPEG frames (POST /predict)                          │
│         ▼                                                       │
│  ┌─────────────────────┐    UDP commands ┌───────────────────┐  │
│  │   PC / Laptop       │────────────────▶│ ESP32 DevKit      │  │
│  │   server/main.py    │                 │ rover_control.ino │  │
│  │   FastAPI + ONNX    │                 │ L298N motors      │  │
│  │   Port 8000         │                 │ Relay pump        │  │
│  └─────────────────────┘                 └───────────────────┘  │
│         │  SSE / MJPEG proxy                                    │
│         ▼                                                       │
│  ┌─────────────────────┐                                        │
│  │  dashboard/         │                                        │
│  │  index.html         │                                        │
│  │  (open in browser)  │                                        │
│  └─────────────────────┘                                        │
└─────────────────────────────────────────────────────────────────┘
```

---

## Hardware Requirements

| Component | Details |
|-----------|---------|
| **ESP32-CAM** (AI-Thinker) | Camera module with OV2640 |
| **ESP32 DevKit V1** | Motor + relay controller |
| **L298N Motor Driver** | Dual H-bridge for 4 DC motors |
| **4× DC gear motors** | 3V–6V TT motors with wheels |
| **5V Relay Module** | For pesticide pump (active-low) |
| **Peristaltic/submersible pump** | 5V/12V pesticide pump |
| **LiPo / 18650 battery pack** | 7.4V–12V for motors; 5V for ESP32 |
| **Chassis** | 4WD rover platform |

---

## Wiring Diagrams

### ESP32-CAM (rover_camera)
The AI-Thinker ESP32-CAM uses fixed pins for the OV2640 camera — **do not rewire camera pins**.

| Signal | ESP32-CAM Pin |
|--------|--------------|
| PWDN   | GPIO 32       |
| XCLK   | GPIO 0        |
| SIOD   | GPIO 26       |
| SIOC   | GPIO 27       |
| Flash LED | GPIO 4     |

> ⚠️ GPIO 0 must be pulled LOW during flash, then HIGH (or floating) for normal boot.

### ESP32 DevKit (rover_control) → L298N

| L298N Pin | ESP32 GPIO | Function |
|-----------|-----------|----------|
| IN1       | GPIO 25   | Left motors – forward |
| IN2       | GPIO 26   | Left motors – backward |
| IN3       | GPIO 27   | Right motors – forward |
| IN4       | GPIO 14   | Right motors – backward |
| ENA       | GPIO 32   | Left speed (PWM) |
| ENB       | GPIO 33   | Right speed (PWM) |
| GND       | GND       | Common ground |

### Relay (Pump)

| Relay Pin | ESP32 GPIO |
|-----------|-----------|
| Signal    | GPIO 13   |
| VCC       | 5V        |
| GND       | GND       |

> The relay is **active-LOW**: `LOW` = pump ON, `HIGH` = pump OFF.

---

## Project Structure

```
tomatrix/
├── esp32/
│   ├── rover_camera/
│   │   └── rover_camera.ino    ← Flash to ESP32-CAM
│   └── rover_control/
│       └── rover_control.ino   ← Flash to ESP32 DevKit
├── server/
│   ├── main.py                 ← FastAPI inference server
│   ├── labels.json             ← Disease class labels
│   ├── requirements.txt
│   ├── tomato_disease_detector.onnx
│   └── tomato_disease_detector.onnx.data
├── dashboard/
│   └── index.html              ← Open in browser (no build step)
├── backend/                    ← Model training (kept for retraining)
│   ├── train_model.py
│   └── export_to_onnx.py
└── README.md
```

---

## Setup Guide

### Step 1 – Configure WiFi

Edit **both** Arduino sketches and set your WiFi credentials:

```cpp
// In rover_camera.ino and rover_control.ino
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
```

Also set your PC's local IP in `rover_camera.ino`:
```cpp
const char* SERVER_HOST = "http://192.168.1.100:8000";  // your PC IP
```

### Step 2 – Flash Firmware

**Arduino IDE setup:**
1. Install esp32 board package: File → Preferences → Board Manager URL:
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
2. Install via **Tools → Board Manager → "esp32 by Espressif"**

**Flash ESP32-CAM:**
- Board: `AI Thinker ESP32-CAM`
- Partition: `Huge APP (3MB No OTA)`
- PSRAM: `Enabled`
- Pull GPIO 0 to GND while pressing reset to enter flash mode

**Flash ESP32 DevKit:**
- Board: `ESP32 Dev Module`
- No special wiring needed

### Step 3 – Find ESP32 IP Addresses

After flashing, open Serial Monitor (115200 baud) for each board and note the IP printed on boot:
```
[WIFI] Connected! IP: 192.168.1.101   ← CAM board
[WIFI] Connected! IP: 192.168.1.102   ← Control board
```

### Step 4 – Configure the Server

Set environment variables before starting the server:
```powershell
$env:ESP32_CAM_URL      = "http://192.168.1.101"
$env:ESP32_CONTROL_IP   = "192.168.1.102"
$env:ESP32_CONTROL_PORT = "4210"
```

### Step 5 – Install Server Dependencies

```powershell
cd server
pip install -r requirements.txt
```

### Step 6 – Start the Inference Server

```powershell
cd server
uvicorn main:app --host 0.0.0.0 --port 8000 --reload
```

### Step 7 – Open the Dashboard

Simply open `dashboard/index.html` in your browser:
```
File → Open → C:\Users\annam\Documents\tomatrix\dashboard\index.html
```

Or the server also serves it at: `http://localhost:8000/dashboard`

Set the **Server IP** field to `http://localhost:8000` (or your PC IP if accessing from phone) and click **Connect**.

---

## API Reference

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Health check + ESP32 info |
| `/predict` | POST | Upload JPEG → disease prediction JSON |
| `/stream` | GET | MJPEG stream proxy from ESP32-CAM |
| `/events` | GET | SSE stream of real-time predictions |
| `/rover/command` | POST | Send UDP command to rover |
| `/status` | GET | Server + rover status |
| `/alerts` | GET | Alert history (last N) |
| `/dashboard` | GET | Serves the local dashboard |

### Rover Commands (POST /rover/command)

```json
{ "command": "FORWARD" }
{ "command": "BACKWARD" }
{ "command": "LEFT" }
{ "command": "RIGHT" }
{ "command": "STOP" }
{ "command": "SPRAY" }
{ "command": "STOP_SPRAY" }
{ "command": "SPEED:180" }
```

### Dashboard Keyboard Shortcuts

| Key | Action |
|-----|--------|
| `↑` | Forward |
| `↓` | Backward |
| `←` | Left |
| `→` | Right |
| `Space` / `S` | Stop |

---

## Disease Classes (10 Classes)

| Class | Disease |
|-------|---------|
| 0 | Bacterial Spot |
| 1 | Early Blight |
| 2 | Late Blight |
| 3 | Leaf Mold |
| 4 | Septoria Leaf Spot |
| 5 | Spider Mites |
| 6 | Target Spot |
| 7 | Yellow Leaf Curl Virus |
| 8 | Mosaic Virus |
| 9 | Healthy ✅ |

---

## Retraining the Model

To retrain on new data:
```powershell
cd backend
python train_model.py        # Train PyTorch model
python export_to_onnx.py     # Export to ONNX
# Then copy the .onnx files to server/
```

---

## Troubleshooting

| Problem | Solution |
|---------|---------|
| ESP32-CAM no stream | Check GPIO 0 is HIGH on boot; check WiFi credentials |
| Server can't reach CAM | Verify both devices are on the same WiFi network |
| Rover not responding | Check UDP port 4210 not blocked by firewall |
| Model not loaded | Ensure `.onnx` and `.onnx.data` are both in `server/` |
| Dashboard shows "Waiting for stream" | Click Connect after entering server IP |
