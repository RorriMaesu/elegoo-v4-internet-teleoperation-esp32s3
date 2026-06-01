const express = require('express');
const http = require('http');
const WebSocket = require('ws');
const path = require('path');

const app = express();
app.use(express.json()); // Support JSON payload parsing for remote debugging
const server = http.createServer(app);
// perMessageDeflate explicitly disabled: the ESP32 WebSocketsClient library negotiates
// compression extensions but does not always honour RSV bit rules when the server rejects
// them mid-stream. Stripping the Sec-WebSocket-Extensions header from robot upgrade
// requests at the HTTP layer prevents the RSV2/RSV3 framing errors entirely.
const wss = new WebSocket.Server({ noServer: true, perMessageDeflate: false });

// --- Dual-Brain hardware sockets ---
let eyeSocket    = null;  // The Eye    — camera ESP32  — connects on /robot-video
let muscleSocket = null;  // The Muscle — motor  ESP32  — connects on /robot-control

const videoClientSockets = new Set();
const controlClientSockets = new Set();
const videoClientState = new Map();
const MAX_CLIENT_BUFFERED_BYTES = 32 * 1024;
const STALE_FRAME_AGE_MS = 150;   // drop relay→browser frames older than this (ms)
const MIN_RELAY_SEND_MS = 0;      // Unleashed: completely disable artificial rate-limiting
const FRAME_PACKET_MAGIC = 0x45594531; // 'EYE1'
const FRAME_PACKET_HEADER_BYTES = 24;
const relayStartedAt = Date.now();
const metrics = {
    controlMessagesReceived: 0,
    controlMessagesForwarded: 0,
    controlMessagesInvalid: 0,
    controlMessagesDroppedMuscleOffline: 0,
    controlPingMessages: 0,
    videoFramesReceivedFromRobot: 0,
    videoFramesDroppedByBackpressure: 0,
    videoFramesDroppedByAge: 0,
    videoFramesSentToClients: 0
};

function buildFramePacket(frame) {
    const payload = Buffer.isBuffer(frame.data) ? frame.data : Buffer.from(frame.data);
    const packet = Buffer.allocUnsafe(FRAME_PACKET_HEADER_BYTES + payload.length);
    packet.writeUInt32BE(FRAME_PACKET_MAGIC, 0);
    packet.writeBigInt64BE(BigInt(frame.sourceTsMs), 4);
    packet.writeBigInt64BE(BigInt(Date.now()), 12);
    packet.writeUInt32BE(payload.length, 20);
    payload.copy(packet, FRAME_PACKET_HEADER_BYTES);
    return packet;
}

function parseControlMessage(rawMessage) {
    const rawText = rawMessage.toString().trim();
    let text = rawText;
    let clientTs = null;
    let sequence = null;

    if (rawText.startsWith('{')) {
        try {
            const parsed = JSON.parse(rawText);
            if (parsed && parsed.type === 'ping') {
                return {
                    kind: 'ping',
                    sequence: Number.isFinite(parsed.seq) ? parsed.seq : null,
                    clientTs: Number.isFinite(parsed.ts) ? parsed.ts : null
                };
            }

            if (parsed && parsed.type === 'control' && typeof parsed.command === 'string') {
                text = parsed.command.trim();
                clientTs = Number.isFinite(parsed.clientTs) ? parsed.clientTs : null;
                sequence = Number.isFinite(parsed.seq) ? parsed.seq : null;
            }
        } catch (_err) {
            // If JSON parsing fails, fall back to plain-text legacy parsing.
        }
    }

    const stateMatch = /^drive:(forward|backward|left|right|all),(start|stop)$/i.exec(text);
    if (stateMatch) {
        const direction = stateMatch[1].toLowerCase();
        const state = stateMatch[2].toLowerCase();
        let robotWireCommand = 'S';

        if (state === 'start') {
            if (direction === 'forward') robotWireCommand = 'F';
            else if (direction === 'backward') robotWireCommand = 'B';
            else if (direction === 'left') robotWireCommand = 'L';
            else if (direction === 'right') robotWireCommand = 'R';
        }

        return {
            kind: 'control',
            normalized: `drive:${direction},${state}`,
            robotWireCommand,
            clientTs,
            sequence
        };
    }

    const legacyMap = {
        F: 'drive:forward,start',
        B: 'drive:backward,start',
        L: 'drive:left,start',
        R: 'drive:right,start',
        S: 'drive:all,stop'
    };

    const legacy = legacyMap[text];
    if (!legacy) {
        return null;
    }

    return {
        kind: 'control',
        normalized: legacy,
        robotWireCommand: text,
        clientTs,
        sequence
    };
}

function pushLatestFrameToClient(client, frameBuffer) {
    const state = videoClientState.get(client);
    if (!state || client.readyState !== WebSocket.OPEN) {
        return;
    }

    // Keep only one pending frame per client so stale frames are discarded instantly.
    state.latestFrame = frameBuffer;

    if (!state.sending) {
        flushLatestFrame(client);
    }
}

function flushLatestFrame(client) {
    const state = videoClientState.get(client);
    if (!state || state.sending || client.readyState !== WebSocket.OPEN) {
        return;
    }

    if (!state.latestFrame) {
        return;
    }

    const now = Date.now();

    // Rate-limit relay→browser to prevent kernel TCP buffer buildup over WAN.
    const msSinceLastSend = now - state.lastSentAt;
    if (msSinceLastSend < MIN_RELAY_SEND_MS) {
        setTimeout(() => flushLatestFrame(client), MIN_RELAY_SEND_MS - msSinceLastSend);
        return;
    }

    // Discard frames already too old — sending them would only deepen lag.
    const frameAge = now - state.latestFrame.receivedAt;
    if (frameAge > STALE_FRAME_AGE_MS) {
        state.latestFrame = null;
        metrics.videoFramesDroppedByAge += 1;
        return;
    }

    // Absolute Egress Backpressure Culling: If the browser's WAN network pipe is even slightly
    // congested, instantly throw the frame in the trash to prevent a backup queue over the tunnel.
    if (client.bufferedAmount > 0) {
        state.latestFrame = null;
        metrics.videoFramesDroppedByBackpressure += 1;
        return;
    }

    const frame = state.latestFrame;
    state.latestFrame = null;
    state.sending = true;
    state.lastSentAt = now;
    const packet = buildFramePacket(frame);

    client.send(packet, { binary: true, compress: false }, (err) => {
        state.sending = false;
        if (err) {
            console.error('Client video send error:', err);
            client.terminate();
            return;
        }

        metrics.videoFramesSentToClients += 1;

        // If a newer frame arrived while we were sending, deliver that one next.
        flushLatestFrame(client);
    });
}

// Serve the static frontend files
app.use(express.static(path.join(__dirname, '../Remote_Control_Dashboard')));

// --- WebRTC WHEP Reverse Proxy (go2rtc Bridge) ---
// --- Remote Debugging Log Endpoint ---
app.post('/api/log', (req, res) => {
    const { type, msg, ua } = req.body;
    let device = 'Mobile';
    if (ua.includes('Windows')) device = 'PC';
    else if (ua.includes('Macintosh')) device = 'Mac';
    console.log(`[Browser ${device}] [${type.toUpperCase()}] ${msg}`);
    res.sendStatus(200);
});

// Proxies WHEP HTTP POST signaling handshakes directly to the local go2rtc server.
// This allows both the Dashboard and WebRTC video to run over a single Cloudflare Quick Tunnel!
app.post('/api/whep', (req, res) => {
    const headers = { ...req.headers };
    // Override host header to match go2rtc local port to prevent routing/proxy mismatches
    headers['host'] = '127.0.0.1:1984';

    const proxyReq = http.request({
        host: '127.0.0.1',
        port: 1984,
        path: '/api/webrtc?src=robot_eye',
        method: 'POST',
        headers: headers
    }, (proxyRes) => {
        res.writeHead(proxyRes.statusCode, proxyRes.headers);
        proxyRes.pipe(res);
    });

    req.pipe(proxyReq);

    proxyReq.on('error', (err) => {
        console.error('[WebRTC Proxy] Failed to connect to go2rtc on Port 1984:', err.message);
        if (!res.headersSent) {
            res.status(502).send('go2rtc gateway offline');
        } else {
            res.end();
        }
    });
});

// --- WebRTC MJPEG Stream Fallback Proxy ---
// Proxies the local go2rtc raw MJPEG stream directly over Port 3000 to browser clients.
// This allows a seamless, firewall-penetrating fallback when UDP WebRTC is blocked.
app.get('/api/stream.mjpeg', (req, res) => {
    // Disable Nagle's algorithm and proxy buffering to prevent frame latency build-up
    req.socket.setNoDelay(true);
    res.setHeader('Cache-Control', 'no-cache, no-store, must-revalidate');
    res.setHeader('Pragma', 'no-cache');
    res.setHeader('Expires', '0');
    res.setHeader('Connection', 'keep-alive');
    res.setHeader('X-Accel-Buffering', 'no'); // Prevents proxy and gateway buffering

    const proxyReq = http.request({
        host: '127.0.0.1',
        port: 1984,
        path: '/api/stream.mjpeg?src=robot_eye_mjpeg',
        method: 'GET',
        headers: req.headers
    }, (proxyRes) => {
        res.writeHead(proxyRes.statusCode, proxyRes.headers);
        proxyRes.pipe(res);
    });

    proxyReq.on('error', (err) => {
        console.error('[MJPEG Proxy] Failed to connect to go2rtc on Port 1984:', err.message);
        if (!res.headersSent) {
            res.status(502).send('go2rtc gateway offline');
        } else {
            res.end();
        }
    });

    proxyReq.end();
});

// Status diagnostic endpoint
app.get('/status', (req, res) => {
    res.json({
        eyeConnected:    !!eyeSocket    && (eyeSocket.readyState    === 1 || eyeSocket.readyState    === WebSocket.OPEN),
        muscleConnected: !!muscleSocket && (muscleSocket.readyState === 1 || muscleSocket.readyState === WebSocket.OPEN),
        clientsConnected: controlClientSockets.size,
        videoSubscribers: videoClientSockets.size,
        uptimeMs: Date.now() - relayStartedAt,
        metrics,
        timestamp: new Date().toISOString()
    });
});

/**
 * installEyeRSVPatcher — clears RSV2/RSV3 bits from the Eye WebSocket byte stream.
 *
 * Some builds of the Markus Sattler WebSocketsClient library set RSV2 (0x20) or
 * RSV3 (0x10) bits in WebSocket binary frame headers. RFC 6455 mandates these bits
 * stay zero unless a negotiated extension uses them; ws 8.x throws
 * "RSV2 and RSV3 must be clear" (WS_ERR_UNEXPECTED_RSV_2_3) and terminates the
 * socket whenever it encounters them.
 *
 * This function replaces the ws Receiver's raw 'data' listener on the underlying
 * TCP socket with a frame-boundary-aware interceptor that clears those two bits
 * from each frame header byte 0 before the Receiver inspects the data.
 * Frame-boundary state is tracked across TCP segment boundaries. The patched
 * chunk is then forwarded to the original downstream listener unchanged.
 *
 * This is a server-only fix — no ESP32 reflash required.
 */
function installEyeRSVPatcher(ws) {
    const socket = ws._socket;
    if (!socket) return;

    // Atomically swap out ws's data listeners.
    const downstream = socket.listeners('data').slice();
    socket.removeAllListeners('data');

    let frameBytesLeft = 0;   // bytes remaining in the current frame payload

    socket.on('data', (rawChunk) => {
        const chunk = Buffer.isBuffer(rawChunk) ? rawChunk : Buffer.from(rawChunk);
        let pos = 0;

        while (pos < chunk.length) {
            if (frameBytesLeft > 0) {
                // Inside payload — advance past it.
                const skip = Math.min(frameBytesLeft, chunk.length - pos);
                frameBytesLeft -= skip;
                pos += skip;
            } else {
                // Frame boundary: byte at pos is header byte 0.
                // Clear RSV1 (0x40), RSV2 (0x20), and RSV3 (0x10) protocol extension bits.
                chunk[pos] &= 0x8F;

                // Need at least 2 bytes to read the payload length field.
                if (pos + 1 >= chunk.length) break;

                const byte1       = chunk[pos + 1];
                const masked      = (byte1 & 0x80) !== 0;
                const lenIndicator = byte1 & 0x7F;

                let headerSize;
                let payloadLen;

                if (lenIndicator < 126) {
                    payloadLen = lenIndicator;
                    headerSize = 2;
                } else if (lenIndicator === 126) {
                    if (pos + 4 > chunk.length) break;   // header straddles chunk boundary — rare
                    payloadLen = chunk.readUInt16BE(pos + 2);
                    headerSize = 4;
                } else {
                    if (pos + 10 > chunk.length) break;  // header straddles chunk boundary — rare
                    payloadLen = Number(chunk.readBigUInt64BE(pos + 2));
                    headerSize = 10;
                }

                if (masked) headerSize += 4;   // skip the 4-byte masking key
                frameBytesLeft = payloadLen;
                pos += headerSize;
            }
        }

        // Forward the (patched) chunk to ws's Receiver.
        for (const fn of downstream) fn.call(socket, chunk);
    });
}

// Handle WebSocket connection routing
server.on('upgrade', (request, socket, head) => {
    socket.setNoDelay(true); // Bypass Nagle's algorithm packet batching delays
    const pathname = new URL(request.url, `http://${request.headers.host}`).pathname;

    if (pathname === '/robot-video') {
        // Strip compression extension headers from ESP32 connections to prevent RSV framing errors.
        // The Markus Sattler WebSocketsClient library advertises permessage-deflate but does not
        // honour the RSV bit contract when the server rejects it — removing the header entirely
        // avoids the negotiation mismatch that causes "RSV2 and RSV3 must be clear" disconnects.
        delete request.headers['sec-websocket-extensions'];
        // The Eye — camera ESP32 pushes JPEG frames here
        wss.handleUpgrade(request, socket, head, (ws) => {
            handleEyeConnection(ws);
        });
    } else if (pathname === '/robot-control') {
        delete request.headers['sec-websocket-extensions'];
        // The Muscle — motor ESP32 receives drive commands here
        wss.handleUpgrade(request, socket, head, (ws) => {
            handleMuscleConnection(ws);
        });
    } else if (pathname === '/video-stream') {
        // Browser — receives live JPEG stream from The Eye
        wss.handleUpgrade(request, socket, head, (ws) => {
            handleVideoClientConnection(ws);
        });
    } else if (pathname === '/control-stream') {
        // Browser — sends drive commands forwarded to The Muscle
        wss.handleUpgrade(request, socket, head, (ws) => {
            handleControlClientConnection(ws);
        });
    } else {
        socket.destroy();
    }
});

// --- The Eye: camera-only ESP32 ---
// Accepts binary JPEG frames on /robot-video and fans them to all browser video lanes.
function handleEyeConnection(ws) {
    // Must be the very first call: intercepts the raw socket data stream before
    // the ws Receiver inspects any frame headers.
    installEyeRSVPatcher(ws);

    if (eyeSocket) {
        console.log('[Eye] New connection — closing previous.');
        eyeSocket.close();
    }
    eyeSocket = ws;
    console.log('[Eye] Connected from:', ws._socket.remoteAddress);

    ws.on('message', (message, isBinary) => {
        if (!isBinary) {
            console.warn('[Eye] Unexpected text frame ignored:', message.toString().slice(0, 80));
            return;
        }
        metrics.videoFramesReceivedFromRobot += 1;
        const frameBuffer = Buffer.isBuffer(message) ? message : Buffer.from(message);
        const sourceTsMs = Date.now();
        const stampedFrame = { data: frameBuffer, sourceTsMs, receivedAt: sourceTsMs };

        // Latest-frame-wins: each browser client retains at most one pending frame.
        videoClientSockets.forEach(client => {
            if (client.readyState === WebSocket.OPEN) {
                pushLatestFrameToClient(client, stampedFrame);
            }
        });
    });

    ws.on('close', () => {
        console.log('[Eye] Disconnected.');
        if (eyeSocket === ws) eyeSocket = null;
    });
    ws.on('error', (err) => { console.error('[Eye] Socket error:', err.message); });
}

// --- The Muscle: motor-only ESP32 ---
// Connects on /robot-control and receives single-character drive commands forwarded from the relay.
function handleMuscleConnection(ws) {
    if (muscleSocket) {
        console.log('[Muscle] New connection — closing previous.');
        muscleSocket.close();
    }
    muscleSocket = ws;
    console.log('[Muscle] Connected from:', ws._socket.remoteAddress);

    // The Muscle sends no meaningful upstream data — log anything unexpected.
    ws.on('message', (message, isBinary) => {
        if (!isBinary) console.log('[Muscle] Status update:', message.toString().slice(0, 80));
    });

    ws.on('close', () => {
        console.log('[Muscle] Disconnected.');
        if (muscleSocket === ws) muscleSocket = null;
    });
    ws.on('error', (err) => { console.error('[Muscle] Socket error:', err.message); });
}

function handleVideoClientConnection(ws) {
    const socket = ws._socket;
    if (socket) {
        socket.setNoDelay(true); // Explicitly kill Nagle's algorithm on dashboard video socket
    }
    videoClientSockets.add(ws);
    videoClientState.set(ws, { latestFrame: null, sending: false, lastSentAt: 0 });
    console.log(`Video client connected. Total video clients: ${videoClientSockets.size}`);

    ws.on('close', () => {
        videoClientSockets.delete(ws);
        videoClientState.delete(ws);
        console.log(`Video client disconnected. Total video clients: ${videoClientSockets.size}`);
    });

    ws.on('error', (err) => {
        videoClientState.delete(ws);
        console.error('Video client socket error:', err);
    });
}

// Control lane handler (Web browsers)
function handleControlClientConnection(ws) {
    const socket = ws._socket;
    if (socket) {
        socket.setNoDelay(true); // Explicitly kill Nagle's algorithm on dashboard control socket
    }
    controlClientSockets.add(ws);
    console.log(`Control client connected. Total control clients: ${controlClientSockets.size}`);

    ws.on('message', (message) => {
        metrics.controlMessagesReceived += 1;
        const parsedControl = parseControlMessage(message);
        if (!parsedControl) {
            metrics.controlMessagesInvalid += 1;
            ws.send(JSON.stringify({ error: 'Invalid control command format' }));
            return;
        }

        if (parsedControl.kind === 'ping') {
            metrics.controlPingMessages += 1;
            ws.send(JSON.stringify({
                type: 'pong',
                seq: parsedControl.sequence,
                echoedClientTs: parsedControl.clientTs,
                serverTs: Date.now()
            }));
            return;
        }
        
        // Forward command to The Muscle if connected
        if (muscleSocket && (muscleSocket.readyState === 1 || muscleSocket.readyState === WebSocket.OPEN)) {
            let transit = '';
            if (Number.isFinite(parsedControl.clientTs)) {
                transit = ` transit=${Date.now() - parsedControl.clientTs}ms`;
            }
            console.log(`[Muscle] Relaying '${parsedControl.normalized}' -> '${parsedControl.robotWireCommand}'${transit}`);
            muscleSocket.send(parsedControl.robotWireCommand);
            metrics.controlMessagesForwarded += 1;
        } else {
            metrics.controlMessagesDroppedMuscleOffline += 1;
            console.warn(`Command '${parsedControl.normalized}' dropped — Muscle is offline.`);
            ws.send(JSON.stringify({ error: 'Muscle is currently offline' }));
        }
    });

    ws.on('close', () => {
        controlClientSockets.delete(ws);
        console.log(`Control client disconnected. Total control clients: ${controlClientSockets.size}`);
        // Safety stop: browser gone — immediately halt The Muscle.
        if (muscleSocket && muscleSocket.readyState === WebSocket.OPEN) {
            muscleSocket.send('S');
        }
    });

    ws.on('error', (err) => {
        console.error('Control client socket error:', err);
    });
}

let mjpegConsumerActive = false;
let mjpegRequest = null;

function startMjpegConsumer() {
    if (mjpegConsumerActive) return;
    mjpegConsumerActive = true;

    console.log('[MJPEG Consumer] Starting local stream acquisition from go2rtc...');

    let buffer = Buffer.alloc(0);

    mjpegRequest = http.request({
        host: '127.0.0.1',
        port: 1984,
        path: '/api/stream.mjpeg?src=robot_eye_mjpeg',
        method: 'GET'
    }, (res) => {
        res.on('data', (chunk) => {
            buffer = Buffer.concat([buffer, chunk]);

            while (true) {
                const soiIdx = buffer.indexOf(Buffer.from([0xFF, 0xD8]));
                if (soiIdx === -1) {
                    if (buffer.length > 64 * 1024) {
                        buffer = Buffer.alloc(0);
                    }
                    break;
                }

                if (soiIdx > 0) {
                    buffer = buffer.slice(soiIdx);
                    continue;
                }

                const eoiIdx = buffer.indexOf(Buffer.from([0xFF, 0xD9]), 2);
                if (eoiIdx === -1) {
                    break;
                }

                const frameLen = eoiIdx + 2;
                const jpegFrame = buffer.slice(0, frameLen);
                buffer = buffer.slice(frameLen);

                // Fan out to all WebSocket video clients
                const stampedFrame = { data: jpegFrame, sourceTsMs: Date.now(), receivedAt: Date.now() };
                videoClientSockets.forEach(client => {
                    if (client.readyState === WebSocket.OPEN) {
                        pushLatestFrameToClient(client, stampedFrame);
                    }
                });
            }
        });

        res.on('end', () => {
            console.log('[MJPEG Consumer] Local stream ended. Reconnecting in 2 seconds...');
            mjpegConsumerActive = false;
            setTimeout(startMjpegConsumer, 2000);
        });
    });

    mjpegRequest.on('error', (err) => {
        console.warn('[MJPEG Consumer] Failed to connect to go2rtc API. Retrying in 4 seconds...');
        mjpegConsumerActive = false;
        setTimeout(startMjpegConsumer, 4000);
    });

    mjpegRequest.end();
}

const PORT = process.env.PORT || 3000;
server.listen(PORT, () => {
    console.log(`=======================================================`);
    console.log(`     ELEGOO DUAL-BRAIN WEBSOCKET RELAY RUNNING         `);
    console.log(`=======================================================`);
    console.log(`  Eye uplink   : ws://localhost:${PORT}/robot-video`);
    console.log(`  Muscle uplink: ws://localhost:${PORT}/robot-control`);
    console.log(`  Browser video: ws://localhost:${PORT}/video-stream`);
    console.log(`  Browser ctrl : ws://localhost:${PORT}/control-stream`);
    console.log(`  Status       : http://localhost:${PORT}/status`);
    console.log(`  Dashboard    : http://localhost:${PORT}`);
    console.log(`=======================================================`);

    // Start local acquisition stream
    startMjpegConsumer();
});
