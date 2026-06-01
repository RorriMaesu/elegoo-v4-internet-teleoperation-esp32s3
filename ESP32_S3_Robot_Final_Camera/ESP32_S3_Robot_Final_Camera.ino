#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include "esp_http_server.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <WebSocketsClient.h> // Markus Sattler's WebSockets library

// Include the local dashboard HTML/JS definition
#include "dashboard.h"

// WiFi credentials
const char* ssid = "MySpectrumWiFi47-2G";
const char* password = "finishoasis957";

// Relay Server IP and Port (Local PC hosting Node.js backend)
const char* ws_host = "192.168.1.213";
const int ws_port = 3000;

// Led Pin for visual feedback (GPIO 46 on Elegoo ESP32-S3 camera module)
const int ledPin = 46;

// Serial commands for robot control
#define CMD_FORWARD   'F'
#define CMD_BACKWARD  'B'
#define CMD_LEFT      'L'
#define CMD_RIGHT     'R'
#define CMD_STOP      'S'

// ESP32-S3 Camera Pin Definition (SmartCar ESP32S3 Camera V1.0) - Official ESP32-S3-EYE Mappings
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     15
#define SIOD_GPIO_NUM     4
#define SIOC_GPIO_NUM     5
#define Y9_GPIO_NUM       16
#define Y8_GPIO_NUM       17
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       12
#define Y5_GPIO_NUM       10
#define Y4_GPIO_NUM       8
#define Y3_GPIO_NUM       9
#define Y2_GPIO_NUM       11
#define VSYNC_GPIO_NUM    6
#define HREF_GPIO_NUM     7
#define PCLK_GPIO_NUM     13

// Remote Mode vs Local AP Mode
bool isRemoteMode = false;
WebSocketsClient webSocket;

// Local Mode Web and Stream servers
WebServer server(80);
httpd_handle_t stream_httpd = NULL;

// Stream boundary definition for local MJPEG
#define PART_BOUNDARY "123456789000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// Handler for local MJPEG streaming (AP mode)
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  char part_buf[128];
  
  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) {
    return res;
  }
  
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  
  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
    } else {
      if (fb->format != PIXFORMAT_JPEG) {
        bool jpeg_converted = frame2jpg(fb, 25, &_jpg_buf, &_jpg_buf_len);
        esp_camera_fb_return(fb);
        fb = NULL;
        if (!jpeg_converted) {
          Serial.println("JPEG compression failed");
          res = ESP_FAIL;
        }
      } else {
        _jpg_buf_len = fb->len;
        _jpg_buf = fb->buf;
      }
    }
    
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }
    
    if (res == ESP_OK) {
      size_t hlen = snprintf(part_buf, 128, _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    
    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    } else if (_jpg_buf) {
      free(_jpg_buf);
      _jpg_buf = NULL;
    }
    
    if (res != ESP_OK) {
      break;
    }
  }
  return res;
}

// Start local camera streaming server on Port 81 (AP mode)
void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 81; 
  config.ctrl_port = 32769; // Use unique control port to avoid conflicts
  
  httpd_uri_t stream_uri = {
    .uri       = "/stream",
    .method    = HTTP_GET,
    .handler   = stream_handler,
    .user_ctx  = NULL
  };
  
  Serial.printf("Starting local stream server on port: %d\n", config.server_port);
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
    Serial.println("Local stream server active on Port 81");
  } else {
    Serial.println("Failed to start local stream server");
  }
}

// Send commands to Uno motherboard via Serial2 (Pins 3/40)
void sendCommandToArduino(char cmd) {
  Serial2.write(cmd);
  Serial.printf("Forwarding command to Uno: '%c'\n", cmd);
}

void handleDriveCommandPayload(const String& incomingPayload) {
  String msg = incomingPayload;
  msg.trim();
  if (msg.length() == 0) {
    return;
  }

  if (msg.length() == 1) {
    char legacyCmd = msg.charAt(0);
    if (legacyCmd == CMD_FORWARD || legacyCmd == CMD_BACKWARD || legacyCmd == CMD_LEFT || legacyCmd == CMD_RIGHT || legacyCmd == CMD_STOP) {
      sendCommandToArduino(legacyCmd);
    }
    return;
  }

  if (!msg.startsWith("drive:")) {
    Serial.printf("[WS] Unsupported command payload: %s\n", msg.c_str());
    return;
  }

  int commaPos = msg.indexOf(',');
  if (commaPos < 0) {
    Serial.printf("[WS] Malformed drive command (missing comma): %s\n", msg.c_str());
    return;
  }

  String direction = msg.substring(6, commaPos);
  String state = msg.substring(commaPos + 1);
  direction.toLowerCase();
  state.toLowerCase();

  if (state == "stop") {
    sendCommandToArduino(CMD_STOP);
    return;
  }

  if (state != "start") {
    Serial.printf("[WS] Unsupported drive state: %s\n", state.c_str());
    return;
  }

  if (direction == "forward") {
    sendCommandToArduino(CMD_FORWARD);
  } else if (direction == "backward") {
    sendCommandToArduino(CMD_BACKWARD);
  } else if (direction == "left") {
    sendCommandToArduino(CMD_LEFT);
  } else if (direction == "right") {
    sendCommandToArduino(CMD_RIGHT);
  } else if (direction == "all") {
    sendCommandToArduino(CMD_STOP);
  } else {
    Serial.printf("[WS] Unsupported drive direction: %s\n", direction.c_str());
  }
}

// WebSocket Event Handler (STA mode)
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.println("[WS] Disconnected from relay server");
      // Blink LED slowly on connection loss
      digitalWrite(ledPin, LOW);
      break;
    case WStype_CONNECTED:
      Serial.printf("[WS] Connected to relay server URL: %s\n", payload);
      // Solid ON LED on successful connection to server
      digitalWrite(ledPin, HIGH);
      break;
    case WStype_TEXT:
      if (length > 0) {
        String msg;
        msg.reserve(length + 1);
        for (size_t i = 0; i < length; i++) {
          msg += static_cast<char>(payload[i]);
        }
        handleDriveCommandPayload(msg);
      }
      break;
    case WStype_BIN:
      // We do not expect incoming binary data from clients
      break;
    default:
      break;
  }
}

const char* wifiStatusToString(wl_status_t status) {
  switch (status) {
    case WL_NO_SHIELD: return "WL_NO_SHIELD";
    case WL_IDLE_STATUS: return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL: return "WL_NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED: return "WL_SCAN_COMPLETED";
    case WL_CONNECTED: return "WL_CONNECTED";
    case WL_CONNECT_FAILED: return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED: return "WL_DISCONNECTED";
    default: return "WL_UNKNOWN";
  }
}

void setup() {
  // Disable brownout detector inside safe startup code block
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  
  // Initialize status LED
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, HIGH); // Turn LED ON during initialization
  
  // Initialize USB-CDC console serial
  Serial.begin(115200);
  
  // Initialize Serial2 for Uno motherboard (RX=3, TX=40)
  Serial2.begin(115200, SERIAL_8N1, 3, 40);
  
  delay(1500);
  
  Serial.println("\n\n=======================================================");
  Serial.println("   ELEGOO SMART CAR V4.0 - DUAL TELEOP FIRMWARE BOOT   ");
  Serial.println("=======================================================");
  Serial.printf("OPI PSRAM Detected: %s\n", psramFound() ? "YES" : "NO");
  Serial.printf("Configured WiFi SSID: '%s' (length=%u)\n", ssid, (unsigned)String(ssid).length());
  
  // Scan Wi-Fi to diagnose signal strength
  Serial.println("Scanning Wi-Fi networks...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  int numNetworks = WiFi.scanNetworks();
  Serial.printf("Found %d networks\n", numNetworks);
  
  // Connect to Router SSID
  Serial.printf("Connecting to Router SSID: '%s'\n", ssid);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);
  
  // 10 seconds timeout for fast AP fallback
  int wifiTimeout = 20; // 20 * 500ms = 10s
  while (WiFi.status() != WL_CONNECTED && wifiTimeout > 0) {
    wl_status_t currentStatus = WiFi.status();
    Serial.printf("WiFi.status() = %d (%s)\n", (int)currentStatus, wifiStatusToString(currentStatus));
    digitalWrite(ledPin, !digitalRead(ledPin)); // Blink LED during Wi-Fi connect
    delay(500);
    wifiTimeout--;
  }

  wl_status_t finalStatus = WiFi.status();
  Serial.printf("Final WiFi.status() = %d (%s)\n", (int)finalStatus, wifiStatusToString(finalStatus));
  
  if (WiFi.status() == WL_CONNECTED) {
    isRemoteMode = true;
    digitalWrite(ledPin, HIGH); // Solid ON for Wi-Fi connected
    Serial.println("\nWiFi Connected successfully!");
    Serial.print("DHCP IP Address: ");
    Serial.println(WiFi.localIP());
      WiFi.setSleep(false);
      WiFi.setTxPower(WIFI_POWER_19_5dBm);
      Serial.println("WiFi modem sleep disabled for low-jitter streaming");
      Serial.println("WiFi TX power set to 19.5 dBm (max)");
  } else {
    isRemoteMode = false;
    Serial.println("\nWiFi Connection timed out! Starting Local AP Fallback Mode...");
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Elegoo-SmartCar", "");
    Serial.println("Hotspot 'Elegoo-SmartCar' activated!");
    Serial.print("Access Point IP: ");
    Serial.println(WiFi.softAPIP());
  }
  
  // Camera Setup
  Serial.println("Initializing OV2640/OV3660 Camera Sensor...");
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  
  config.xclk_freq_hz = 20000000;      // 20MHz for stable clock alignment
  config.pixel_format = PIXFORMAT_JPEG;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality = 25;
  
  if (psramFound()) {
    config.frame_size = FRAMESIZE_QVGA; // 320x240 resolution for WAN latency
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
    Serial.println("PSRAM Enabled: QVGA (320x240) with CAMERA_GRAB_LATEST");
  } else {
    config.frame_size = FRAMESIZE_QVGA;
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    Serial.println("PSRAM Not Found: QVGA (320x240)");
  }
  
  esp_err_t err = esp_camera_init(&config);
  if (err == ESP_OK) {
    Serial.println("Camera initialized successfully!");
    
    sensor_t * s = esp_camera_sensor_get();
    if (s) {
      if (s->id.PID == OV3660_PID) {
        s->set_vflip(s, 1);        // Flip vertical orientation for OV3660
        s->set_brightness(s, 1);   // Brighten slightly
        s->set_saturation(s, -2);  // Lower saturation for natural colors
      } else {
        s->set_brightness(s, 1);   // Brighten slightly for OV2640
        s->set_contrast(s, 0);
        s->set_saturation(s, 0);
      }
      s->set_whitebal(s, 1);
      s->set_awb_gain(s, 1);
      s->set_exposure_ctrl(s, 1);
      s->set_gain_ctrl(s, 1);
    }
  } else {
    Serial.printf("Camera initialization failed! Error: 0x%x\n", err);
    // Blink LED to signal error
    for (int i = 0; i < 8; i++) {
      digitalWrite(ledPin, HIGH); delay(70);
      digitalWrite(ledPin, LOW); delay(70);
    }
  }
  
  // Initialize communication modes
  if (isRemoteMode) {
    // Connect to WebSocket Relay Server on PC
    Serial.printf("Connecting to WebSocket relay: ws://%s:%d/robot\n", ws_host, ws_port);
    webSocket.begin(ws_host, ws_port, "/robot");
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000); // Reconnect every 5 seconds if dropped
  } else {
    // Start Local Web Server on Port 80
    server.on("/", HTTP_GET, []() {
      server.send_P(200, "text/html", index_html);
    });
    
    server.on("/control", HTTP_GET, []() {
      String cmdArg = server.arg("cmd");
      if (cmdArg.length() > 0) {
        char cmd = cmdArg.charAt(0);
        sendCommandToArduino(cmd);
        server.send(200, "text/plain", "ACK");
      } else {
        server.send(400, "text/plain", "Missing cmd parameter");
      }
    });
    
    server.on("/telemetry", HTTP_GET, []() {
      String json = "{\"rssi\":0,\"uptime\":" + String(millis() / 1000) + "}";
      server.send(200, "application/json", json);
    });
    
    server.begin();
    Serial.println("Local Control Web Server active on Port 80");
    
    // Start local camera stream on Port 81
    startCameraServer();
  }
  
  // Initialization complete: turn status LED off
  digitalWrite(ledPin, LOW);
  Serial.println("System Boot Complete!");
}

void loop() {
  if (isRemoteMode) {
    // Run WebSocket Client loop
    webSocket.loop();
    
    // Capture and stream frames to WebSocket
    static unsigned long lastFrameTime = 0;
    unsigned long now = millis();
    
      static const unsigned long FRAME_INTERVAL_MS = 100; // ~10 FPS — reduces WAN TCP backlog
      if (now - lastFrameTime >= FRAME_INTERVAL_MS) {
      if (webSocket.isConnected()) {
        camera_fb_t * fb = esp_camera_fb_get();
        if (fb) {
          if (fb->format == PIXFORMAT_JPEG) {
            webSocket.sendBIN(fb->buf, fb->len);
          } else {
            uint8_t * _jpg_buf = NULL;
            size_t _jpg_buf_len = 0;
              if (frame2jpg(fb, 25, &_jpg_buf, &_jpg_buf_len)) {
              webSocket.sendBIN(_jpg_buf, _jpg_buf_len);
              free(_jpg_buf);
            }
          }
          esp_camera_fb_return(fb);
        }
      }
      lastFrameTime = now;
    }
  } else {
    // Local AP Mode: handle client requests
    server.handleClient();
  }
  
  // Forward physical UART motherboard status updates to USB console
  while (Serial2.available() > 0) {
    char inChar = Serial2.read();
    Serial.print("Uno Board Status Update: ");
    Serial.write(inChar);
    Serial.println();
  }
  
  delay(1); // Feed WDT
}
