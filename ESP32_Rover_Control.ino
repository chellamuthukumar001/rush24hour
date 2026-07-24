#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>

/* ================= WIFI ================= */
const char* ssid = "Redmi Note 12";
const char* password = "redmi12pro";

WebServer server(80);

/* ================= MOTOR PINS (UPDATED FOR ESP32-S3) ================= */
// Pins 25, 26, 27, and 33 on the original ESP32 are either not present or 
// are reserved for the internal SPI Flash / PSRAM on the ESP32-S3.
// They have been remapped to safe, general-purpose GPIOs.
#define IN1 6
#define IN2 7
#define IN3 8
#define IN4 9
#define ENA 14
#define ENB 12

/* ================= SERVO PINS (UPDATED FOR ESP32-S3) ================= */
// Pin 19 on the ESP32-S3 is the default native USB D- line. To avoid breaking 
// USB upload or serial communication when the servo moves, we remapped SERVO_UP2 to 13.
#define SERVO_UP1   18
#define SERVO_UP2   13
#define SERVO_LEFT  21

/* ================= RELAY PINS ================= */
#define RELAY1 2
#define RELAY2 15   // ACTIVE-LOW

/* ================= ULTRASONIC ================= */
#define TRIG 5
#define ECHO 4

// Distance (cm) below which forward() will refuse to move
#define OBSTACLE_STOP_CM 20
// Value returned when the sensor times out / gives no reading (treated as "clear")
#define NO_ECHO_DISTANCE_CM 999
// Max time (us) to wait for an echo. ~30ms covers ~5m range, HC-SR04's max spec.
#define ECHO_TIMEOUT_US 30000

Servo servoUp1, servoUp2, servoLeft;

int upPos = 90;
int leftPos = 90;

/* ================= DISTANCE ================= */
// Single raw reading. Returns NO_ECHO_DISTANCE_CM if no echo was received.
long readDistanceOnce() {
  digitalWrite(TRIG, LOW);
  delayMicroseconds(3);
  digitalWrite(TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG, LOW);

  long duration = pulseIn(ECHO, HIGH, ECHO_TIMEOUT_US);

  if (duration == 0) {
    // No echo received - sensor not wired, out of range, or timed out.
    return NO_ECHO_DISTANCE_CM;
  }

  return duration * 0.034 / 2;
}

// Takes 3 quick samples and returns the median, to filter out a single
// noisy/glitched reading (common cause of "ultrasonic randomly not working").
long getDistance() {
  long a = readDistanceOnce();
  delay(10); // give the sensor a moment to settle between pulses
  long b = readDistanceOnce();
  delay(10);
  long c = readDistanceOnce();

  // simple median of 3
  if (a > b) { long t = a; a = b; b = t; }
  if (b > c) { long t = b; b = c; c = t; }
  if (a > b) { long t = a; a = b; b = t; }

  return b;
}

/* ================= MOTOR ================= */
void stopCar() {
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
}

void forward() {
  if (getDistance() < OBSTACLE_STOP_CM) { stopCar(); return; }
  digitalWrite(IN1, LOW); digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW); digitalWrite(IN4, HIGH);
}

void back() {
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
}

void left() {
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);
}

void right() {
  digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
}

/* ================= SERVO ================= */
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

/* ================= RELAY (ACTIVE-LOW) ================= */
void sprayON() {
  digitalWrite(RELAY1, LOW);
  digitalWrite(RELAY2, LOW);
}

void sprayOFF() {
  digitalWrite(RELAY1, HIGH);
  digitalWrite(RELAY2, HIGH);
}

bool isSprayON() {
  return digitalRead(RELAY1) == LOW;
}

/* ================= SIMPLE WEB PAGE ================= */
const char page[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
body{font-family:Arial;background:#111;color:#0f0;text-align:center}
button{padding:15px;margin:8px;font-size:18px}
</style>
</head>
<body>

<h2>ESP32 Rover Control</h2>

<h3>Live Camera</h3>
<iframe src="http://10.54.194.85" width="360" height="300"></iframe>
<br><br>

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

<h3 id="dist">Distance: --</h3>
<h3 id="spray">Spray: --</h3>

<script>
function cmd(u){ fetch(u); }

async function update(){
  let r = await fetch('/status');
  let d = await r.json();
  document.getElementById('dist').innerText = "Distance: " + d.distance + " cm";
  document.getElementById('spray').innerText = "Spray: " + (d.spray ? "ON":"OFF");
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

  // Allocate all timers for ESP32Servo to prevent resource conflicts on S3
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);

  pinMode(RELAY1, OUTPUT);
  pinMode(RELAY2, OUTPUT);
  sprayOFF();

  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
  pinMode(ENA, OUTPUT); pinMode(ENB, OUTPUT);
  digitalWrite(ENA, HIGH); digitalWrite(ENB, HIGH);

  pinMode(TRIG, OUTPUT);
  digitalWrite(TRIG, LOW); // ensure a known, clean starting state
  pinMode(ECHO, INPUT);

  // Explicitly set the servo update period (standard 50Hz)
  servoUp1.setPeriodHertz(50);
  servoUp2.setPeriodHertz(50);
  servoLeft.setPeriodHertz(50);

  servoUp1.attach(SERVO_UP1);
  servoUp2.attach(SERVO_UP2);
  servoLeft.attach(SERVO_LEFT);
  sprayCenter();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);

  Serial.println(WiFi.localIP());

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
    long d = getDistance();
    Serial.print("Distance: ");
    Serial.println(d); // watch this in Serial Monitor to verify readings
    String json = "{";
    json += "\"distance\":" + String(d) + ",";
    json += "\"spray\":" + String(isSprayON() ? "1":"0");
    json += "}";
    server.send(200,"application/json",json);
  });

  server.begin();
}

/* ================= LOOP ================= */
void loop() {
  server.handleClient();
}
