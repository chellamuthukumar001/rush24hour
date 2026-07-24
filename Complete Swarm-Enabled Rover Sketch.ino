#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#include <esp_now.h>

/* ================= SWARM CONFIGURATION ================= */
#define ROVER_ID 1             // CHANGE THIS for each rover (e.g., 1, 2, 3, etc.)
#define MAX_ROVERS 5

// Peer broadcast address (sends to all rovers in range)
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* ================= WIFI CONFIGURATION ================= */
const char* ssid = "Redmi Note 12";
const char* password = "redmi12pro";

WebServer server(80);

/* ================= MOTOR PINS (L298N) ================= */
#define IN1 6
#define IN2 7
#define IN3 8
#define IN4 9
#define ENA 14
#define ENB 12

/* ================= SERVO PINS ================= */
#define SERVO_UP1   18
#define SERVO_UP2   13
#define SERVO_LEFT  21

/* ================= PUMP RELAY PINS ================= */
#define RELAY1 2
#define RELAY2 15   // ACTIVE-LOW

/* ================= ULTRASONIC SENSOR PINS ================= */
#define TRIG 5
#define ECHO 4

Servo servoUp1, servoUp2, servoLeft;

int upPos = 90;
int leftPos = 90;

/* ================= SYSTEM STATE & GRID MAPPING ================= */
long distanceCm = 999;          // Ultrasonic sensor distance
unsigned long lastMeasureTime = 0;
bool isMovingForward = false;   // Movement state

// Virtual Grid Coordinates (10x10 field representation)
int currentX = 5;
int currentY = 5;
int heading = 0;                // 0=North, 1=East, 2=South, 3=West
unsigned long lastMoveTime = 0;

// Shared Field Coverage Map (true = sprayed)
bool fieldMap[10][10];

// Struct for ESP-NOW Swarm Packets
struct SwarmPacket {
  uint8_t id;
  int x;
  int y;
  bool isSpraying;
  uint8_t heading;
  unsigned long timestamp;
};

// Tracks state of other active rovers in the swarm
struct RoverState {
  int x;
  int y;
  bool isSpraying;
  uint8_t heading;
  bool active;
  unsigned long lastSeen;
};
RoverState otherRovers[MAX_ROVERS + 1];

unsigned long lastBroadcastTime = 0;

/* ================= ESP-NOW HANDLERS ================= */

// Handles receiving swarm packets from other rovers
void onDataRecv(const uint8_t * mac_addr, const uint8_t *incomingData, int len) {
  if (len == sizeof(SwarmPacket)) {
    SwarmPacket packet;
    memcpy(&packet, incomingData, sizeof(SwarmPacket));
    
    // Ignore packet if it's from ourselves
    if (packet.id == ROVER_ID) return;

    // Log the data in our swarm registry
    if (packet.id <= MAX_ROVERS) {
      otherRovers[packet.id].x = packet.x;
      otherRovers[packet.id].y = packet.y;
      otherRovers[packet.id].isSpraying = packet.isSpraying;
      otherRovers[packet.id].heading = packet.heading;
      otherRovers[packet.id].active = true;
      otherRovers[packet.id].lastSeen = millis();

      // If the peer rover is spraying, mark their position in our local coverage map
      if (packet.isSpraying && packet.x >= 0 && packet.x < 10 && packet.y >= 0 && packet.y < 10) {
        fieldMap[packet.x][packet.y] = true;
      }
    }
  }
}

// Callback when data is sent
void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  // Optional diagnostics
}

// Broadcasts our status to the swarm
void broadcastSwarmStatus() {
  if (millis() - lastBroadcastTime >= 300) { // Broadcast every 300ms
    SwarmPacket packet;
    packet.id = ROVER_ID;
    packet.x = currentX;
    packet.y = currentY;
    packet.isSpraying = (digitalRead(RELAY1) == LOW); // Active-low status
    packet.heading = heading;
    packet.timestamp = millis();

    esp_now_send(broadcastAddress, (uint8_t *) &packet, sizeof(packet));
    lastBroadcastTime = millis();
  }
}

/* ================= POSITION ESTIMATION (DEAD RECKONING) ================= */
void updateDeadReckoning() {
  if (isMovingForward) {
    if (millis() - lastMoveTime >= 1000) { // Assume moving 1 grid unit every 1 second
      if (heading == 0 && currentY < 9) currentY++;
      else if (heading == 1 && currentX < 9) currentX++;
      else if (heading == 2 && currentY > 0) currentY--;
      else if (heading == 3 && currentX > 0) currentX--;
      
      // If we are currently spraying, record it in our coverage map
      if (digitalRead(RELAY1) == LOW) {
        fieldMap[currentX][currentY] = true;
      }
      
      lastMoveTime = millis();
    }
  }
}

/* ================= SWARM COLLISION AVOIDANCE ================= */
// Check if our path overlaps or is too close to another active rover
bool isSwarmCollisionImminent() {
  for (int i = 1; i <= MAX_ROVERS; i++) {
    if (i != ROVER_ID && otherRovers[i].active && (millis() - otherRovers[i].lastSeen < 4000)) {
      // Calculate distance between this rover and peer i
      float dist = sqrt(pow(currentX - otherRovers[i].x, 2) + pow(currentY - otherRovers[i].y, 2));
      
      if (dist < 1.5) { // If less than 1.5 units away
        // Priority rule: Lower Rover ID has priority.
        // If our ID is higher, we must halt to let the lower ID rover pass.
        if (ROVER_ID > i) {
          return true;
        }
      }
    }
  }
  return false;
}

/* ================= ULTRASONIC SENSOR ================= */
long readSensorDistance() {
  digitalWrite(TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG, LOW);

  long duration = pulseIn(ECHO, HIGH, 30000); 
  if (duration == 0) {
    return -1;
  }
  return duration * 0.034 / 2;
}

void updateDistance() {
  if (millis() - lastMeasureTime >= 150) {
    long dist = readSensorDistance();
    if (dist != -1) {
      distanceCm = dist;
    } else {
      distanceCm = 999; 
    }
    lastMeasureTime = millis();
  }
}

/* ================= MOTOR CONTROL ================= */
void stopCar() {
  isMovingForward = false;
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
}

void forward() {
  if (distanceCm < 20) { 
    Serial.println("[Safety Alert] Forward blocked! Obstacle too close.");
    stopCar(); 
    return; 
  }
  if (isSwarmCollisionImminent()) {
    Serial.println("[Swarm Alert] Forward blocked! Yielding to higher priority rover.");
    stopCar();
    return;
  }
  isMovingForward = true;
  lastMoveTime = millis();
  digitalWrite(IN1, LOW); digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW); digitalWrite(IN4, HIGH);
}

void back() {
  isMovingForward = false;
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
}

void left() {
  isMovingForward = false;
  heading = (heading + 3) % 4; // Turn 90 deg counter-clockwise
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);
  delay(300); // Pulse motors for quick turn pivot
  stopCar();
}

void right() {
  isMovingForward = false;
  heading = (heading + 1) % 4; // Turn 90 deg clockwise
  digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
  delay(300);
  stopCar();
}

/* ================= SERVO CONTROL ================= */
void applyUpServo() {
  servoUp1.write(upPos);
  servoUp2.write(180 - upPos);
}

void sprayUp()   { upPos = min(upPos + 10, 170); applyUpServo(); }
void sprayDown() { upPos = max(upPos - 10, 10);  applyUpServo(); }

void sprayLeft()  { leftPos = min(leftPos + 10, 170); servoLeft.write(leftPos); }
void sprayRight() { leftPos = max(leftPos - 10, 10);  servoLeft.write(leftPos); }

void sprayCenter() {
  upPos = 90;
  leftPos = 90;
  applyUpServo();
  servoLeft.write(leftPos);
}

/* ================= PUMP / RELAY CONTROL ================= */
void sprayON() {
  // Check if our current grid cell is already sprayed by another swarm peer
  if (fieldMap[currentX][currentY]) {
    Serial.println("[Swarm Control] Spray disabled: Zone already covered by another peer!");
    return;
  }
  digitalWrite(RELAY1, LOW);
  digitalWrite(RELAY2, LOW);
  fieldMap[currentX][currentY] = true;
}

void sprayOFF() {
  digitalWrite(RELAY1, HIGH);
  digitalWrite(RELAY2, HIGH);
}

bool isSprayON() {
  return digitalRead(RELAY1) == LOW;
}

/* ================= WEB PAGE DASHBOARD ================= */
const char page[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body{font-family:Arial;background:#111;color:#0f0;text-align:center}
button{padding:15px;margin:8px;font-size:18px}
.grid{display:grid;grid-template-columns:repeat(10,30px);grid-gap:2px;justify-content:center;margin:15px auto}
.cell{width:30px;height:30px;background:#333;border:1px solid #444;display:flex;align-items:center;justify-content:center;font-size:10px;color:#fff}
.sprayed{background:#2e7d32}
.self{background:#d32f2f;font-weight:bold}
.peer{background:#1976d2;font-weight:bold}
</style>
</head>
<body>

<h2>ESP32 Rover Swarm Dashboard</h2>

<h3>Field Grid Map</h3>
<div id="grid" class="grid"></div>

<button onclick="cmd('/f')">Forward</button><br>
<button onclick="cmd('/l')">Left</button>
<button onclick="cmd('/s')">Stop</button>
<button onclick="cmd('/r')">Right</button><br>
<button onclick="cmd('/b')">Back</button><br><br>

<button onclick="cmd('/up')">Spray Up</button>
<button onclick="cmd('/down')">Spray Down</button><br>
<button onclick="cmd('/lefts')">Left</button>
<button onclick="cmd('/cent')">Center</button>
<button onclick="cmd('/rights')">Right</button><br><br>

<button onclick="cmd('/on')">Spray ON</button>
<button onclick="cmd('/off')">Spray OFF</button>

<h3 id="telemetry">Telemetry: --</h3>

<script>
function cmd(u){ fetch(u); }

// Initialize 10x10 Grid Map UI
const gridDiv = document.getElementById('grid');
for (let y = 9; y >= 0; y--) {
  for (let x = 0; x < 10; x++) {
    let cell = document.createElement('div');
    cell.className = 'cell';
    cell.id = `c-${x}-${y}`;
    gridDiv.appendChild(cell);
  }
}

async function update(){
  let r = await fetch('/status');
  let d = await r.json();
  
  document.getElementById('telemetry').innerText = 
    `Rover ID: ${d.id} | Position: (${d.x}, ${d.y}) | Distance: ${d.distance} cm | Spray: ${d.spray ? "ON":"OFF"}`;
  
  // Clear previous classes
  document.querySelectorAll('.cell').forEach(c => {
    c.classList.remove('sprayed', 'self', 'peer');
    c.innerText = '';
  });

  // Mark all sprayed cells
  d.sprayed.forEach(coords => {
    let cell = document.getElementById(`c-${coords[0]}-${coords[1]}`);
    if (cell) cell.classList.add('sprayed');
  });

  // Mark other swarm peers
  d.peers.forEach(peer => {
    let cell = document.getElementById(`c-${peer.x}-${peer.y}`);
    if (cell) {
      cell.classList.add('peer');
      cell.innerText = `R${peer.id}`;
    }
  });

  // Mark self position
  let selfCell = document.getElementById(`c-${d.x}-${d.y}`);
  if (selfCell) {
    selfCell.classList.add('self');
    selfCell.innerText = `R${d.id}`;
  }
}
setInterval(update,1000);
update();
</script>

</body>
</html>
)rawliteral";

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);

  // Servo timer allocation
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);

  // Configure Relays & Motors
  pinMode(RELAY1, OUTPUT);
  pinMode(RELAY2, OUTPUT);
  sprayOFF();

  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
  pinMode(ENA, OUTPUT); pinMode(ENB, OUTPUT);
  digitalWrite(ENA, HIGH); digitalWrite(ENB, HIGH);

  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);

  // Configure Servos
  servoUp1.setPeriodHertz(50);
  servoUp2.setPeriodHertz(50);
  servoLeft.setPeriodHertz(50);

  servoUp1.attach(SERVO_UP1);
  servoUp2.attach(SERVO_UP2);
  servoLeft.attach(SERVO_LEFT);
  sprayCenter();

  // Configure Wi-Fi in AP and STA mode (necessary for concurrent web server & ESP-NOW)
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);
  
  Serial.print("Connected! Webpage Dashboard URL: http://");
  Serial.println(WiFi.localIP());

  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  // Register callback wrappers
  esp_now_register_recv_cb(esp_now_recv_cb_t(onDataRecv));
  esp_now_register_send_cb(onDataSent);

  // Register Swarm Broadcast Peer
  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0; // Automatically match Wi-Fi channel
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add broadcast peer");
    return;
  }

  /* ROUTES */
  server.on("/", [](){ server.send_P(200,"text/html",page); });

  server.on("/f", [](){ forward(); server.send(200); });
  server.on("/b", [](){ back(); server.send(200); });
  server.on("/l", [](){ left(); server.send(200); });
  server.on("/r", [](){ right(); server.send(200); });
  server.on("/s", [](){ stopCar(); server.send(200); });

  server.on("/up", [](){ sprayUp(); server.send(200); });
  server.on("/down", [](){ sprayDown(); server.send(200); });
  server.on("/lefts", [](){ sprayLeft(); server.send(200); });
  server.on("/rights", [](){ sprayRight(); server.send(200); });
  server.on("/cent", [](){ sprayCenter(); server.send(200); });

  server.on("/on", [](){ sprayON(); server.send(200); });
  server.on("/off", [](){ sprayOFF(); server.send(200); });

  server.on("/status", [](){
    String json = "{";
    json += "\"id\":" + String(ROVER_ID) + ",";
    json += "\"x\":" + String(currentX) + ",";
    json += "\"y\":" + String(currentY) + ",";
    json += "\"distance\":" + String(distanceCm) + ",";
    json += "\"spray\":" + String(isSprayON() ? "1":"0") + ",";
    
    // Add active peers to JSON response
    json += "\"peers\":[";
    bool first = true;
    for (int i = 1; i <= MAX_ROVERS; i++) {
      if (i != ROVER_ID && otherRovers[i].active && (millis() - otherRovers[i].lastSeen < 6000)) {
        if (!first) json += ",";
        json += "{\"id\":" + String(i) + ",";
        json += "\"x\":" + String(otherRovers[i].x) + ",";
        json += "\"y\":" + String(otherRovers[i].y) + ",";
        json += "\"spray\":" + String(otherRovers[i].isSpraying ? "1":"0") + "}";
        first = false;
      }
    }
    json += "],";

    // Send the shared field coverage map
    json += "\"sprayed\":[";
    first = true;
    for (int r = 0; r < 10; r++) {
      for (int c = 0; c < 10; c++) {
        if (fieldMap[r][c]) {
          if (!first) json += ",";
          json += "[" + String(r) + "," + String(c) + "]";
          first = false;
        }
      }
    }
    json += "]";
    json += "}";
    server.send(200, "application/json", json);
  });

  server.begin();
}

/* ================= LOOP ================= */
void loop() {
  server.handleClient();
  
  // Update sensor readings
  updateDistance(); 
  
  // Track location estimates (dead reckoning)
  updateDeadReckoning();

  // Send status packet to all other swarm rovers via ESP-NOW
  broadcastSwarmStatus();

  // Emergency safety halts (Ultrasonic obstacle or Swarm collision imminent)
  if (isMovingForward && (distanceCm < 20 || isSwarmCollisionImminent())) {
    stopCar();
  }
}
