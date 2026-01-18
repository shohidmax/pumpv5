// server.js
// =================================================================
// Simple & Robust WebSocket Relay Server for ESP32
// Features: MongoDB Logging, Command Forwarding, Date Filtering
// =================================================================

const express = require('express');
const http = require('http');
const WebSocket = require('ws');
const path = require('path');
const mongoose = require('mongoose');
require('dotenv').config();

const app = express();
app.use(express.static(path.join(__dirname, 'public')));

// --- MongoDB Setup ---
const mongoUri = process.env.MONGODB_URI;
if (mongoUri) {
    mongoose.connect(mongoUri)
        .then(() => console.log('Connected to MongoDB'))
        .catch(err => console.error('MongoDB error:', err));
} else {
    console.warn("WARNING: MONGODB_URI not set. Logs will not be saved!");
}

const motorLogSchema = new mongoose.Schema({
    macAddress: String,
    onTime: String,
    offTime: String,
    duration: String,
    serverTime: { type: Date, default: Date.now }
});
const MotorLog = mongoose.model('MotorLog', motorLogSchema);

const server = http.createServer(app);
const wss = new WebSocket.Server({ server });

let esp32Socket = null;

wss.on('connection', (ws) => {
    console.log('Client connected.');

    ws.on('message', async (message) => {
        let data;
        try {
            data = JSON.parse(message.toString());
        } catch (e) {
            console.error('JSON Error:', e);
            return;
        }

        if (data.type === 'esp32-identify') {
            console.log('ESP32 Connected.');
            esp32Socket = ws;
            ws.isEsp32 = true;
        }
        // --- LOG UPLOAD FROM ESP32 ---
        else if (data.type === 'uploadLog') {
            const { mac, onTime, offTime, duration } = data.payload;
            console.log(`Log received from ${mac}: ${duration}`);

            if (mongoose.connection.readyState === 1) {
                try {
                    await MotorLog.create({
                        macAddress: mac,
                        onTime, offTime, duration
                    });
                    console.log("Log saved to DB.");
                } catch (err) {
                    console.error("DB Save Fail:", err);
                }
            } else {
                console.error("Log Dropped: MongoDB NOT Connected (State: " + mongoose.connection.readyState + ")");
            }
        }
        // --- LOG QUERY FROM DASHBOARD ---
        else if (data.type === 'getLogs') {
            const { startDate, endDate } = data.payload || {};

            // STRICT FILTER ENFORCEMENT
            if (!startDate || !endDate) {
                console.warn("Log Fetch Denied: Missing Date Range");
                ws.send(JSON.stringify({ type: 'error', message: 'Date Range Required' }));
                return;
            }

            let query = {};
            if (startDate || endDate) {
                query.serverTime = {};
                if (startDate) {
                    // Force start of day UTC
                    query.serverTime.$gte = new Date(startDate);
                }
                if (endDate) {
                    // Force End of Day by going to Next Day 00:00
                    const end = new Date(endDate);
                    end.setDate(end.getDate() + 1);
                    query.serverTime.$lt = end;
                }
            }

            if (mongoose.connection.readyState === 1) {
                try {
                    const logs = await MotorLog.find(query).sort({ serverTime: -1 }).limit(100);
                    ws.send(JSON.stringify({ type: 'logHistory', payload: logs }));
                } catch (err) {
                    console.error("DB Query Fail:", err);
                    ws.send(JSON.stringify({ type: 'error', message: 'Database Error. Check Server Logs.' }));
                }
            } else {
                ws.send(JSON.stringify({ type: 'logHistory', payload: [] }));
            }
        }
        // --- CLEAR/DELETE LOGS ---
        else if (data.type === 'deleteLogs' || data.type === 'clearLogs') {
            const { startDate, endDate } = data.payload || {};
            let match = {};

            if (startDate && endDate) {
                match.serverTime = {
                    $gte: new Date(startDate),
                    $lt: new Date(new Date(endDate).setDate(new Date(endDate).getDate() + 1))
                };
            }

            if (mongoose.connection.readyState === 1) {
                try {
                    const result = await MotorLog.deleteMany(match);
                    console.log(`Deleted ${result.deletedCount} logs.`);

                    // Notify dashboard to clear table if it was a full clear, 
                    // or just refresh if it was a partial clear.
                    // Easiest is to send an empty list or trigger a refresh? 
                    // Actually, sending 'logHistory' with [] is aggressive if only partial.
                    // Better to send a success message and let client refresh.
                    ws.send(JSON.stringify({ type: 'error', message: `Deleted ${result.deletedCount} logs.` })); // Reuse toast

                    // If full clear, clear table
                    if (Object.keys(match).length === 0) {
                        ws.send(JSON.stringify({ type: 'logHistory', payload: [] }));
                    } else {
                        // If partial, maybe ask client to refresh? 
                        // For now client will manually refresh or we can trigger it.
                    }
                } catch (err) {
                    console.error("DB Clear Fail:", err);
                    ws.send(JSON.stringify({ type: 'error', message: 'Delete Failed.' }));
                }
            }
        }
        // --- EXISTING STATUS/COMMAND LOGIC ---
        else if (data.type === 'statusUpdate' && ws.isEsp32) {
            wss.clients.forEach((client) => {
                if (!client.isEsp32 && client.readyState === WebSocket.OPEN) {
                    client.send(JSON.stringify({ type: 'statusUpdate', payload: data.payload }));
                }
            });
        }
        else if (data.type === 'command') {
            console.log(`[CMD] Received: ${data.command} Value: ${data.value}`);
            if (esp32Socket) {
                if (esp32Socket.readyState === WebSocket.OPEN) {
                    esp32Socket.send(JSON.stringify(data));
                    console.log(`[CMD] Forwarded to ESP32 (State: OPEN)`);
                } else {
                    console.warn(`[CMD] Failed: ESP32 Socket exists but State is ${esp32Socket.readyState}`);
                    ws.send(JSON.stringify({ type: 'error', message: 'Device Disconnected (Socket Closed).' }));
                    esp32Socket = null;
                }
            } else {
                console.warn("[CMD] Failed: ESP32 Socket is NULL.");
                ws.send(JSON.stringify({ type: 'error', message: 'Device Offline (No Socket).' }));
            }
        }
        else if (data.type === 'ping') {
            ws.send(JSON.stringify({ type: 'pong' }));
        }
        else if (data.type === 'requestStatus') {
            if (esp32Socket && esp32Socket.readyState === WebSocket.OPEN) {
                // 1. Tell Dashboard "Server knows Device is Online"
                ws.send(JSON.stringify({ type: 'serverStatus', deviceOnline: true }));
                // 2. Ask ESP32 for fresh data
                esp32Socket.send(JSON.stringify({ command: "FORCE_STATUS_UPDATE", value: 1 }));
            } else {
                ws.send(JSON.stringify({ type: 'serverStatus', deviceOnline: false }));
            }
        }
    });

    ws.on('close', () => {
        if (ws.isEsp32) {
            esp32Socket = null;
            console.log('ESP32 Disconnected.');
        }
    });
});

const PORT = process.env.PORT || 3000;
server.listen(PORT, () => {
    console.log(`Server listening on ${PORT}`);
});
