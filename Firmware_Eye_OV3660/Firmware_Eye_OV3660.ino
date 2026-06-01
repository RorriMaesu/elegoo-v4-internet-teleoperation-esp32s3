/*
 * ================================================================
 *  DUAL-BRAIN EYE FIRMWARE — Firmware_Eye_OV3660.ino
 * ================================================================
 *  Board  : ESP32-S3 WROOM (Elegoo SmartCar V4.0 camera module)
 *  Sensor : OV3660
 *  Role   : Pure vision — streams JPEG frames to relay /robot-video
 *  Power  : Battery Pack 2 → second Arduino UNO (bracket + 5V rail)
 *
 *  Board settings (Arduino IDE / arduino-cli):
 *    Board           : ESP32S3 Dev Module
 *    USB CDC On Boot : Enabled
 *    Flash Size      : 8MB (64Mb)
 *    Partition Scheme: Huge APP (3MB No OTA)
 *    PSRAM           : OPI PSRAM
 *
 *  Pin mapping source: Elegoo official CameraWebServer_AP_2023_V1.3
 *                      CAMERA_MODEL_ESP32S3_EYE (confirmed identical
 *                      to our proven monolithic baseline)
 * ================================================================
 */

#include "esp_camera.h"
#include <WiFi.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <WebSocketsClient.h>   // Markus Sattler's WebSockets library

// ─── Network credentials ─────────────────────────────────────────────────────
const char* ssid     = "MySpectrumWiFi47-2G";
const char* password = "finishoasis957";

// ─── Relay server ────────────────────────────────────────────────────────────
const char* ws_host = "192.168.1.213";
const int   ws_port = 3000;

// ─── Status LED ──────────────────────────────────────────────────────────────
const int ledPin = 46;

// ─── Frame pacing ────────────────────────────────────────────────────────────
// 45ms ≈ 22 FPS cap — balanced throughput vs relay-side latency budget
static const unsigned long FRAME_INTERVAL_MS = 45;

// ─── ESP32-S3-EYE / Elegoo SmartCar V4.0 Camera Pin Mapping ──────────────────
// Verified against Elegoo official docs (CAMERA_MODEL_ESP32S3_EYE).
// THESE PINS ARE FROZEN — do not alter without a hardware re-audit.
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM   15
#define SIOD_GPIO_NUM    4   // I²C SDA
#define SIOC_GPIO_NUM    5   // I²C SCL
#define Y9_GPIO_NUM     16
#define Y8_GPIO_NUM     17
#define Y7_GPIO_NUM     18
#define Y6_GPIO_NUM     12
#define Y5_GPIO_NUM     10
#define Y4_GPIO_NUM      8
#define Y3_GPIO_NUM      9
#define Y2_GPIO_NUM     11
#define VSYNC_GPIO_NUM   6
#define HREF_GPIO_NUM    7
#define PCLK_GPIO_NUM   13

// ─── WebSocket client ────────────────────────────────────────────────────────
WebSocketsClient webSocket;

// ─── WebSocket event handler ─────────────────────────────────────────────────
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {

    case WStype_DISCONNECTED:
      Serial.println("[Eye][WS] Disconnected from relay.");
      digitalWrite(ledPin, LOW);
      break;

    case WStype_CONNECTED:
      Serial.printf("[Eye][WS] Connected: %s\n", payload);
      digitalWrite(ledPin, HIGH);
      break;

    case WStype_TEXT:
      // Eye receives no control commands; log and discard.
      Serial.printf("[Eye][WS] Unexpected text (ignored): %.*s\n",
                    (int)length, (char*)payload);
      break;

    case WStype_BIN:
      // Eye sends binary; it does not receive it.
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
  delay(1500);

  Serial.println("\n=======================================================");
  Serial.println("        DUAL-BRAIN EYE FIRMWARE — OV3660 SENSOR        ");
  Serial.println("=======================================================");
  Serial.printf("OPI PSRAM detected: %s\n", psramFound() ? "YES" : "NO");

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

  // ── Camera initialisation (OV3660, CAMERA_MODEL_ESP32S3_EYE) ─────────────
  Serial.println("Initializing OV3660 camera sensor...");

  camera_config_t cfg;
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.ledc_timer   = LEDC_TIMER_0;
  cfg.pin_d0       = Y2_GPIO_NUM;
  cfg.pin_d1       = Y3_GPIO_NUM;
  cfg.pin_d2       = Y4_GPIO_NUM;
  cfg.pin_d3       = Y5_GPIO_NUM;
  cfg.pin_d4       = Y6_GPIO_NUM;
  cfg.pin_d5       = Y7_GPIO_NUM;
  cfg.pin_d6       = Y8_GPIO_NUM;
  cfg.pin_d7       = Y9_GPIO_NUM;
  cfg.pin_xclk     = XCLK_GPIO_NUM;
  cfg.pin_pclk     = PCLK_GPIO_NUM;
  cfg.pin_vsync    = VSYNC_GPIO_NUM;
  cfg.pin_href     = HREF_GPIO_NUM;
  cfg.pin_sccb_sda = SIOD_GPIO_NUM;
  cfg.pin_sccb_scl = SIOC_GPIO_NUM;
  cfg.pin_pwdn     = PWDN_GPIO_NUM;
  cfg.pin_reset    = RESET_GPIO_NUM;
  cfg.xclk_freq_hz = 20000000;         // 20 MHz — verified stable on OV3660
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.fb_location  = CAMERA_FB_IN_PSRAM;
  cfg.jpeg_quality = 25;               // Matches proven baseline
  cfg.frame_size   = FRAMESIZE_QVGA;  // 320 × 240

  if (psramFound()) {
    cfg.fb_count  = 2;
    cfg.grab_mode = CAMERA_GRAB_LATEST; // Always pull the freshest frame
    Serial.println("PSRAM: fb_count=2, CAMERA_GRAB_LATEST");
  } else {
    cfg.fb_count  = 1;
    cfg.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    Serial.println("No PSRAM: fb_count=1, CAMERA_GRAB_WHEN_EMPTY");
  }

  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    Serial.printf("Camera init FAILED: 0x%x — check wiring.\n", err);
    for (int i = 0; i < 10; i++) {
      digitalWrite(ledPin, HIGH); delay(70);
      digitalWrite(ledPin, LOW);  delay(70);
    }
    // Do not halt; let the WebSocket connect so the relay sees the board online.
  } else {
    Serial.println("Camera initialized OK.");

    // OV3660-specific sensor register tuning
    // CRITICAL: OV3660 mounts inverted on this PCB — vflip is mandatory.
    sensor_t* s = esp_camera_sensor_get();
    if (s && s->id.PID == OV3660_PID) {
      s->set_vflip(s, 1);        // Correct OV3660 vertical orientation
      s->set_brightness(s, 1);   // Slight brightness boost
      s->set_saturation(s, -2);  // Reduce oversaturation
      s->set_whitebal(s, 1);     // Auto white balance on
      s->set_awb_gain(s, 1);     // AWB gain on
      s->set_exposure_ctrl(s, 1);// Auto exposure on
      s->set_gain_ctrl(s, 1);    // Auto gain on
      Serial.println("OV3660 sensor tuning applied (vflip, brightness, saturation).");
    } else if (s) {
      Serial.printf("Warning: expected OV3660_PID but got PID=0x%x\n", s->id.PID);
    }
  }

  // ── WebSocket relay connection ─────────────────────────────────────────────
  Serial.printf("Connecting WS → ws://%s:%d/robot-video\n", ws_host, ws_port);
  webSocket.begin(ws_host, ws_port, "/robot-video");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);   // Retry every 5 s on drop

  digitalWrite(ledPin, LOW);
  Serial.println("Eye boot complete. Streaming frames on /robot-video.");
  Serial.println("=======================================================");
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
  webSocket.loop();   // Must be called frequently for connection maintenance

  static unsigned long lastFrameTime = 0;
  const unsigned long now = millis();

  if ((now - lastFrameTime >= FRAME_INTERVAL_MS) && webSocket.isConnected()) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) {
      if (fb->format == PIXFORMAT_JPEG) {
        webSocket.sendBIN(fb->buf, fb->len);
      } else {
        // Fallback software JPEG conversion (non-JPEG pixel formats)
        uint8_t* jpgBuf = nullptr;
        size_t   jpgLen = 0;
        if (frame2jpg(fb, 25, &jpgBuf, &jpgLen)) {
          webSocket.sendBIN(jpgBuf, jpgLen);
          free(jpgBuf);
        }
      }
      esp_camera_fb_return(fb);
    }
    lastFrameTime = now;
  }

  delay(1);   // Feed watchdog
}
