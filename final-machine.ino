#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>

// ====== SERVOMOTOR DEFINITION ======
Servo myservo1;
Servo myservo2;
// NOTE: Pins 1 & 3 are UART0; if you see glitches, move to pins 4/5.
#define SERVO_PIN_1 1
#define SERVO_PIN_2 3

// ====== PIN DEFINITIONS ======
// Boat detector (used for Auto mode)
#define BoatRight_TRIG 16
#define BoatRight_ECHO 17

// Other sensors (wired, optional)
#define RoadBack_TRIG 21
#define RoadBack_ECHO 22
#define RoadFront_TRIG 25
#define RoadFront_ECHO 5
#define BoatLeft_TRIG 18
#define BoatLeft_ECHO 19

// Encoder
#define ENCODE_A_PIN 32
#define ENCODE_B_PIN 33

// Traffic Lights (Boat side)
#define RED_PIN 27
#define YELLOW_PIN 26
#define GREEN_PIN 12
// Traffic Lights (Road side)
#define RED2_PIN 23
#define YELLOW2_PIN 4
#define GREEN2_PIN 14

// Motor driver (H-bridge)
#define MOTOR_IN1 15
#define MOTOR_IN2 13
#define MOTOR_ENA 2   // PWM

// ====== MOTOR / ENCODER SETTINGS ======
#define ENCODER_COUNTS_PER_REV 900
volatile long encoderCount = 0;
int MOTOR_SPEED = 150;

bool bridgeOpen = false;   // UI/logic latch

// Auto mode thresholds (hysteresis)
bool autoMode = true;
const float OPEN_NEAR_CM = 25.0;  // boat very close -> open
const float CLOSE_FAR_CM = 28.0;  // boat gone -> close

// ====== WIFI AP ======
const char* apSSID = "Smart_Bridge";
const char* apPASS = "12345678";
WebServer server(80);

// ------------------ ENCODER ISR ------------------
void IRAM_ATTR onEncA() {
  int a = digitalRead(ENCODE_A_PIN);
  int b = digitalRead(ENCODE_B_PIN);
  if (a != b) encoderCount++;
  else encoderCount--;
}

// ------------------ MOTOR CONTROL ----------------
// NOTE (wiring): OPEN uses cw=false, CLOSE uses cw=true to match your wiring.
void motorForward(int pwm) {
  digitalWrite(MOTOR_IN1, HIGH);
  digitalWrite(MOTOR_IN2, LOW);
  analogWrite(MOTOR_ENA, pwm);
}
void motorReverse(int pwm) {
  digitalWrite(MOTOR_IN1, LOW);
  digitalWrite(MOTOR_IN2, HIGH);
  analogWrite(MOTOR_ENA, pwm);
}
void motorStop() {
  digitalWrite(MOTOR_IN1, LOW);
  digitalWrite(MOTOR_IN2, LOW);
  analogWrite(MOTOR_ENA, 0);
}

// ------------------ DISTANCE ----------------------
float getDistance(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);
  long duration = pulseIn(echoPin, HIGH, 30000UL);
  if (duration == 0) return 9999.0f;
  return duration * 0.0343f / 2.0f;
}

// ------------------ LIGHTS -----------------------
void setBoatLights(const char* color) {
  int r = LOW, y = LOW, g = LOW;
  if (!strcmp(color, "red")) r = HIGH;
  if (!strcmp(color, "yellow")) y = HIGH;
  if (!strcmp(color, "green")) g = HIGH;
  digitalWrite(RED_PIN, r);
  digitalWrite(YELLOW_PIN, y);
  digitalWrite(GREEN_PIN, g);
}
void setRoadLights(const char* color) {
  int r = LOW, y = LOW, g = LOW;
  if (!strcmp(color, "red")) r = HIGH;
  if (!strcmp(color, "yellow")) y = HIGH;
  if (!strcmp(color, "green")) g = HIGH;
  digitalWrite(RED2_PIN, r);
  digitalWrite(YELLOW2_PIN, y);
  digitalWrite(GREEN2_PIN, g);
}
void allLightsOff() {
  digitalWrite(RED_PIN, LOW); digitalWrite(YELLOW_PIN, LOW); digitalWrite(GREEN_PIN, LOW);
  digitalWrite(RED2_PIN, LOW); digitalWrite(YELLOW2_PIN, LOW); digitalWrite(GREEN2_PIN, LOW);
}

// ------------------ ROTATION (1 rev OR 14s timeout) ---------------------
void rotateRevolutions(float revolutions, bool cw) {
  long targetCounts = (long)(revolutions * ENCODER_COUNTS_PER_REV);
  encoderCount = 0;
  unsigned long startTime = millis();
  const unsigned long stopAfter = 14000;   // 14 seconds timeout

  if (cw) motorForward(MOTOR_SPEED);
  else    motorReverse(MOTOR_SPEED);

  while (true) {
    unsigned long elapsed = millis() - startTime;
    if (abs(encoderCount) >= targetCounts || elapsed >= stopAfter) break;
    server.handleClient();
    delay(10);
  }

  motorStop();
  delay(300); // settle
  Serial.printf("Motor stopped (%.2f rev or %lu ms, Encoder=%ld)\n",
                revolutions, millis() - startTime, encoderCount);
}

// -------- Helpers so manual + auto do identical safe sequences --------
void openOneRev() {
  // MOVING PHASE (safety): cars must be red while lifting
  setRoadLights("red");
  setBoatLights("red");
  rotateRevolutions(1, false);  // OPEN (wiring-corrected)
  bridgeOpen = true;

  // STEADY STATE:
  // Boat present context → Boat GREEN, Road RED
  setBoatLights("green");
  setRoadLights("red");
}

void closeOneRev() {
  // MOVING PHASE (safety): boats must be red while lowering
  setBoatLights("red");
  setRoadLights("yellow"); delay(1500);
  setRoadLights("red");
  rotateRevolutions(1, true);   // CLOSE (wiring-corrected)
  bridgeOpen = false;

  // STEADY STATE:
  // No boat context → Road GREEN, Boat RED
  setRoadLights("green");
  setBoatLights("red");
}

// ------------------ WEB UI ---------------------------
String htmlPage() {
  float boatDist = getDistance(BoatRight_TRIG, BoatRight_ECHO);

  String s;
  s.reserve(6000);
  s += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  s += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  s += "<style>";
  s += "body{font-family:system-ui,Arial;margin:16px;text-align:center}";
  s += "h2{margin:8px 0} .row{margin:14px 0}";
  s += "button{font-size:16px;padding:10px 16px;margin:6px;border-radius:8px;border:1px solid #ccc;cursor:pointer}";
  s += ".danger{background:#e74c3c;color:#fff;border:none}";
  s += ".ok{background:#2ecc71;color:#fff;border:none}";
  s += ".warn{background:#f1c40f;color:#222;border:none}";
  s += ".chip{display:inline-block;padding:6px 10px;border-radius:999px;border:1px solid #ddd;margin:2px 6px}";
  s += "</style></head><body>";

  s += "<h2>Smart Bridge Controller</h2>";
  s += "<div class='row'>";
  s += "<span class='chip'><b>Status:</b> " + String(bridgeOpen ? "OPEN" : "CLOSED") + "</span>";
  s += "<span class='chip'><b>Boat Dist:</b> " + String(boatDist,1) + " cm</span>";
  s += "<span class='chip'><b>Speed:</b> " + String(MOTOR_SPEED) + "</span>";
  s += "<span class='chip'><b>Auto Mode:</b> " + String(autoMode ? "ON" : "OFF") + "</span>";
  s += "</div>";
  s += "<p><small>Auto: open when &lt; 25 cm (Boat GREEN, Road RED); close when &gt; 28 cm (Road GREEN, Boat RED). Motion stops at 1 rev or <b>14 s</b>.</small></p>";

  // --- Motor Controls ---
  s += "<div class='row'><h3>Bridge Motor</h3>";
  s += "<button onclick=\"location.href='/open1'\" class='ok'>⬆️ Open (1 Rev)</button>";
  s += "<button onclick=\"location.href='/close1'\" class='warn'>⬇️ Close (1 Rev)</button><br>";
  s += "<button onclick=\"location.href='/stop'\" class='danger'>■ STOP</button>";
  s += "</div>";

  // --- Speed Controls ---
  s += "<div class='row'><h3>Motor Speed</h3>";
  s += "<button onclick=\"location.href='/speed?val=150'\">150</button>";
  s += "<button onclick=\"location.href='/speed?val=180'\">180</button>";
  s += "<button onclick=\"location.href='/speed?val=200'\">200</button>";
  s += "</div>";

  // --- Auto Mode ---
  s += "<div class='row'><h3>Auto Mode</h3>";
  s += "<button onclick=\"location.href='/auto/toggle'\">" + String(autoMode ? "Disable" : "Enable") + " Auto</button>";
  s += "</div>";

  // --- Lights ---
  s += "<div class='row'><h3>Boat Traffic Lights</h3>";
  s += "<button onclick=\"location.href='/boat/red'\">RED</button>";
  s += "<button onclick=\"location.href='/boat/yellow'\">YELLOW</button>";
  s += "<button onclick=\"location.href='/boat/green'\">GREEN</button>";
  s += "</div>";

  s += "<div class='row'><h3>Road Traffic Lights</h3>";
  s += "<button onclick=\"location.href='/road/red'\">RED</button>";
  s += "<button onclick=\"location.href='/road/yellow'\">YELLOW</button>";
  s += "<button onclick=\"location.href='/road/green'\">GREEN</button>";
  s += "</div>";

  s += "<div class='row'>";
  s += "<button onclick=\"location.href='/lightsOff'\">All Lights OFF</button>";
  s += "</div>";

  // --- Servos ---
  s += "<div class='row'><h3>Boom Gates</h3>";
  s += "<button onclick=\"location.href='/servo/0'\">Closed (0°)</button>";
  s += "<button onclick=\"location.href='/servo/90'\">Open (90°)</button>";
  s += "</div>";

  s += "</body></html>";
  return s;
}

// ------------------ HANDLERS -----------------
void handleRoot() { server.send(200, "text/html", htmlPage()); }
void handleStop() { motorStop(); server.sendHeader("Location", "/", true); server.send(302, ""); }

void handleSpeed() {
  if (server.hasArg("val")) {
    int v = server.arg("val").toInt();
    v = constrain(v, 0, 255);
    MOTOR_SPEED = v;
  }
  server.sendHeader("Location", "/", true); server.send(302, "");
}
void handleBoatLights() {
  String path = server.uri();
  if (path.endsWith("/red")) setBoatLights("red");
  else if (path.endsWith("/yellow")) setBoatLights("yellow");
  else if (path.endsWith("/green")) setBoatLights("green");
  server.sendHeader("Location", "/", true); server.send(302, "");
}
void handleRoadLights() {
  String path = server.uri();
  if (path.endsWith("/red")) setRoadLights("red");
  else if (path.endsWith("/yellow")) setRoadLights("yellow");
  else if (path.endsWith("/green")) setRoadLights("green");
  server.sendHeader("Location", "/", true); server.send(302, "");
}
void handleServo0() { myservo1.write(0); myservo2.write(0); server.sendHeader("Location", "/", true); server.send(302, ""); }
void handleServo90() { myservo1.write(90); myservo2.write(90); server.sendHeader("Location", "/", true); server.send(302, ""); }

// ------------------ SETUP ------------------------
void setup() {
  Serial.begin(115200);

  // Pins
  pinMode(BoatRight_TRIG, OUTPUT); pinMode(BoatRight_ECHO, INPUT);
  pinMode(RoadBack_TRIG, OUTPUT);  pinMode(RoadBack_ECHO, INPUT);
  pinMode(RoadFront_TRIG, OUTPUT); pinMode(RoadFront_ECHO, INPUT);
  pinMode(BoatLeft_TRIG, OUTPUT);  pinMode(BoatLeft_ECHO, INPUT);

  pinMode(RED_PIN, OUTPUT); pinMode(YELLOW_PIN, OUTPUT); pinMode(GREEN_PIN, OUTPUT);
  pinMode(RED2_PIN, OUTPUT); pinMode(YELLOW2_PIN, OUTPUT); pinMode(GREEN2_PIN, OUTPUT);

  pinMode(MOTOR_IN1, OUTPUT); pinMode(MOTOR_IN2, OUTPUT); pinMode(MOTOR_ENA, OUTPUT);

  pinMode(ENCODE_A_PIN, INPUT_PULLUP);
  pinMode(ENCODE_B_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENCODE_A_PIN), onEncA, CHANGE);

  // Servos
  myservo1.attach(SERVO_PIN_1);
  myservo2.attach(SERVO_PIN_2);
  myservo1.write(0);
  myservo2.write(0);

  allLightsOff();
  // Start with a sensible default: no boat → road green
  setRoadLights("green");
  setBoatLights("red");

  // Wi-Fi
  WiFi.softAP(apSSID, apPASS);
  Serial.println("====================================");
  Serial.printf("Hotspot: %s\nPassword: %s\n", apSSID, apPASS);
  Serial.printf("Open:   http://%s\n", WiFi.softAPIP().toString().c_str());
  Serial.println("====================================");

  // Routes
  server.on("/", handleRoot);
  server.on("/stop", handleStop);

  server.on("/speed", handleSpeed);

  server.on("/boat/red",    handleBoatLights);
  server.on("/boat/yellow", handleBoatLights);
  server.on("/boat/green",  handleBoatLights);

  server.on("/road/red",    handleRoadLights);
  server.on("/road/yellow", handleRoadLights);
  server.on("/road/green",  handleRoadLights);

  server.on("/lightsOff", [](){ allLightsOff(); server.sendHeader("Location", "/", true); server.send(302, ""); });

  server.on("/servo/0", handleServo0);
  server.on("/servo/90", handleServo90);

  // Manual motion → safe helpers (and set steady-state greens after motion)
  server.on("/open1",  [](){ openOneRev();  server.sendHeader("Location","/",true); server.send(302,""); });
  server.on("/close1", [](){ closeOneRev(); server.sendHeader("Location","/",true); server.send(302,""); });

  // Auto toggle
  server.on("/auto/toggle", [](){ autoMode = !autoMode; server.sendHeader("Location","/",true); server.send(302,""); });

  server.begin();
  Serial.println("Web server started.");
}

// ------------------ LOOP -------------------------
void loop() {
  server.handleClient();

  // -------- AUTO BOAT DETECTION (wiring-corrected directions) --------
  static unsigned long lastCheck = 0;
  if (!autoMode) return;

  if (millis() - lastCheck >= 500) { // 2x/sec
    lastCheck = millis();
    float boatDist = getDistance(BoatRight_TRIG, BoatRight_ECHO);

    // Boat near -> OPEN: moving phase then Boat GREEN steady-state
    if (boatDist < OPEN_NEAR_CM && !bridgeOpen) {
      Serial.println("🚢 Boat <25 cm → Opening bridge (1 rev / 14s)");
      openOneRev();   // includes road RED while moving, then Boat GREEN / Road RED
    }
    // Boat gone -> CLOSE: moving phase then Road GREEN steady-state
    else if (boatDist > CLOSE_FAR_CM && bridgeOpen) {
      Serial.println("✅ Boat >28 cm → Closing bridge (1 rev / 14s)");
      closeOneRev();  // includes Boat RED while moving, then Road GREEN / Boat RED
    }
  }
}
