/**
 * =============================================================
 *  TomAItrix – ESP32 Rover Control Firmware
 *  Board: ESP32 DevKit V1 (or any generic ESP32)
 * =============================================================
 *  Controls:
 *   - 4-wheel drive via L298N dual H-bridge motor driver
 *   - Pesticide spray pump via a 5V relay module
 *   - Receives commands over WiFi UDP from the dashboard
 *   - Also accepts Serial commands for wired testing
 *
 *  L298N Wiring:
 *   IN1 → GPIO 25    IN2 → GPIO 26   (Left motors)
 *   IN3 → GPIO 27    IN4 → GPIO 14   (Right motors)
 *   ENA → GPIO 32    ENB → GPIO 33   (PWM speed control)
 *
 *  Relay (Pump) Wiring:
 *   Signal → GPIO 13  (LOW = pump ON for active-low relay)
 *
 *  Arduino IDE Setup:
 *   Board:  "ESP32 Dev Module"
 *   Upload Speed: 115200
 *   Flash Size: 4MB
 * =============================================================
 */

#include <WiFi.h>
#include <WiFiUdp.h>

// -----------------------------------------------------------
// WiFi Credentials – must match rover_camera settings
// -----------------------------------------------------------
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// UDP listener port (dashboard sends commands here)
const uint16_t UDP_PORT = 4210;

// -----------------------------------------------------------
// Motor driver pins (L298N)
// -----------------------------------------------------------
#define IN1  25   // Left motor forward
#define IN2  26   // Left motor backward
#define IN3  27   // Right motor forward
#define IN4  14   // Right motor backward
#define ENA  32   // Left  motor PWM speed
#define ENB  33   // Right motor PWM speed

// -----------------------------------------------------------
// Pump relay pin
// -----------------------------------------------------------
#define RELAY_PUMP 13  // LOW = ON (active-low relay)

// -----------------------------------------------------------
// LEDC PWM settings for motor speed
// -----------------------------------------------------------
#define PWM_FREQ    5000
#define PWM_RES     8       // 8-bit (0-255)
#define CH_LEFT     0
#define CH_RIGHT    1

uint8_t motorSpeed = 180;   // default speed (0–255)

// -----------------------------------------------------------
// UDP
// -----------------------------------------------------------
WiFiUDP udp;
char packetBuf[64];

// -----------------------------------------------------------
// Motor helpers
// -----------------------------------------------------------
void setMotorLeft(int dir, uint8_t spd) {
  // dir: 1=forward, -1=backward, 0=stop
  ledcWrite(CH_LEFT, (dir != 0) ? spd : 0);
  digitalWrite(IN1, dir ==  1 ? HIGH : LOW);
  digitalWrite(IN2, dir == -1 ? HIGH : LOW);
}

void setMotorRight(int dir, uint8_t spd) {
  ledcWrite(CH_RIGHT, (dir != 0) ? spd : 0);
  digitalWrite(IN3, dir ==  1 ? HIGH : LOW);
  digitalWrite(IN4, dir == -1 ? HIGH : LOW);
}

void roverStop()     { setMotorLeft(0, 0);  setMotorRight(0, 0);  }
void roverForward()  { setMotorLeft(1, motorSpeed);  setMotorRight(1, motorSpeed);  }
void roverBackward() { setMotorLeft(-1, motorSpeed); setMotorRight(-1, motorSpeed); }
void roverLeft()     { setMotorLeft(-1, motorSpeed); setMotorRight(1, motorSpeed);  }
void roverRight()    { setMotorLeft(1, motorSpeed);  setMotorRight(-1, motorSpeed); }

void pumpOn()  { digitalWrite(RELAY_PUMP, LOW);  Serial.println("[PUMP] ON");  }
void pumpOff() { digitalWrite(RELAY_PUMP, HIGH); Serial.println("[PUMP] OFF"); }

// -----------------------------------------------------------
// Command dispatcher
// -----------------------------------------------------------
void handleCommand(const String& cmd) {
  String c = cmd;
  c.trim();
  c.toUpperCase();

  Serial.printf("[CMD] %s\n", c.c_str());

  if      (c == "FORWARD")    roverForward();
  else if (c == "BACKWARD")   roverBackward();
  else if (c == "LEFT")       roverLeft();
  else if (c == "RIGHT")      roverRight();
  else if (c == "STOP")       roverStop();
  else if (c == "SPRAY")      pumpOn();
  else if (c == "STOP_SPRAY") pumpOff();
  else if (c.startsWith("SPEED:")) {
    uint8_t spd = (uint8_t)c.substring(6).toInt();
    motorSpeed = constrain(spd, 50, 255);
    Serial.printf("[SPEED] Set to %d\n", motorSpeed);
  } else {
    Serial.printf("[CMD] Unknown: %s\n", c.c_str());
  }
}

// -----------------------------------------------------------
// Setup
// -----------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== TomAItrix Rover Control ===");

  // Motor pins
  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);

  // PWM channels for speed control
  ledcSetup(CH_LEFT,  PWM_FREQ, PWM_RES);
  ledcSetup(CH_RIGHT, PWM_FREQ, PWM_RES);
  ledcAttachPin(ENA, CH_LEFT);
  ledcAttachPin(ENB, CH_RIGHT);

  // Pump relay (high = off by default)
  pinMode(RELAY_PUMP, OUTPUT);
  digitalWrite(RELAY_PUMP, HIGH);

  // Safety: motors off on boot
  roverStop();

  // WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WIFI] Connecting");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println();
  Serial.printf("[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());

  udp.begin(UDP_PORT);
  Serial.printf("[UDP]  Listening on port %d\n", UDP_PORT);
  Serial.println("Ready! Send commands: FORWARD | BACKWARD | LEFT | RIGHT | STOP | SPRAY | STOP_SPRAY | SPEED:<0-255>");
}

// -----------------------------------------------------------
// Loop
// -----------------------------------------------------------
void loop() {
  // --- UDP commands (from dashboard) ---
  int pktSize = udp.parsePacket();
  if (pktSize) {
    int len = udp.read(packetBuf, sizeof(packetBuf) - 1);
    packetBuf[len] = '\0';
    handleCommand(String(packetBuf));

    // Acknowledge
    udp.beginPacket(udp.remoteIP(), udp.remotePort());
    udp.print("ACK");
    udp.endPacket();
  }

  // --- Serial commands (for wired testing) ---
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    handleCommand(line);
  }

  delay(10);
}
