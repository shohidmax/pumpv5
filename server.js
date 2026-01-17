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
                }
            } else {
                ws.send(JSON.stringify({ type: 'logHistory', payload: [] }));
            }
        }
        // --- CLEAR LOGS ---
        else if (data.type === 'clearLogs') {
            if (mongoose.connection.readyState === 1) {
                try {
                    await MotorLog.deleteMany({});
                    console.log("All logs cleared from DB.");
                    // Notify dashboard to clear table
                    ws.send(JSON.stringify({ type: 'logHistory', payload: [] }));
                } catch (err) {
                    console.error("DB Clear Fail:", err);
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
