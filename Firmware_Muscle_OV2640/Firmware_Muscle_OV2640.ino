/*
 * ================================================================
 *  DUAL-BRAIN MUSCLE FIRMWARE — Firmware_Muscle_OV2640.ino
 * ================================================================
 *  Board  : ESP32-S3 WROOM (second Elegoo SmartCar V4.0 module)
 *  Sensor : OV2640 (camera circuit physically present but BYPASSED)
 *  Role   : Pure drivetrain — receives drive commands from relay
 *           /robot-control and forwards them to motor UNO via Serial2
 *  Power  : Battery Pack 1 → original Arduino UNO + motor shield
 *  Switch : Physical CAM switch LOCKED to 'CAM' position to bridge
 *           ESP32-S3 TX/RX lines down to the driving UNO
 *
 *  Board settings (Arduino IDE / arduino-cli):
 *    Board           : ESP32S3 Dev Module
 *    USB CDC On Boot : Enabled
 *    Flash Size      : 8MB (64Mb)
 *    Partition Scheme: Huge APP (3MB No OTA)
 *    PSRAM           : OPI PSRAM
 *
 *  Camera note: esp_camera_init() is intentionally omitted.
 *  The OV2640 circuit is physically present but electrically idle.
 *  This eliminates ~100 mA camera current draw and frees the DMA
 *  controller for cleaner WebSocket throughput.
 * ================================================================
 */

#include <WiFi.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <WebSocketsClient.h>   // Markus Sattler's WebSockets library

// ─── Network credentials ─────────────────────────────────────────────────────
const char* ssid     = "MySpectrumWiFi47-2G";
const char* password = "finishoasis957";

// ─── Relay server ────────────────────────────────────────────────────────────
const char* ws_host = "192.168.1.85";
const int   ws_port = 3000;

// ─── Status LED ──────────────────────────────────────────────────────────────
const int ledPin = 46;

// ─── Serial2 → Arduino UNO motor shield ──────────────────────────────────────
// Physical CAM switch bridges these UART lines to the UNO beneath.
#define UNO_RX_PIN  3
#define UNO_TX_PIN 40

// ─── Direct H-Bridge GPIO Definitions (Multi-Hardware Capability) ───────────
#define IN1  7
#define IN2  8
#define IN3  9
#define IN4  10
#define PWMA 5
#define PWMB 6
#define STBY 3

// ─── Legacy single-character UNO wire commands ───────────────────────────────
#define CMD_FORWARD  'F'
#define CMD_BACKWARD 'B'
#define CMD_LEFT     'L'
#define CMD_RIGHT    'R'
#define CMD_STOP     'S'

// ─── Time-Bounded Atomic Pulse Engine State ──────────────────────────────────
bool isTurning = false;
unsigned long turnEndTime = 0;
char currentTurnCmd = 0;

bool inBrakeInterval = false;
unsigned long brakeStartTime = 0;
const unsigned long BRAKE_DURATION_MS = 40; // Brake lock pause

bool nextPulsePending = false;
char pendingTurnCmd = 0;
char activeSteerState = 0;  // Tracks stateful steering holds (CMD_LEFT, CMD_RIGHT, or 0)


// ─── State Machine Helper Functions ──────────────────────────────────────────
void startTurnPulse(char cmd) {
  isTurning = true;
  currentTurnCmd = cmd;
  turnEndTime = millis() + 80; // 80ms hard steering pulse boundary

  // 1. HIGH-TORQUE TURN INITIATION (Direct H-Bridge GPIO Control)
  digitalWrite(STBY, HIGH);
  if (cmd == CMD_LEFT) {
    // Left turn: Right forward (IN1=HIGH, IN2=LOW), Left backward (IN3=LOW, IN4=HIGH)
    digitalWrite(IN1, HIGH);
    digitalWrite(IN2, LOW);
    digitalWrite(IN3, LOW);
    digitalWrite(IN4, HIGH);
  } else if (cmd == CMD_RIGHT) {
    // Right turn: Right backward (IN1=LOW, IN2=HIGH), Left forward (IN3=HIGH, IN4=LOW)
    digitalWrite(IN1, LOW);
    digitalWrite(IN2, HIGH);
    digitalWrite(IN3, HIGH);
    digitalWrite(IN4, LOW);
  }
  analogWrite(PWMA, 255); // Punch motors at full PWM duty cycle (255) to break static scrub
  analogWrite(PWMB, 255);

  // 2. HIGH-TORQUE TURN INITIATION (Serial2 forward to UNO)
  Serial2.write(cmd);

  Serial.printf("[Pulse Engine] Steering pulse '%c' initiated (80ms)\n", cmd);
}

void stopAndBrake() {
  isTurning = false;

  // 3. ACTIVE ELECTRONIC BRAKING (Direct H-Bridge GPIO Control)
  // Pull both direction pins HIGH on the H-Bridge to short-circuit the motors
  digitalWrite(STBY, HIGH);
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, HIGH);
  analogWrite(PWMA, 255); // Full PWM active braking force
  analogWrite(PWMB, 255);

  // 3. ACTIVE ELECTRONIC BRAKING (Serial2 forward to UNO)
  Serial2.write(CMD_STOP);

  Serial.println("[Pulse Engine] ACTIVE HARD BRAKE engaged!");
}

void releaseBrake() {
  // Put driver in standby mode to prevent permanent power drain & heat build-up
  digitalWrite(STBY, LOW);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
  analogWrite(PWMA, 0);
  analogWrite(PWMB, 0);
  Serial.println("[Pulse Engine] Brake released, standby active.");
}

// ─── WebSocket client ────────────────────────────────────────────────────────
WebSocketsClient webSocket;

// ─── Command resolver ────────────────────────────────────────────────────────
// Accepts two input formats:
//   1. Stateful string  : "drive:forward,start"  →  'F'
//   2. Legacy char      : "F"                    →  'F'
// Any unrecognised input resolves to 'S' (safe stop).
char resolveWireCommand(const String& raw) {
  String msg = raw;
  msg.trim();

  // ── Legacy single-character (relay sends pre-mapped wire commands) ─────────
  if (msg.length() == 1) {
    char c = (char)toupper((unsigned char)msg.charAt(0));
    if (c == CMD_FORWARD  || c == CMD_BACKWARD ||
        c == CMD_LEFT     || c == CMD_RIGHT    || c == CMD_STOP) {
      return c;
    }
    return CMD_STOP;
  }

  // ── Stateful string: "drive:<direction>,<state>" ──────────────────────────
  if (!msg.startsWith("drive:")) return CMD_STOP;

  int comma = msg.indexOf(',');
  if (comma < 0) return CMD_STOP;

  String direction = msg.substring(6, comma);
  String state     = msg.substring(comma + 1);
  direction.toLowerCase();
  state.toLowerCase();

  if (state == "stop") return CMD_STOP;
  if (state != "start") return CMD_STOP;

  if      (direction == "forward")  return CMD_FORWARD;
  else if (direction == "backward") return CMD_BACKWARD;
  else if (direction == "left")     return CMD_LEFT;
  else if (direction == "right")    return CMD_RIGHT;
  else if (direction == "all")      return CMD_STOP;

  return CMD_STOP;   // Unknown direction → safe stop
}

// ─── WebSocket event handler ─────────────────────────────────────────────────
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {

    case WStype_DISCONNECTED:
      // Issue an immediate safety stop & active brake on connection loss.
      nextPulsePending = false;
      activeSteerState = 0;
      stopAndBrake();
      inBrakeInterval = false;
      releaseBrake();
      Serial.println("[Muscle][WS] Disconnected — safety STOP & hard BRAKE engaged.");
      digitalWrite(ledPin, LOW);
      break;

    case WStype_CONNECTED:
      Serial.printf("[Muscle][WS] Connected: %s\n", payload);
      // Confirm stopped state on reconnect.
      nextPulsePending = false;
      activeSteerState = 0;
      stopAndBrake();
      inBrakeInterval = false;
      releaseBrake();
      digitalWrite(ledPin, HIGH);
      break;

    case WStype_TEXT: {
      // Build String from payload (avoids String(char*) length ambiguity)
      String msg;
      msg.reserve(length + 1);
      for (size_t i = 0; i < length; i++) msg += (char)payload[i];

      char wireCmd = resolveWireCommand(msg);
      
      if (wireCmd == CMD_LEFT || wireCmd == CMD_RIGHT) {
        activeSteerState = wireCmd; // Track active steering hold state
        if (!isTurning && !inBrakeInterval) {
          // No turn active and not in brake pause — initiate pulse immediately
          startTurnPulse(wireCmd);
        } else {
          // Turn active or currently braking — queue the next pulse to smoothly chain stutter steps
          nextPulsePending = true;
          pendingTurnCmd = wireCmd;
        }
      } else {
        // Direct non-steering commands (F, B, S) immediately cancel any pending steering pulses
        nextPulsePending = false;
        activeSteerState = 0; // Clear stateful steer hold
        
        if (wireCmd == CMD_STOP) {
          if (isTurning) {
            // Trigger active electronic braking for the full duration
            stopAndBrake();
            inBrakeInterval = true;
            brakeStartTime = millis();
          } else if (inBrakeInterval) {
            // Already braking, let it finish naturally
          } else {
            // Otherwise, release brake and standby immediately
            releaseBrake();
            Serial2.write(CMD_STOP);
          }
        } else {
          // Forward/Backward: stop any active steer and forward immediately
          if (isTurning || inBrakeInterval) {
            stopAndBrake();
            inBrakeInterval = false;
            releaseBrake();
          }
          Serial2.write(wireCmd);
        }
      }

      Serial.printf("[Muscle] '%s' → UNO '%c'\n", msg.c_str(), wireCmd);
      break;
    }

    case WStype_BIN:
      // Muscle does not receive binary data from the relay.
      break;

    default:
      break;
  }
}

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // Disable brownout detector

  // Initialize status LED
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, HIGH);   // LED ON during initialization

  // Initialize H-bridge control pins
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);
  pinMode(PWMA, OUTPUT);
  pinMode(PWMB, OUTPUT);
  pinMode(STBY, OUTPUT);

  // Set H-bridge initially disabled
  digitalWrite(STBY, LOW);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
  analogWrite(PWMA, 0);
  analogWrite(PWMB, 0);

  Serial.begin(115200);

  // ── Serial2 → Arduino UNO motor shield ───────────────────────────────────
  Serial2.begin(115200, SERIAL_8N1, UNO_RX_PIN, UNO_TX_PIN);

  delay(1500);

  Serial.println("\n=======================================================");
  Serial.println("       DUAL-BRAIN MUSCLE FIRMWARE — DRIVETRAIN ONLY    ");
  Serial.println("=======================================================");
  Serial.println("Camera: BYPASSED — esp_camera_init() omitted by design.");
  Serial.println("Serial2 to UNO: RX=3, TX=40, 115200 baud.");

  // ── WiFi STA connect ─────────────────────────────────────────────────────
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);

  Serial.printf("Connecting to '%s'...\n", ssid);
  int wifiTimeout = 20;   // 20 × 500 ms = 10 s
  while (WiFi.status() != WL_CONNECTED && wifiTimeout-- > 0) {
    digitalWrite(ledPin, !digitalRead(ledPin));
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false);                       // RF hardening: no sleep
    WiFi.setTxPower(WIFI_POWER_19_5dBm);        // RF hardening: max TX power
    Serial.printf("WiFi connected — IP: %s\n",
                  WiFi.localIP().toString().c_str());
    Serial.println("WiFi sleep disabled | TX power 19.5 dBm (max)");
    digitalWrite(ledPin, HIGH);
  } else {
    Serial.println("WiFi FAILED — halting. Check SSID / password / range.");
    while (true) {
      digitalWrite(ledPin, HIGH); delay(150);
      digitalWrite(ledPin, LOW);  delay(150);
    }
  }

  // Safety stop: guarantee UNO starts in a halted state before WS connects.
  Serial2.write(CMD_STOP);
  Serial.println("Safety STOP sent to UNO.");

  // ── WebSocket relay connection ─────────────────────────────────────────────
  Serial.printf("Connecting WS → ws://%s:%d/robot-control\n", ws_host, ws_port);
  webSocket.begin(ws_host, ws_port, "/robot-control");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);   // Retry every 5 s on drop

  digitalWrite(ledPin, LOW);
  Serial.println("Muscle boot complete. Listening on /robot-control.");
  Serial.println("=======================================================");
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
  webSocket.loop();   // Must be called frequently for connection maintenance

  // ─── Time-Bounded Atomic Pulse Engine State Monitor ────────────────────────
  unsigned long currentMillis = millis();

  // 1. Check if the active steering pulse has expired
  if (isTurning && (currentMillis >= turnEndTime)) {
    stopAndBrake();
    inBrakeInterval = true;
    brakeStartTime = currentMillis;
  }

  // 2. Check if the active hard brake interval has completed
  if (inBrakeInterval && (currentMillis - brakeStartTime >= BRAKE_DURATION_MS)) {
    inBrakeInterval = false;
    if (activeSteerState == CMD_LEFT || activeSteerState == CMD_RIGHT) {
      nextPulsePending = false; // Clear queued pulse since we are auto-generating
      startTurnPulse(activeSteerState);
    } else if (nextPulsePending) {
      nextPulsePending = false;
      startTurnPulse(pendingTurnCmd);
    } else {
      releaseBrake();
    }
  }

  // Echo any UNO status bytes back to the USB console for diagnostics.
  while (Serial2.available() > 0) {
    char c = (char)Serial2.read();
    Serial.print("[UNO] ");
    Serial.write(c);
    Serial.println();
  }

  delay(1);   // Feed watchdog
}
