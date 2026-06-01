#ifndef DASHBOARD_H
#define DASHBOARD_H

#include <Arduino.h>

// HTML, CSS, and JS Dashboard - Premium Glassmorphism Design
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-select=no">
    <title>Elegoo SmartCar Dashboard</title>
    <link href="https://fonts.googleapis.com/css2?family=Outfit:wght@300;400;600;800&family=Space+Grotesk:wght@400;700&display=swap" rel="stylesheet">
    <style>
        :root {
            --bg-grad: radial-gradient(circle at center, #1b162e 0%, #0d0a15 100%);
            --glass-bg: rgba(255, 255, 255, 0.05);
            --glass-border: rgba(255, 255, 255, 0.08);
            --accent-cyan: #00f0ff;
            --accent-magenta: #ff007f;
            --accent-green: #39ff14;
            --text-main: #f3effa;
            --text-mute: #8e85a6;
        }

        * {
            box-sizing: border-box;
            margin: 0;
            padding: 0;
            font-family: 'Outfit', sans-serif;
            -webkit-user-select: none;
            user-select: none;
        }

        body {
            background: var(--bg-grad);
            color: var(--text-main);
            min-height: 100vh;
            display: flex;
            flex-direction: column;
            align-items: center;
            overflow-x: hidden;
            padding: 20px;
        }

        header {
            margin-bottom: 20px;
            text-align: center;
            width: 100%;
            max-width: 900px;
        }

        h1 {
            font-family: 'Space Grotesk', sans-serif;
            font-size: 2.2rem;
            font-weight: 800;
            background: linear-gradient(135deg, var(--accent-cyan), var(--accent-magenta));
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
            letter-spacing: 2px;
            text-shadow: 0 4px 15px rgba(0, 240, 255, 0.15);
        }

        .subtitle {
            font-size: 0.9rem;
            color: var(--text-mute);
            letter-spacing: 1px;
            margin-top: 5px;
        }

        .main-container {
            display: grid;
            grid-template-columns: 1.3fr 1fr;
            gap: 25px;
            width: 100%;
            max-width: 1000px;
            margin: 0 auto;
        }

        @media (max-width: 900px) {
            .main-container {
                grid-template-columns: 1fr;
            }
        }

        /* Glassmorphic Panel */
        .glass-panel {
            background: var(--glass-bg);
            border: 1px solid var(--glass-border);
            border-radius: 24px;
            backdrop-filter: blur(20px);
            -webkit-backdrop-filter: blur(20px);
            padding: 24px;
            box-shadow: 0 8px 32px 0 rgba(0, 0, 0, 0.3);
            display: flex;
            flex-direction: column;
            align-items: center;
            justify-content: center;
            transition: all 0.3s ease;
        }

        .glass-panel:hover {
            border-color: rgba(255, 255, 255, 0.15);
            box-shadow: 0 12px 40px 0 rgba(0, 240, 255, 0.05);
        }

        /* Video Feed */
        .video-container {
            position: relative;
            width: 100%;
            aspect-ratio: 4/3;
            background: rgba(0, 0, 0, 0.4);
            border-radius: 18px;
            overflow: hidden;
            border: 1px solid rgba(255, 255, 255, 0.05);
            display: flex;
            align-items: center;
            justify-content: center;
        }

        .video-container img {
            width: 100%;
            height: 100%;
            object-fit: cover;
            border-radius: 18px;
        }

        .video-overlay {
            position: absolute;
            top: 15px;
            left: 15px;
            background: rgba(13, 10, 21, 0.7);
            padding: 6px 12px;
            border-radius: 20px;
            border: 1px solid var(--glass-border);
            font-size: 0.8rem;
            display: flex;
            align-items: center;
            gap: 8px;
            pointer-events: none;
        }

        .live-dot {
            width: 8px;
            height: 8px;
            background-color: var(--accent-magenta);
            border-radius: 50%;
            box-shadow: 0 0 8px var(--accent-magenta);
            animation: pulse 1.5s infinite alternate;
        }

        @keyframes pulse {
            0% { transform: scale(0.8); opacity: 0.5; }
            100% { transform: scale(1.2); opacity: 1; }
        }

        /* Telemetry */
        .telemetry-grid {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 15px;
            width: 100%;
            margin-top: 20px;
        }

        .telemetry-item {
            background: rgba(0, 0, 0, 0.2);
            border: 1px solid rgba(255, 255, 255, 0.03);
            border-radius: 16px;
            padding: 15px;
            display: flex;
            flex-direction: column;
        }

        .tel-label {
            font-size: 0.8rem;
            color: var(--text-mute);
            margin-bottom: 5px;
            text-transform: uppercase;
            letter-spacing: 1px;
        }

        .tel-value {
            font-size: 1.2rem;
            font-weight: 600;
            color: #fff;
        }

        .status-ready {
            color: var(--accent-cyan);
            text-shadow: 0 0 10px rgba(0, 240, 255, 0.3);
        }

        .status-active {
            color: var(--accent-green);
            text-shadow: 0 0 10px rgba(57, 255, 20, 0.3);
        }

        /* D-Pad Steering Controls */
        .dpad-container {
            display: grid;
            grid-template-columns: repeat(3, 85px);
            grid-template-rows: repeat(3, 85px);
            gap: 15px;
            justify-content: center;
            align-content: center;
            margin: 20px 0;
        }

        .btn-ctrl {
            background: rgba(255, 255, 255, 0.04);
            border: 1px solid rgba(255, 255, 255, 0.08);
            border-radius: 20px;
            color: var(--text-main);
            display: flex;
            align-items: center;
            justify-content: center;
            cursor: pointer;
            transition: all 0.2s cubic-bezier(0.175, 0.885, 0.32, 1.275);
            outline: none;
            -webkit-tap-highlight-color: transparent;
        }

        .btn-ctrl svg {
            width: 32px;
            height: 32px;
            fill: currentColor;
            transition: transform 0.2s ease;
        }

        /* Hover & Active Glowing States */
        .btn-ctrl:hover {
            background: rgba(255, 255, 255, 0.08);
            color: var(--accent-cyan);
            border-color: rgba(0, 240, 255, 0.3);
            box-shadow: 0 0 15px rgba(0, 240, 255, 0.1);
        }

        .btn-ctrl:active, .btn-ctrl.active {
            background: rgba(0, 240, 255, 0.15);
            color: var(--accent-cyan);
            border-color: var(--accent-cyan);
            box-shadow: 0 0 25px rgba(0, 240, 255, 0.4);
            transform: scale(0.92);
        }

        /* Stop Button (Center of D-Pad) */
        .btn-stop {
            grid-column: 2;
            grid-row: 2;
            background: rgba(255, 0, 127, 0.05);
            border-color: rgba(255, 0, 127, 0.15);
            color: var(--accent-magenta);
        }

        .btn-stop:hover {
            background: rgba(255, 0, 127, 0.1);
            color: var(--accent-magenta);
            border-color: rgba(255, 0, 127, 0.4);
            box-shadow: 0 0 15px rgba(255, 0, 127, 0.1);
        }

        .btn-stop:active, .btn-stop.active {
            background: rgba(255, 0, 127, 0.2);
            color: var(--accent-magenta);
            border-color: var(--accent-magenta);
            box-shadow: 0 0 25px rgba(255, 0, 127, 0.4);
        }

        .btn-up { grid-column: 2; grid-row: 1; }
        .btn-left { grid-column: 1; grid-row: 2; }
        .btn-right { grid-column: 3; grid-row: 2; }
        .btn-down { grid-column: 2; grid-row: 3; }

        /* Keyboard Shortcuts Panel */
        .key-legend {
            font-size: 0.8rem;
            color: var(--text-mute);
            text-align: center;
            margin-top: 15px;
            padding: 10px;
            border-radius: 12px;
            background: rgba(0, 0, 0, 0.1);
            border: 1px solid rgba(255, 255, 255, 0.02);
            width: 100%;
        }

        .key-badge {
            background: rgba(255, 255, 255, 0.1);
            padding: 2px 6px;
            border-radius: 5px;
            border: 1px solid rgba(255, 255, 255, 0.1);
            color: #fff;
            font-size: 0.75rem;
            font-family: 'Space Grotesk', monospace;
            margin: 0 2px;
        }

        footer {
            margin-top: 30px;
            color: var(--text-mute);
            font-size: 0.8rem;
            text-align: center;
        }
    </style>
</head>
<body>

    <header>
        <h1>ELEGOO SMART CAR V4.0</h1>
        <p class="subtitle">STA MODE CONNECTIVITY &bull; LIVE TELEMETRY</p>
    </header>

    <div class="main-container">
        <!-- Live Video Feed Panel -->
        <div class="glass-panel">
            <div class="video-container">
                <img id="cameraStream" src="" alt="Live Video Feed">
                <div class="video-overlay">
                    <div class="live-dot"></div>
                    <span>OV2640 LIVE</span>
                </div>
            </div>
            
            <div class="telemetry-grid">
                <div class="telemetry-item">
                    <span class="tel-label">System Status</span>
                    <span id="sys-status" class="tel-value status-ready">Ready</span>
                </div>
                <div class="telemetry-item">
                    <span class="tel-label">WiFi Signal</span>
                    <span id="wifi-rssi" class="tel-value">-- dBm</span>
                </div>
                <div class="telemetry-item">
                    <span class="tel-label">Uptime</span>
                    <span id="uptime" class="tel-value">0s</span>
                </div>
                <div class="telemetry-item">
                    <span class="tel-label">Last Command</span>
                    <span id="last-cmd" class="tel-value">STOP</span>
                </div>
            </div>
        </div>

        <!-- Steering Panel -->
        <div class="glass-panel">
            <h3 style="font-family: 'Space Grotesk', sans-serif; letter-spacing: 1px; margin-bottom: 20px;">STEERING PORTAL</h3>
            
            <div class="dpad-container">
                <!-- Forward -->
                <button class="btn-ctrl btn-up" id="btn-F" 
                        onmousedown="sendCmd('F')" onmouseup="sendCmd('S')" onmouseleave="sendCmd('S')"
                        ontouchstart="sendCmd('F')" ontouchend="sendCmd('S')" ontouchcancel="sendCmd('S')">
                    <svg viewBox="0 0 24 24"><path d="M7 14l5-5 5 5H7z"/></svg>
                </button>

                <!-- Left -->
                <button class="btn-ctrl btn-left" id="btn-L" 
                        onmousedown="sendCmd('L')" onmouseup="sendCmd('S')" onmouseleave="sendCmd('S')"
                        ontouchstart="sendCmd('L')" ontouchend="sendCmd('S')" ontouchcancel="sendCmd('S')">
                    <svg viewBox="0 0 24 24"><path d="M14 17l-5-5 5-5v10z"/></svg>
                </button>

                <!-- Stop -->
                <button class="btn-ctrl btn-stop" id="btn-S" onclick="sendCmd('S')">
                    <svg viewBox="0 0 24 24"><rect x="6" y="6" width="12" height="12" rx="2"/></svg>
                </button>

                <!-- Right -->
                <button class="btn-ctrl btn-right" id="btn-R" 
                        onmousedown="sendCmd('R')" onmouseup="sendCmd('S')" onmouseleave="sendCmd('S')"
                        ontouchstart="sendCmd('R')" ontouchend="sendCmd('S')" ontouchcancel="sendCmd('S')">
                    <svg viewBox="0 0 24 24"><path d="M10 17l5-5-5-5v10z"/></svg>
                </button>

                <!-- Backward -->
                <button class="btn-ctrl btn-down" id="btn-B" 
                        onmousedown="sendCmd('B')" onmouseup="sendCmd('S')" onmouseleave="sendCmd('S')"
                        ontouchstart="sendCmd('B')" ontouchend="sendCmd('S')" ontouchcancel="sendCmd('S')">
                    <svg viewBox="0 0 24 24"><path d="M7 10l5 5 5-5H7z"/></svg>
                </button>
            </div>

            <div class="key-legend">
                <span>Keyboard controls enabled:</span><br>
                <div style="margin-top: 5px;">
                    <span class="key-badge">W</span> / <span class="key-badge">&uarr;</span> Forward &bull;
                    <span class="key-badge">S</span> / <span class="key-badge">&darr;</span> Backward<br>
                    <span class="key-badge">A</span> / <span class="key-badge">&larr;</span> Left &bull;
                    <span class="key-badge">D</span> / <span class="key-badge">&rarr;</span> Right &bull;
                    <span class="key-badge">Space</span> Stop
                </div>
            </div>
        </div>
    </div>

    <footer>
        <p>Advanced Agentic Firmware &bull; Antigravity 2026</p>
    </footer>

    <script>
        // Start Video Stream from Port 81
        const streamUrl = window.location.protocol + '//' + window.location.hostname + ':81/stream';
        document.getElementById('cameraStream').src = streamUrl;

        // Command processing
        let activeCmd = 'S';
        const statusEl = document.getElementById('sys-status');
        const lastCmdEl = document.getElementById('last-cmd');

        function sendCmd(cmd) {
            if (cmd === activeCmd && cmd !== 'S') return; // Prevent spamming duplicate move commands
            
            // Visual feedback on button states
            clearButtonActives();
            if (cmd !== 'S') {
                const btn = document.getElementById('btn-' + cmd);
                if (btn) btn.classList.add('active');
                statusEl.innerText = "Driving";
                statusEl.className = "tel-value status-active";
            } else {
                const btnStop = document.getElementById('btn-S');
                if (btnStop) btnStop.classList.add('active');
                setTimeout(() => btnStop.classList.remove('active'), 150);
                statusEl.innerText = "Ready";
                statusEl.className = "tel-value status-ready";
            }

            activeCmd = cmd;
            lastCmdEl.innerText = getCommandName(cmd);

            fetch('/control?cmd=' + cmd)
                .catch(err => {
                    console.error("Command failed:", err);
                    statusEl.innerText = "Error";
                    statusEl.className = "tel-value";
                });
        }

        function clearButtonActives() {
            ['F', 'B', 'L', 'R', 'S'].forEach(c => {
                const btn = document.getElementById('btn-' + c);
                if (btn) btn.classList.remove('active');
            });
        }

        function getCommandName(cmd) {
            switch(cmd) {
                case 'F': return "FORWARD";
                case 'B': return "BACKWARD";
                case 'L': return "TURN LEFT";
                case 'R': return "TURN RIGHT";
                case 'S': return "STOPPED";
                default: return "STOPPED";
            }
        }

        // Telemetry Update Loop
        function updateTelemetry() {
            fetch('/telemetry')
                .then(res => res.json())
                .then(data => {
                    document.getElementById('wifi-rssi').innerText = data.rssi + " dBm";
                    document.getElementById('uptime').innerText = formatUptime(data.uptime);
                })
                .catch(err => {
                    console.warn("Telemetry offline");
                });
        }

        function formatUptime(sec) {
            if (sec < 60) return sec + "s";
            const m = Math.floor(sec / 60);
            const s = sec % 60;
            return m + "m " + s + "s";
        }

        setInterval(updateTelemetry, 3000);
        updateTelemetry();

        // Keyboard Controls
        const keyMap = {
            'w': 'F', 'ArrowUp': 'F',
            's': 'B', 'ArrowDown': 'B',
            'a': 'L', 'ArrowLeft': 'L',
            'd': 'R', 'ArrowRight': 'R',
            ' ': 'S'
        };

        let pressedKeys = new Set();

        window.addEventListener('keydown', (e) => {
            if (e.repeat) return;
            const mapped = keyMap[e.key] || keyMap[e.key.toLowerCase()];
            if (mapped) {
                pressedKeys.add(e.key);
                sendCmd(mapped);
            }
        });

        window.addEventListener('keyup', (e) => {
            pressedKeys.delete(e.key);
            // If no driving keys are pressed, stop the robot
            const drivingKeysPressed = Array.from(pressedKeys).some(k => {
                const mapped = keyMap[k] || keyMap[k.toLowerCase()];
                return mapped && mapped !== 'S';
            });
            if (!drivingKeysPressed) {
                sendCmd('S');
            }
        });
    </script>
</body>
</html>
)rawliteral";

#endif
