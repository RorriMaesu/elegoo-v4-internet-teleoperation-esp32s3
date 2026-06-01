# 🏎️ Elegoo V4 Internet Teleoperation (ESP32-S3)

<p align="center">
  <strong>Turn your local Wi-Fi toy into an ultra-low-latency, internet-accessible teleoperated WAN rover using a state-of-the-art dual-brain WebRTC pipeline.</strong>
</p>

<p align="center">
  <a href="https://github.com/RorriMaesu/elegoo-v4-internet-teleoperation-esp32s3/stargazers"><img src="https://img.shields.io/github/stars/RorriMaesu/elegoo-v4-internet-teleoperation-esp32s3?style=for-the-badge&color=blue" alt="Stars"></a>
  <a href="https://github.com/RorriMaesu/elegoo-v4-internet-teleoperation-esp32s3/blob/main/LICENSE"><img src="https://img.shields.io/github/license/RorriMaesu/elegoo-v4-internet-teleoperation-esp32s3?style=for-the-badge&color=green" alt="License"></a>
  <img src="https://img.shields.io/badge/Platform-ESP32--S3-orange?style=for-the-badge&logo=espressif" alt="ESP32-S3 Platform">
  <img src="https://img.shields.io/badge/Stack-Node.js%20%7C%20WebRTC%20%7C%20FFmpeg-blueviolet?style=for-the-badge" alt="Tech Stack">
  <a href="https://www.buymeacoffee.com/rorrimaesu"><img src="https://img.shields.io/badge/Buy%20Me%20A%20Coffee-Support-FFDD00?style=for-the-badge&logo=buy-me-a-coffee&logoColor=black" alt="Buy Me A Coffee"></a>
</p>

---

## 📐 Systems Engineering & Operational Philosophy

Most hobby robot cars route high-bandwidth video streams and real-time directional controls through a singular microcontroller, over a shared power rail and radio. In long-distance wide-area network (WAN) teleoperation, this creates two fatal bottlenecks:

1. **Inductive Voltage Sag:** DC motors drawing high transient currents under physical load drop the logic voltage rail, triggering immediate Wi-Fi transceiver brownouts, packet fragmentation, or hardware brownouts.
2. **Network Queue Trapping:** High-volume binary JPEG video frames fill the chip's internal RF buffer queue. Time-sensitive directional steering strings get trapped behind image packets, causing severe "rubber-band" control lag.

This framework mitigates these failures via **True Physical Power-Domain and Processing-Domain Isolation**. The workload is completely decoupled across two independent **ESP32-S3** microcontrollers running on separate battery packs over a single unified mechanical chassis.

---

## 🌐 De-Coupled WebRTC & Control Architecture

To achieve sub-100ms low-latency teleoperation over wide-area networks, the vision system utilizes a **Two-Hop WebRTC Gateway** that transcodes raw camera captures into browser-compatible H.264 video streams on the fly:

```
                  ┌─── [ Web Dashboard Cockpit ] ───┐
                  │                                  │
    (Receives H.264 WebRTC)               (Sends Drive Commands)
                  ▲                                  │
          /api/whep (WHEP)                    /control-stream (WS)
                  │                                  ▼
      ┌───────────┴─ [ Node.js Relay Server (Port 3000) ] ───────────┐
      │                                                              │
      │   (SDP Proxy)                                                │
      ▼                                                              │
┌────────────── [ go2rtc Gateway (Port 1984) ] ──────────────┐       │
│                                                            │       │
│  - Ingests HTTP MJPEG from The Eye                         │       │
│  - Transcodes MJPEG to H.264 via FFmpeg                    │       │
│  - Exposes active WebRTC WHEP signalling endpoint          │       │
└─────────────────────────────┬──────────────────────────────┘       │
                              ▲                                      │
                              │ (Local HTTP MJPEG Stream)            │
                              │                                      │
               [ System 1: "The Eye" ]                [ System 2: "The Muscle" ]
                (ESP32-S3 HTTP Server)                 (ESP32-S3 WS Client)
                    192.168.1.214                          192.168.1.38
```

1. **First Hop (Local Wi-Fi):** **The Eye (ESP32-S3)** runs a high-performance native Espressif HTTP server on Port 80, serving an un-choked VGA (`640x480`) MJPEG stream at 40 FPS directly to the Host PC.
2. **Transcoding & WebRTC Ingestion:** **go2rtc** runs as a service on the Host PC, pulling the local camera feed and employing **FFmpeg** to dynamically transcode the MJPEG containers to low-latency H.264 video.
3. **WebRTC WHEP Signaling Proxy:** The Node.js server intercepts WHEP SDP handshake offers (`POST /api/whep`) from the browser and reverse-proxies them to `go2rtc`'s active signaling endpoint on Port 1984, allowing both the HTTP cockpit and UDP WebRTC media to run over a single Cloudflare Edge Tunnel.
4. **Decoupled Locomotion Socket:** **The Muscle (ESP32-S3)** connects directly to the Node.js server via a dedicated control socket (`ws://[Host]:3000/robot-control`) to execute time-sensitive locomotion, remaining completely isolated from the heavy video stream load.

---

## 🗂️ Production Workspace Layout

The active production workspace is structured for clean modular deployment:

```text
M:\Robot Projects\ElegooRobot\
│
├── .tools\                        <-- Core Executables & Gateways
│   ├── cloudflared.exe            <-- Cloudflare Quick Tunnel connector
│   ├── go2rtc.exe                 <-- WebRTC gateway server
│   └── ffmpeg.exe                 <-- Space-free symlinked transcoder
│
├── Remote_Relay_Server\           <-- Core Node.js Routing Server & Dashboard
│   ├── server.js                  <-- WHEP proxy & WebSocket control relay
│   └── go2rtc.yaml                <-- Stream mappings & FFmpeg parameters
│
├── Remote_Control_Dashboard\      <-- Sleek Gamepad UI Cockpit
│   └── index.html                 <-- Decoupled HTML5 WebRTC overlay cockpit
│
├── Firmware_Eye_OV3660\           <-- Isolated Vision System Source
│   └── Firmware_Eye_OV3660.ino    <-- Espressif camera server (Core 0, Port 80)
│
└── Firmware_Muscle_OV2640\        <-- Isolated Drivetrain System Source
    └── Firmware_Muscle_OV2640.ino <-- Drivetrain motor controller (Serial2 to UNO)
```

---

## ⚡ Hardware Subsystem Specifications

| Specification | 👁️ THE EYE (Isolated Vision Tier) | 🏎️ THE MUSCLE (Isolated Drivetrain Tier) |
| :--- | :--- | :--- |
| **Silicon Platform** | ESP32-S3 Development Module | ESP32-S3 Development Module |
| **Camera Sensor** | **OV3660 Camera Sensor** (Active) | OV2640 (Fully Uninitialized & Disabled) |
| **Power Source** | Dedicated **Battery Pack 2** | Dedicated **Battery Pack 1** (Isolates motor noise) |
| **Base Configuration** | Mounted on secondary mother board | Primary mother board + motor shield |
| **Stream Method** | Native Espressif HTTP Server (Port 80) | WebSocket uplink to relay server |
| **Video Format** | HTTP MJPEG -> Transcoded to H.264 WebRTC | No video (UART RX=3, TX=40 to UNO board) |
| **Connection Endpoint** | `http://[Eye_IP]:80/stream` | `ws://[Server_IP]:3000/robot-control` |
| **Frame Parameters** | VGA (`640x480`), `fb_count = 3`, Q=16 | High-frequency asynchronous event loop |
| **Processing Alloc** | 32KB task stack size, 10 MHz DMA clock | Low-overhead WebSocket frame handler |

---

## 🛑 Critical Programming Boundaries (Must Preserve)

The following server-side and client-side custom code blocks are mission-critical. They resolve underlying hardware and library protocol anomalies and must be preserved during any subsequent file refactors:

### 1. WebRTC WHEP Proxy Interceptor (`POST /api/whep`)
To enable secure WAN-grade video streaming through a single Cloudflare Quick Tunnel alongside dashboard telemetry, the Node.js server hosts a custom proxy route. It overrides the `Host` header to `127.0.0.1:1984` to prevent proxy/CORS handshake failures, forwarding browser SDP offers cleanly to `go2rtc`'s WebRTC signaling endpoint (`/api/webrtc?src=robot_eye`).

### 2. Dual-Standard Connection Tracking
The Muscle drivetrain's connection status is monitored inside the Node.js `/status` route and command dispatcher using dual-standard checks that support both `WebSocket.OPEN` and its literal numeric state `1`. This ensures flawless control availability tracking when the locomotion board reboots.

### 3. Decoupled UI State Machine
In `index.html`, the HUD camera overlay (`liveDot` and `streamLabel`) is decoupled from the Muscle drivetrain status loop. Video stream lifecycle tracking is governed strictly by the browser's native `RTCPeerConnection` track state. The locomotion overlay checks `/status` on a separate background polling thread, unlocking the virtual steering overlay without interrupting active video playback.

---

## 🚀 Quick Start Guide

### 1. Spin up the go2rtc Transcoder & Node.js Relay Server
Ensure `ffmpeg.exe` is copied to a folder path without spaces (such as `C:/Users/Tesla/.gemini/antigravity/scratch/ffmpeg.exe`), and update `Remote_Relay_Server/go2rtc.yaml` with its location.

```bash
# Start go2rtc gateway
cd .tools
go2rtc.exe -config "../Remote_Relay_Server/go2rtc.yaml"

# Start Node.js relay server
cd ../Remote_Relay_Server
npm install
npm start
```

### 2. Expose the Server using Cloudflare Tunnel
```bash
npx cloudflared tunnel --url http://localhost:3000
```

### 3. Deploy ESP32-S3 Firmwares
* Flash `Firmware_Eye_OV3660.ino` to **System 1 (Vision ESP32)** on COM4.
* Flash `Firmware_Muscle_OV2640.ino` to **System 2 (Drivetrain ESP32)** on COM3, pointing its `ws_host` configuration to your Host PC's active local IP address.
* Load the public Cloudflare tunnel link on your mobile cockpit and drive!

---

## ☕ Support the Project

Has this isolated dual-brain architecture saved your robot from brownouts and telemetry lag? 🤖⚡

Building custom WAN-teleoperated rovers requires endless hours of hardware troubleshooting, wireless signal tuning, and custom network protocol engineering. If this framework saved you days of debugging and helped you bring your Elegoo Smart Car to life over the internet, consider supporting further development!

<p align="center">
  <a href="https://www.buymeacoffee.com/rorrimaesu">
    <img src="https://img.shields.io/badge/Support_My_Work-Buy%20Me%20A%20Coffee-FFDD00?style=for-the-badge&logo=buy-me-a-coffee&logoColor=black" alt="Buy Me A Coffee button" />
  </a>
</p>