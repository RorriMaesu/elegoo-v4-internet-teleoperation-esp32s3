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

// ─── Legacy single-character UNO wire commands ───────────────────────────────
#define CMD_FORWARD  'F'
#define CMD_BACKWARD 'B'
#define CMD_LEFT     'L'
#define CMD_RIGHT    'R'
#define CMD_STOP     'S'

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
      // Issue an immediate safety stop to the UNO on connection loss.
      Serial2.write(CMD_STOP);
      Serial.println("[Muscle][WS] Disconnected — safety STOP sent to UNO.");
      digitalWrite(ledPin, LOW);
      break;

    case WStype_CONNECTED:
      Serial.printf("[Muscle][WS] Connected: %s\n", payload);
      // Confirm stopped state on reconnect.
      Serial2.write(CMD_STOP);
      digitalWrite(ledPin, HIGH);
      break;

    case WStype_TEXT: {
      // Build String from payload (avoids String(char*) length ambiguity)
      String msg;
      msg.reserve(length + 1);
      for (size_t i = 0; i < length; i++) msg += (char)payload[i];

      char wireCmd = resolveWireCommand(msg);
      Serial2.write(wireCmd);
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

  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, HIGH);   // LED ON during initialization

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

  // Echo any UNO status bytes back to the USB console for diagnostics.
  while (Serial2.available() > 0) {
    char c = (char)Serial2.read();
    Serial.print("[UNO] ");
    Serial.write(c);
    Serial.println();
  }

  delay(1);   // Feed watchdog
}
