# 🏎️ Elegoo V4 Internet Teleoperation (ESP32-S3)

<p align="center">
  <strong>Turn your local Wi-Fi toy into an ultra-low-latency, internet-accessible teleoperated WAN rover.</strong>
</p>

<p align="center">
  <a href="https://github.com/RorriMaesu/elegoo-v4-internet-teleoperation-esp32s3/stargazers"><img src="https://img.shields.io/github/stars/RorriMaesu/elegoo-v4-internet-teleoperation-esp32s3?style=for-the-badge&color=blue" alt="Stars"></a>
  <a href="https://github.com/RorriMaesu/elegoo-v4-internet-teleoperation-esp32s3/blob/main/LICENSE"><img src="https://img.shields.io/github/license/RorriMaesu/elegoo-v4-internet-teleoperation-esp32s3?style=for-the-badge&color=green" alt="License"></a>
  <img src="https://img.shields.io/badge/Platform-ESP32--S3-orange?style=for-the-badge&logo=espressif" alt="ESP32-S3 Platform">
  <img src="https://img.shields.io/badge/Stack-Node.js%20%7C%20C%2B%2B%20%7C%20HTML5-blueviolet?style=for-the-badge" alt="Tech Stack">
  <a href="https://www.buymeacoffee.com/rorrimaesu"><img src="https://img.shields.io/badge/Buy%20Me%20A%20Coffee-Support-FFDD00?style=for-the-badge&logo=buy-me-a-coffee&logoColor=black" alt="Buy Me A Coffee"></a>
</p>

---

## 📐 Systems Engineering & Operational Philosophy

Most hobby robot cars route high-bandwidth video streams and real-time directional controls through a singular microcontroller, over a shared power rail and radio. In long-distance wide-area network (WAN) teleoperation, this creates two fatal bottlenecks:

1. **Inductive Voltage Sag:** DC motors drawing high transient currents under physical load drop the logic voltage rail, triggering immediate Wi-Fi transceivers brownouts, packet fragmentation, or hardware brownouts.
2. **Network Queue Trapping:** High-volume binary JPEG video frames fill the chip's internal RF buffer queue. Time-sensitive directional steering strings get trapped behind image packets, causing severe "rubber-band" control lag.

This framework mitigates these failures via **True Physical Power-Domain and Processing-Domain Isolation**. The workload is completely decoupled across two independent **ESP32-S3** microcontrollers running on separate battery packs over a single unified mechanical chassis.

---

## 🗂️ Production Workspace Layout

The active production workspace is structured for clean modular deployment:

```text
M:\Robot Projects\ElegooRobot\
│
├── Remote_Relay_Server\           <-- Core Node.js Routing Server & Dashboard
│   ├── server.js                  <-- Dual-lane stream multiplexer & TCP frame interceptor
│   └── index.html                 <-- Low-overhead HTML5 Canvas teleoperation UI
│
├── Firmware_Eye_OV3660\           <-- Isolated Vision System Source
│   └── Firmware_Eye_OV3660.ino    <-- Optimized for OV3660; no serial/motor logic
│
└── Firmware_Muscle_OV2640\        <-- Isolated Drivetrain System Source
    └── Firmware_Muscle_OV2640.ino <-- Optimized for OV2640; camera hardware fully disabled
```

---

## ⚡ Hardware Subsystem Specifications

| Specification | 👁️ THE EYE (Isolated Vision Tier) | 🏎️ THE MUSCLE (Isolated Drivetrain Tier) |
| :--- | :--- | :--- |
| **Silicon Platform** | ESP32-S3 Development Module | ESP32-S3 Development Module |
| **Camera Sensor** | **OV3660 Camera Sensor** (Active) | OV2640 (Fully Uninitialized & Disabled) |
| **Power Source** | Dedicated **Battery Pack 2** | Dedicated **Battery Pack 1** (Isolates motor noise) |
| **Base Configuration** | Mounted on secondary UNO mother board (5V mounting rail) | Mounted on primary Elegoo V4 UNO & L298N/L293D shield |
| **Pin Mapping (UART)** | Disconnected / Hardware `Serial2` unpopulated | Onboard CAM switch active; UART RX=3, TX=40 to UNO board |
| **Connection Endpoint** | `ws://[Server_IP]:3000/robot-video` | `ws://[Server_IP]:3000/robot-control` |
| **RF / Power Settings** | `WiFi.setSleep(false);` & `WiFi.setTxPower(WIFI_POWER_19_5dBm);` | Camera initialization code omitted to optimize thermal draw |
| **Frame Rates** | Non-blocking clock throttled to ~22 FPS (~45ms interval) | High-frequency asynchronous event loop |

---

## 🔄 Network Routing Architecture

The Node.js relay server acts as an internet traffic cop running on **Port 3000**, communicating through a secure Cloudflare Quick Tunnel wrapper.

```text
               ┌─── [ Web Dashboard UI ] ───┐
 (Receives Video Feed)                      (Sends Drive Commands)
         ▲                                          │
   /video-stream                              /control-stream
         │                                          ▼
   ┌─────────── [ Node.js Relay Server (Port 3000) ] ───────────┐
   │                                                            │
   ▲ (/robot-video endpoint)        (/robot-control endpoint) ──▼
   │                                                            │
[System 1: OV3660 "The Eye"]                 [System 2: OV2640 "The Muscle"]
```

---

## 🛑 Critical Programming Boundaries (Must Preserve)

The following server-side and client-side custom code blocks are mission-critical. They resolve underlying hardware and library protocol anomalies and must be preserved during any subsequent file refactors:

### 1. Raw TCP Socket Interceptor (`installEyeRSVPatcher`)
Under high-volume image loops, the ESP32 third-party WebSocket library occasionally sets reserved protocol header bits (RSV2 and RSV3) inside individual frame packages. Standard Node.js WebSocket libraries (such as `ws` 8.x) treat these as a framing violation and immediately terminate the connection (`Invalid WebSocket frame: RSV2 and RSV3 must be clear`).
* **The Solution in `server.js`:** A custom middleware hook (`installEyeRSVPatcher`) intercepts raw data buffers coming out of the Eye's underlying TCP socket *before* they hit the high-level WebSocket parser. It programmatically sweeps through the byte arrays and clears bits 4 and 5 (`chunk[pos] &= 0xCF`) from header byte 0 of every frame, completely masking out the problematic RSV2+RSV3 mask (`0x30`).

### 2. Binary ArrayBuffer HTML5 Canvas Pipeline
To completely bypass browser rendering lag, frame build backlogs, and sudden UI freezes caused by browser engine Garbage Collection (GC) loops, the classic frontend pattern using `URL.createObjectURL` and `URL.revokeObjectURL` has been completely purged.
* **The Solution in `index.html`:** The video websocket communication is cast explicitly to `arraybuffer` format. Incoming data is decoded on the fly into individual binary image arrays and painted directly onto an HTML5 `<canvas>` rendering target. This layout extracts the end-to-end millisecond timestamps embedded into the binary frame wrapper, exposing a real-time `Frame Age @ Render` telemetry counter on the dashboard interface.

### 3. Stateful Control Validation & Disconnect Safety Gate
The web dashboard enforces an event-locked state model. UI keydown triggers an explicit `start` command, while keyup fires a deterministic `stop` action. To prevent a vehicle from running away if a connection drops mid-stride, a server-side disconnect hook tracks the `muscleSocket` connection health. If the socket closes or experiences an unexpected drop, the server bypasses the UI and automatically transmits a hardwired wire stop (`S`) straight down to the locomotion board.

---

## 🚀 Quick Start Guide

### 1. Spin up the Node.js Relay Server
```bash
cd Remote_Relay_Server
npm install
npm start
```

### 2. Expose the Server using Cloudflare Tunnel
```bash
# Generate public ephemeral edge routing node
npx cloudflared tunnel --url http://localhost:3000
```

### 3. Deploy ESP32-S3 Firmwares
* Flash `Firmware_Eye_OV3660.ino` to **System 1 (Vision ESP32)**.
* Flash `Firmware_Muscle_OV2640.ino` to **System 2 (Drivetrain ESP32)**.
* Ensure both point to your public Cloudflare Tunnel URL!

---

## 🎯 Vetted Technical Roadmap

### Part A: Immediate Tactical Optimizations (MJPEG/WebSockets Pipeline)
* [ ] **Enforce TCP Low-Latency Rules:** Inject `socket.setNoDelay(true);` onto the underlying TCP sockets within `server.js` to completely bypass Nagle's algorithm packet batching delays.
* [ ] **Egress Backpressure Drop Logic:** Add a `dashboardSocket.bufferedAmount > 0` validation inside the relay's video fanout loop to discard old frames and prioritize "latest-frame-wins" execution.
* [ ] **Downscale the JPEG Footprint:** Modify `config.jpeg_quality` within `Firmware_Eye_OV3660.ino` from `25` to `42` to optimize payloads.

### Part B: Deep-Dive Protocol Transitions (Industrial Upgrades)
* [ ] **WebRTC Integration on ESP32-S3:** Migrate from TCP WebSockets to native UDP-based WebRTC to drop end-to-end video age below 200ms across wide-area networks.
* [ ] **Persistent Named Tunnels:** Upgrade from public, ephemeral Cloudflare Quick Tunnels to a static, authenticated Named Tunnel for permanent dedicated edge routing.

---

## ☕ Support the Project

**Has this isolated dual-brain architecture saved your robot from brownouts and telemetry lag?** 🤖⚡

Building custom WAN-teleoperated rovers requires endless hours of hardware troubleshooting, wireless signal tuning, and custom network protocol engineering. If this framework saved you days of debugging and helped you bring your Elegoo Smart Car to life over the internet, consider supporting further development!

Your sponsorship helps fund new hardware testing, upcoming features (like WebRTC migration & real-time IMU telemetry mapping), and keeps this open-source engineering alive.

<p align="center">
  <a href="https://www.buymeacoffee.com/rorrimaesu">
    <img src="https://img.shields.io/badge/Support_My_Work-Buy%20Me%20A%20Coffee-FFDD00?style=for-the-badge&logo=buy-me-a-coffee&logoColor=black" alt="Buy Me A Coffee button" />
  </a>
</p>