/*
 * ================================================================
 *  DUAL-BRAIN EYE FIRMWARE — Firmware_Eye_OV3660.ino (HTTP Version)
 * ================================================================
 *  Board  : ESP32-S3 WROOM (Elegoo SmartCar V4.0 camera module)
 *  Sensor : OV3660
 *  Role   : Native HTTP MJPEG Streamer (First Hop)
 *  Power  : Battery Pack 2 → second Arduino UNO (bracket + 5V rail)
 *
 *  Compile Configurations:
 *    Board           : ESP32S3 Dev Module
 *    USB CDC On Boot : Enabled
 *    Flash Size      : 8MB (64Mb)
 *    Partition Scheme: Huge APP (3MB No OTA)
 *    PSRAM           : OPI PSRAM
 * ================================================================
 */

#include "esp_camera.h"
#include "esp_http_server.h"
#include <WiFi.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ─── Network credentials ─────────────────────────────────────────────────────
const char* ssid     = "MySpectrumWiFi47-2G";
const char* password = "finishoasis957";

// ─── Camera pin mapping (CAMERA_MODEL_ESP32S3_EYE) ───────────────────────────
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

const int ledPin = 46;

// HTTP Server Handle
httpd_handle_t stream_httpd = NULL;

// Multipart boundary for MJPEG stream
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

/**
 * @brief Sets up the camera with the specified configuration.
 */
bool setupCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000;         // 10 MHz (stabilised DMA timing)
  config.pixel_format = PIXFORMAT_JPEG;   // for streaming
  config.frame_size   = FRAMESIZE_VGA;    // 640 x 480 (sharp, high-res canvas)
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 16;               // Stabilised premium quality (keeps frames in buffer boundaries)
  config.fb_count     = 2;

  if (psramFound()) {
    config.fb_count  = 3;                 // Triple-Buffering
    config.grab_mode = CAMERA_GRAB_LATEST; // Fresh frames only
    Serial.println("[Eye] PSRAM detected: fb_count=3, CAMERA_GRAB_LATEST");
  } else {
    config.fb_count  = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location = CAMERA_FB_IN_DRAM;
    Serial.println("[Eye] WARNING: No PSRAM, falling back to SRAM!");
  }

  // Initialize camera
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[Eye] Camera init failed: 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);        // Correct inverted mounting
    s->set_brightness(s, 1);   // Boost brightness
    s->set_saturation(s, -2);  // Avoid oversaturation
  }

  // Ensure frame size is locked to VGA
  s->set_framesize(s, FRAMESIZE_VGA);

  Serial.println("[Eye] Camera initialized OK.");
  return true;
}

/**
 * @brief HTTP GET stream handler that pushes raw JPEG frames as multipart/x-mixed-replace.
 */
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  char part_buf[64];

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if(res != ESP_OK) {
    return res;
  }

  // Send initial boundary
  res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
  if(res != ESP_OK) {
    return res;
  }

  unsigned long lastFrameTime = 0;
  const unsigned long FRAME_INTERVAL_MS = 25; // 40 FPS cap (Unchoked)

  Serial.println("[Eye] Client connected to HTTP /stream. Starting transmission...");

  while(true) {
    unsigned long now = millis();
    if (now - lastFrameTime >= FRAME_INTERVAL_MS) {
      fb = esp_camera_fb_get();
      if (!fb) {
        Serial.println("[Eye] Camera capture failed");
        res = ESP_FAIL;
      } else {
        _jpg_buf_len = fb->len;
        _jpg_buf = fb->buf;
      }

      if(res == ESP_OK) {
        size_t hlen = snprintf(part_buf, sizeof(part_buf), _STREAM_PART, _jpg_buf_len);
        res = httpd_resp_send_chunk(req, part_buf, hlen);
      }
      if(res == ESP_OK) {
        res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
      }
      if(res == ESP_OK) {
        res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
      }

      if(fb) {
        esp_camera_fb_return(fb);
        fb = NULL;
        _jpg_buf = NULL;
      }

      if(res != ESP_OK) {
        Serial.printf("[Eye] Send failed or client disconnected: %d\n", res);
        break;
      }
      lastFrameTime = now;
    }
    // Yield to allow background systems and networking stack to run
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  Serial.println("[Eye] Stream transmission finished.");
  return res;
}

/**
 * @brief Starts the lightweight Espressif HTTP Web Server on Port 80.
 */
void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;      // High control port for internal signals
  config.stack_size = 32768;     // Expanded stack size to prevent VGA payload overflow
  config.task_priority = 5;      // Priority
  config.core_id = 0;            // Pin server task to Core 0 (WiFi / camera Core)

  httpd_uri_t stream_uri = {
    .uri       = "/stream",
    .method    = HTTP_GET,
    .handler   = stream_handler,
    .user_ctx  = NULL
  };

  Serial.println("[Eye] Launching native Espressif HTTP Server on Port 80...");
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
    Serial.println("[Eye] HTTP Server started successfully.");
    Serial.println("[Eye] Stream endpoint live at: http://<ip>:80/stream");
  } else {
    Serial.println("[Eye] Critical: Failed to start HTTP server.");
  }
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Disable brownout detector

  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, HIGH); // LED ON during setup

  Serial.begin(115200);
  delay(1500);

  Serial.println("\n=======================================================");
  Serial.println("     DUAL-BRAIN EYE FIRMWARE — NATIVE HTTP MJPEG SERVER ");
  Serial.println("=======================================================");
  Serial.printf("OPI PSRAM detected: %s\n", psramFound() ? "YES" : "NO");

  // WiFi Connection
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);

  Serial.printf("[Eye] Connecting to '%s'...\n", ssid);
  int wifiTimeout = 20; // 10 seconds timeout
  while (WiFi.status() != WL_CONNECTED && wifiTimeout-- > 0) {
    digitalWrite(ledPin, !digitalRead(ledPin));
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false);                 // RF hardening: no sleep
    WiFi.setTxPower(WIFI_POWER_19_5dBm);  // RF hardening: max TX power
    Serial.printf("[Eye] WiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
    digitalWrite(ledPin, HIGH);           // Solid LED on successful connect
  } else {
    Serial.println("[Eye] WiFi Connection FAILED! Halting.");
    while (true) {
      digitalWrite(ledPin, HIGH); delay(150);
      digitalWrite(ledPin, LOW);  delay(150);
    }
  }

  // Camera setup
  if (!setupCamera()) {
    Serial.println("[Eye] Camera setup failed. Halting.");
    while (true);
  }

  // Start HTTP Stream Server
  startCameraServer();

  digitalWrite(ledPin, LOW); // Setup complete, blink off
  Serial.println("[Eye] Setup complete! HTTP MJPEG server is live.");
  Serial.println("=======================================================");
}

void loop() {
  // Free the loop task RAM (we operate 100% inside native Espressif server core tasks)
  delay(1000);
  vTaskDelete(NULL);
}
