/**
 * =============================================================
 *  TomAItrix – ESP32-CAM Rover Camera Firmware
 *  Board: AI-Thinker ESP32-CAM
 * =============================================================
 *  Features:
 *   - Connects to WiFi and starts an MJPEG stream on port 80
 *   - Stream endpoint: http://<ESP32_IP>/stream
 *   - Snapshot endpoint: http://<ESP32_IP>/capture
 *   - Periodically POSTs frames to the inference server
 *     so disease prediction works even without the dashboard open
 *  
 *  Arduino IDE Setup:
 *   Board:        "AI Thinker ESP32-CAM"  (esp32 by Espressif v2.x)
 *   Partition:    Huge APP (3MB No OTA)
 *   Flash Freq:   80MHz
 *   Upload Speed: 115200
 *   PSRAM:        Enabled
 * =============================================================
 */

#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include <HTTPClient.h>

// -----------------------------------------------------------
// WiFi Credentials – change these before flashing
// -----------------------------------------------------------
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// -----------------------------------------------------------
// Inference server address (PC running server/main.py)
// -----------------------------------------------------------
const char* SERVER_HOST = "http://192.168.1.100:8000";  // change to your PC IP

// -----------------------------------------------------------
// AI-Thinker ESP32-CAM pin map
// -----------------------------------------------------------
#define CAM_PIN_PWDN    32
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK     0
#define CAM_PIN_SIOD    26
#define CAM_PIN_SIOC    27
#define CAM_PIN_D7      35
#define CAM_PIN_D6      34
#define CAM_PIN_D5      39
#define CAM_PIN_D4      36
#define CAM_PIN_D3      21
#define CAM_PIN_D2      19
#define CAM_PIN_D1      18
#define CAM_PIN_D0       5
#define CAM_PIN_VSYNC   25
#define CAM_PIN_HREF    23
#define CAM_PIN_PCLK    22
#define LED_BUILTIN      4   // onboard flash LED (active HIGH)

// -----------------------------------------------------------
// Global state
// -----------------------------------------------------------
httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

unsigned long lastPredictMs = 0;
const unsigned long PREDICT_INTERVAL_MS = 2000;  // send frame every 2s

String latestPrediction = "Scanning...";
float  latestConfidence  = 0.0f;

// -----------------------------------------------------------
// Camera initialisation
// -----------------------------------------------------------
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = CAM_PIN_D0;
  config.pin_d1       = CAM_PIN_D1;
  config.pin_d2       = CAM_PIN_D2;
  config.pin_d3       = CAM_PIN_D3;
  config.pin_d4       = CAM_PIN_D4;
  config.pin_d5       = CAM_PIN_D5;
  config.pin_d6       = CAM_PIN_D6;
  config.pin_d7       = CAM_PIN_D7;
  config.pin_xclk     = CAM_PIN_XCLK;
  config.pin_pclk     = CAM_PIN_PCLK;
  config.pin_vsync    = CAM_PIN_VSYNC;
  config.pin_href     = CAM_PIN_HREF;
  config.pin_sscb_sda = CAM_PIN_SIOD;
  config.pin_sscb_scl = CAM_PIN_SIOC;
  config.pin_pwdn     = CAM_PIN_PWDN;
  config.pin_reset    = CAM_PIN_RESET;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size   = FRAMESIZE_VGA;   // 640x480 – smooth stream
    config.jpeg_quality = 12;
    config.fb_count     = 2;
  } else {
    config.frame_size   = FRAMESIZE_QVGA;  // 320x240 fallback
    config.jpeg_quality = 15;
    config.fb_count     = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] Init failed: 0x%x\n", err);
    return false;
  }

  // Improve image quality settings
  sensor_t* s = esp_camera_sensor_get();
  s->set_brightness(s, 1);
  s->set_saturation(s, -1);
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_exposure_ctrl(s, 1);

  Serial.println("[CAM] Initialised OK");
  return true;
}

// -----------------------------------------------------------
// MJPEG stream handler
// -----------------------------------------------------------
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t* req) {
  camera_fb_t* fb  = NULL;
  esp_err_t    res = ESP_OK;
  char         part_buf[64];

  res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) { Serial.println("[STREAM] Frame capture failed"); res = ESP_FAIL; break; }

    size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, part_buf, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);

    esp_camera_fb_return(fb);

    if (res != ESP_OK) break;
    vTaskDelay(30 / portTICK_PERIOD_MS);   // ~30fps cap
  }
  return res;
}

// -----------------------------------------------------------
// Capture (single JPEG) handler
// -----------------------------------------------------------
static esp_err_t capture_handler(httpd_req_t* req) {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return ESP_OK;
}

// -----------------------------------------------------------
// Status handler – returns JSON with IP and latest prediction
// -----------------------------------------------------------
static esp_err_t status_handler(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  String json = "{\"ip\":\"" + WiFi.localIP().toString() +
                "\",\"rssi\":" + String(WiFi.RSSI()) +
                ",\"prediction\":\"" + latestPrediction +
                "\",\"confidence\":" + String(latestConfidence, 2) + "}";
  httpd_resp_sendstr(req, json.c_str());
  return ESP_OK;
}

// -----------------------------------------------------------
// Start stream server (port 81) and control server (port 80)
// -----------------------------------------------------------
void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 8;

  // --- stream server on port 81 ---
  config.server_port = 81;
  httpd_uri_t stream_uri = { "/stream", HTTP_GET, stream_handler, NULL };
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
    Serial.println("[HTTP] Stream server started on port 81");
  }

  // --- control server on port 80 ---
  config.server_port = 80;
  httpd_uri_t capture_uri = { "/capture", HTTP_GET, capture_handler, NULL };
  httpd_uri_t status_uri  = { "/status",  HTTP_GET, status_handler,  NULL };
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    Serial.println("[HTTP] Control server started on port 80");
  }
}

// -----------------------------------------------------------
// Send a JPEG frame to the inference server
// -----------------------------------------------------------
void sendFrameToServer() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) return;

  HTTPClient http;
  String url = String(SERVER_HOST) + "/predict";
  http.begin(url);
  http.addHeader("Content-Type", "image/jpeg");
  http.setTimeout(3000);

  int code = http.POST(fb->buf, fb->len);
  esp_camera_fb_return(fb);

  if (code == 200) {
    String body = http.getString();
    // Very lightweight JSON parse (no library needed)
    int pi = body.indexOf("\"prediction\":\"");
    int ci = body.indexOf("\"confidence\":");
    if (pi >= 0) {
      int start = pi + 14;
      int end   = body.indexOf("\"", start);
      latestPrediction = body.substring(start, end);
    }
    if (ci >= 0) {
      int start = ci + 13;
      int end   = body.indexOf(",", start);
      if (end < 0) end = body.indexOf("}", start);
      latestConfidence = body.substring(start, end).toFloat();
    }
    Serial.printf("[PREDICT] %s (%.1f%%)\n", latestPrediction.c_str(), latestConfidence * 100);
  } else {
    Serial.printf("[PREDICT] HTTP error: %d\n", code);
  }
  http.end();
}

// -----------------------------------------------------------
// Setup
// -----------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  Serial.println("\n\n=== TomAItrix ESP32-CAM Rover ===");

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  if (!initCamera()) {
    Serial.println("[FATAL] Camera init failed – halting");
    while (true) { delay(1000); }
  }

  // Connect to WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WIFI] Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.printf("[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[WIFI] RSSI: %d dBm\n", WiFi.RSSI());

  startCameraServer();

  Serial.println("\n--- Endpoints ---");
  Serial.printf("  MJPEG Stream : http://%s:81/stream\n",  WiFi.localIP().toString().c_str());
  Serial.printf("  Capture JPEG : http://%s/capture\n",    WiFi.localIP().toString().c_str());
  Serial.printf("  Status JSON  : http://%s/status\n",     WiFi.localIP().toString().c_str());
  Serial.println("-----------------\n");

  // Flash LED to indicate ready
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_BUILTIN, HIGH); delay(100);
    digitalWrite(LED_BUILTIN, LOW);  delay(100);
  }
}

// -----------------------------------------------------------
// Loop – periodically push frames to inference server
// -----------------------------------------------------------
void loop() {
  unsigned long now = millis();
  if (now - lastPredictMs >= PREDICT_INTERVAL_MS) {
    lastPredictMs = now;
    if (WiFi.status() == WL_CONNECTED) {
      sendFrameToServer();
    } else {
      Serial.println("[WIFI] Reconnecting...");
      WiFi.reconnect();
    }
  }
  delay(10);
}
