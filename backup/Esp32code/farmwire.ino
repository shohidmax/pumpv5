// =================================================================
// ESP32 WebSocket IoT Controller - UPDATED PINOUT
// Features: Robust WiFi, Cycle Logging, Pagination, WiFiManager Dynamic Config
// Hardware: SIM800L, DFPlayer, OLED, relays, switches
// =================================================================

#include "time.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Wire.h>
#include <esp_task_wdt.h>

// --- OBJECTS ---
WebSocketsClient webSocket;
Preferences preferences;
WebServer server(80);

// OLED Configuration
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- Configuration ---
// These will be loaded from Preferences
String websocket_server_host = "pumpv5.espserver.site";
int websocket_server_port = 443;

#define WDT_TIMEOUT 30

// NTP Time Configuration
const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 6 * 3600;
const int daylightOffset_sec = 0;

// --- PIN DEFINITIONS ---
#define RELAY_1 25         // Motor ON (Pulse)
#define RELAY_2 26         // Motor OFF (Pulse)
#define RELAY_3 32         // Direct Toggle
#define BUZZER_PIN 13      // Buzzer
#define SWITCH_FEEDBACK 23 // Switch 1: Motor Feedback
#define SWITCH_SECURITY 18 // Switch 2: Connection Security
#define SWITCH_DOOR 19     // Switch 3: Door Feedback
#define BTN_RESET 33       // Hard Reset (10s hold)

// SIM800L (Serial2)
#define SIM800_TX 17
#define SIM800_RX 16

// DFPlayer (Serial1)
#define DFPLAYER_TX 14
#define DFPLAYER_RX 27

// OLED (I2C) - User Requested Swap
#define USER_OLED_SDA 22
#define USER_OLED_SCL 21

// --- GLOBAL VARIABLES ---
unsigned long relay1_timer = 0, relay2_timer = 0;
const int relay_pulse_duration = 500; // 500ms pulsing

unsigned long lastStatusUpdateTime = 0, lastKeepAliveTime = 0;
unsigned long resetBtnStartTime = 0;
unsigned long lastRxTime = 0; // Added for Watchdog
bool resetBtnPressed = false;

String Motor_Stat = "OFF", System_Mode = "Normal", Door_Stat = "Closed",
       lastAction = "System Boot";
int wifiSignal = 0;

String lastMotorStatus = "OFF";
String lastDoorStatus = "Closed";
bool timeSynced = false;
unsigned long lastWifiCheckTime = 0;
bool portalRunning = false;
bool shouldSaveConfig = false; // For WiFiManager callback

// --- FORWARD DECLARATIONS ---
void sendStatusUpdate();
void sendLogPage(int page);
void addMotorLog(time_t onTimestamp, time_t offTimestamp);
String formatDuration(unsigned long seconds);
void connectWiFi();
void handleHardReset();
void saveConfigCallback();
void playTone(String type); // Added forward declaration for playTone

// --- WebSocket Event ---
void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  esp_task_wdt_reset();
  switch (type) {
  case WStype_DISCONNECTED:
    Serial.println("[WSc] Disconnected!");
    break;
  case WStype_CONNECTED:
    Serial.println("[WSc] Connected!");
    webSocket.sendTXT("{\"type\":\"esp32-identify\"}");
    playTone("success");
    lastRxTime = millis(); // Reset watchdog on connect
    break;
  case WStype_TEXT: {
    lastRxTime = millis(); // Valid traffic received
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
      Serial.println(F("JSON Failed"));
      return;
    }

    String command = doc["command"];
    Serial.print("CMD Received: ");
    Serial.println(command);

    if (command == "RELAY_1") {
      digitalWrite(RELAY_1, HIGH);
      relay1_timer = millis();
      lastAction = "Motor ON (Web)";
      playTone("success"); // Changed
      Serial.println("Action: RELAY_1 ON");
      sendStatusUpdate();
    } else if (command == "RELAY_2") {
      digitalWrite(RELAY_2, HIGH);
      relay2_timer = millis();
      lastAction = "Motor OFF (Web)";
      playTone("success"); // Changed
      Serial.println("Action: RELAY_2 ON");
      sendStatusUpdate();
    } else if (command == "RELAY_3") {
      bool currentState = digitalRead(RELAY_3);
      digitalWrite(RELAY_3, !currentState);
      lastAction = "Relay 3 Toggle";
      playTone("click"); // Changed
      sendStatusUpdate();
    } else if (command == "RESTART_ESP") {
      lastAction = "Restarting...";
      sendStatusUpdate();
      delay(1000);
      ESP.restart();
    } else if (command == "CLEAR_LOGS") {
      preferences.begin("motor_logs", false);
      preferences.clear();
      preferences.end();
      lastAction = "Logs Cleared";
      playTone("click"); // Changed
      sendStatusUpdate();
    } else if (command == "GET_LOG_PAGE") {
      sendLogPage(doc["value"]);
      return;
    } else if (command == "FORCE_STATUS_UPDATE") {
      sendStatusUpdate();
      return;
    }
    break;
  }
  default:
    break;
  }
}

// --- SETUP ---
void setup() {
  Serial.begin(115200);
  Serial.println("\n--- Booting ---");

  // Peripherals Init
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(RELAY_1, OUTPUT);
  pinMode(RELAY_2, OUTPUT);
  pinMode(RELAY_3, OUTPUT);
  digitalWrite(RELAY_1, LOW);
  digitalWrite(RELAY_2, LOW);
  digitalWrite(RELAY_3, LOW);

  pinMode(SWITCH_FEEDBACK, INPUT_PULLUP);
  pinMode(SWITCH_SECURITY, INPUT_PULLUP);
  pinMode(SWITCH_DOOR, INPUT_PULLUP);
  pinMode(BTN_RESET, INPUT_PULLUP);

  Serial2.begin(9600, SERIAL_8N1, SIM800_RX, SIM800_TX);
  Serial1.begin(9600, SERIAL_8N1, DFPLAYER_RX, DFPLAYER_TX);

  Wire.begin(USER_OLED_SDA, USER_OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("FarmWire Booting...");
    display.display();
  }

  playTone("startup"); // Changed

  // Load Saved Configuration
  preferences.begin("esp-config", true);
  if (preferences.isKey("ws_host")) {
    websocket_server_host = preferences.getString("ws_host");
  }
  if (preferences.isKey("ws_port")) {
    websocket_server_port = preferences.getInt("ws_port");
  }
  preferences.end();

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    lastAction = "Device Online";
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    unsigned long startSync = millis();
    while (millis() - startSync < 5000) {
      struct tm timeinfo;
      if (getLocalTime(&timeinfo) && timeinfo.tm_year > (2023 - 1900)) {
        timeSynced = true;

        // Recover logic
        preferences.begin("motor_state", true);
        String savedStat = preferences.getString("last_stat", "OFF");
        time_t savedOnTime = preferences.getULong64("on_time", 0);
        preferences.end();

        if (savedStat == "ON" && savedOnTime > 0) {
          time_t bootTime;
          time(&bootTime);
          if ((bootTime - savedOnTime) < 86400) {
            addMotorLog(savedOnTime, bootTime);
          }
          preferences.begin("motor_state", false);
          preferences.putString("last_stat", "OFF");
          preferences.putULong64("on_time", 0);
          preferences.end();
        }
        break;
      }
      delay(100);
    }

    // Connect WS using dynamic config
    // Sanitize Host (Automated Fix)
    websocket_server_host.replace("wss://", "");
    websocket_server_host.replace("https://", "");
    websocket_server_host.replace("/", "");

    Serial.print("Connecting to WS: ");
    Serial.print(websocket_server_host);
    Serial.print(" : ");
    Serial.println(websocket_server_port);

    webSocket.beginSSL(websocket_server_host.c_str(), websocket_server_port,
                       "/");
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
  }

  lastMotorStatus = (digitalRead(SWITCH_FEEDBACK) == LOW) ? "ON" : "OFF";
  lastDoorStatus = (digitalRead(SWITCH_DOOR) == LOW) ? "Open" : "Closed";

  esp_task_wdt_deinit();
  esp_task_wdt_config_t wdt_config = {.timeout_ms = WDT_TIMEOUT * 1000,
                                      .idle_core_mask =
                                          (1 << portNUM_PROCESSORS) - 1,
                                      .trigger_panic = true};
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);
}

// --- MAIN LOOP ---
void loop() {
  esp_task_wdt_reset();

  if (WiFi.status() != WL_CONNECTED) {
    if (!portalRunning && (millis() - lastWifiCheckTime > 30000)) {
      connectWiFi(); // Re-trigger portal if needed
      lastWifiCheckTime = millis();
    }
  } else {
    portalRunning = false;
    webSocket.loop();

    // Retry Time Sync if needed
    if (!timeSynced) {
      struct tm timeinfo;
      if (getLocalTime(&timeinfo) && timeinfo.tm_year > (2023 - 1900)) {
        timeSynced = true;
        Serial.println("Time Synced Late!");
      }
    }
  }

  if (relay1_timer > 0 && millis() - relay1_timer >= relay_pulse_duration) {
    digitalWrite(RELAY_1, LOW);
    relay1_timer = 0;
  }
  if (relay2_timer > 0 && millis() - relay2_timer >= relay_pulse_duration) {
    digitalWrite(RELAY_2, LOW);
    relay2_timer = 0;
  }

  // Hard Reset Button
  if (digitalRead(BTN_RESET) == LOW) {
    if (!resetBtnPressed) {
      resetBtnPressed = true;
      resetBtnStartTime = millis();
    } else {
      if (millis() - resetBtnStartTime > 10000) {
        playTone("click"); // Changed
        handleHardReset();
      }
    }
  } else {
    resetBtnPressed = false;
  }

  // Monitor Inputs
  String currentMotorStatus =
      (digitalRead(SWITCH_FEEDBACK) == LOW) ? "ON" : "OFF";
  if (currentMotorStatus != lastMotorStatus) {
    if (timeSynced) {
      time_t now;
      time(&now);
      if (currentMotorStatus == "ON") {
        preferences.begin("motor_state", false);
        preferences.putString("last_stat", "ON");
        preferences.putULong64("on_time", now);
        preferences.end();
      } else {
        preferences.begin("motor_state", false);
        time_t onTime = preferences.getULong64("on_time", 0);
        preferences.putString("last_stat", "OFF");
        preferences.end();
        if (onTime > 0) {
          addMotorLog(onTime, now);
        }
      }
    }
    lastMotorStatus = currentMotorStatus;
    lastAction =
        (currentMotorStatus == "ON") ? "Motor Started" : "Motor Stopped";
    sendStatusUpdate();
  }

  String currentDoorStatus =
      (digitalRead(SWITCH_DOOR) == LOW) ? "Open" : "Closed";
  if (currentDoorStatus != lastDoorStatus) {
    lastDoorStatus = currentDoorStatus;
    sendStatusUpdate();
  }

  if (millis() - lastStatusUpdateTime > 1000) { // Changed threshold to 1000
    sendStatusUpdate();
    lastStatusUpdateTime = millis();

    // Update OLED Professional
    drawDashboard(); // Replaced OLED drawing block with this call
  }

  // --- HEARTBEAT & WATCHDOG ---
  if (millis() - lastKeepAliveTime > 10000) {
    if (webSocket.isConnected()) {
      webSocket.sendTXT("{\"type\":\"ping\"}");
      // Check for Ghost Connection (No data for 30s)
      if (millis() - lastRxTime > 30000) {
        Serial.println("Watchdog: Connection Dead! Resetting...");
        webSocket.disconnect(); // Force Reconnect
        lastRxTime = millis();  // Reset timer
      }
    }
    lastKeepAliveTime = millis();
  }
}

void saveConfigCallback() {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void connectWiFi() {
  WiFiManager wm;
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setConfigPortalTimeout(180);

  // Add Custom Parameters
  WiFiManagerParameter custom_ws_host("server", "WS Host",
                                      websocket_server_host.c_str(), 60);
  WiFiManagerParameter custom_ws_port("port", "WS Port",
                                      String(websocket_server_port).c_str(), 6);

  wm.addParameter(&custom_ws_host);
  wm.addParameter(&custom_ws_port);

  if (!wm.autoConnect("ESP32-Setup")) {
    Serial.println("Failed to connect and hit timeout");
  } else {
    Serial.println("Connected...yeey :)");

    // Save Config if updated
    if (shouldSaveConfig) {
      websocket_server_host = custom_ws_host.getValue();
      websocket_server_port = String(custom_ws_port.getValue()).toInt();

      preferences.begin("esp-config", false);
      preferences.putString("ws_host", websocket_server_host);
      preferences.putInt("ws_port", websocket_server_port);
      preferences.end();
      Serial.println("Config Saved");
    }
  }
}

// --- BUZZER PATTERNS ---
void playTone(String type) {
  if (type == "success") {
    // Fast Double Chirp
    digitalWrite(BUZZER_PIN, HIGH);
    delay(80);
    digitalWrite(BUZZER_PIN, LOW);
    delay(50);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(80);
    digitalWrite(BUZZER_PIN, LOW);
  } else if (type == "error") {
    // Long Warning
    digitalWrite(BUZZER_PIN, HIGH);
    delay(500);
    digitalWrite(BUZZER_PIN, LOW);
  } else if (type == "click") {
    // Short Tick
    digitalWrite(BUZZER_PIN, HIGH);
    delay(30);
    digitalWrite(BUZZER_PIN, LOW);
  } else if (type == "startup") {
    // Escaping scale
    digitalWrite(BUZZER_PIN, HIGH);
    delay(50);
    digitalWrite(BUZZER_PIN, LOW);
    delay(50);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(50);
    digitalWrite(BUZZER_PIN, LOW);
    delay(50);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(100);
    digitalWrite(BUZZER_PIN, LOW);
  }
}

// --- VISUAL DASHBOARD ---
void drawDashboard() {
  display.clearDisplay();

  // --- HEADER ---
  // Signal Bars
  int rssi = WiFi.RSSI();
  int bars = 0;
  if (rssi > -55)
    bars = 4;
  else if (rssi > -65)
    bars = 3;
  else if (rssi > -75)
    bars = 2;
  else if (rssi > -85)
    bars = 1;

  for (int i = 0; i < 4; i++) {
    int h = (i + 1) * 2;
    if (i < bars)
      display.fillRect(2 + (i * 3), 10 - h, 2, h, SSD1306_WHITE);
    else
      display.drawRect(2 + (i * 3), 10 - h, 2, h, SSD1306_WHITE);
  }

  // Time (Right Aligned)
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    display.setTextSize(1);
    display.setCursor(64, 2);
    display.printf("%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min,
                   timeinfo.tm_sec);
  } else {
    display.setCursor(70, 2);
    display.print("Sync..");
  }

  display.drawLine(0, 12, 128, 12, SSD1306_WHITE);

  // --- MAIN STATUS (MOTOR) ---
  display.setTextSize(1);
  display.setCursor(40, 16);
  display.print("MOTOR IS");

  if (Motor_Stat == "ON") {
    display.fillRoundRect(22, 26, 84, 18, 4, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setTextSize(2);
    display.setCursor(50, 28);
    display.print("ON");
  } else {
    display.drawRoundRect(22, 26, 84, 18, 4, SSD1306_WHITE);
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(45, 28);
    display.print("OFF");
  }
  display.setTextColor(SSD1306_WHITE); // Reset

  // --- SECONDARY STATUS (Door & Mode) ---
  display.setTextSize(1);

  // Door (Left)
  display.setCursor(2, 46);
  display.print("D:");
  display.print(Door_Stat == "Open" ? "OPEN" : "CLS");

  // Mode (Right)
  display.setCursor(64, 46);
  display.print("M:");
  display.print(System_Mode == "Secured" ? "SEC" : "NRM");

  // --- FOOTER ---
  display.drawLine(0, 56, 128, 56, SSD1306_WHITE);
  display.setCursor(0, 57);
  if (WiFi.status() == WL_CONNECTED) {
    if (webSocket.isConnected()) {
      display.print("Online: ");
      display.print(WiFi.localIP());
    } else {
      display.print("Connecting to Srv...");
    }
  } else {
    display.print("WiFi Disconnected");
  }

  display.display();
}

void sendStatusUpdate() {
  Motor_Stat = lastMotorStatus;
  System_Mode = (digitalRead(SWITCH_SECURITY) == LOW) ? "Secured" : "Normal";
  Door_Stat = lastDoorStatus;
  wifiSignal = constrain(map(WiFi.RSSI(), -100, -30, 0, 100), 0, 100);

  DynamicJsonDocument doc(2048);
  doc["type"] = "statusUpdate";
  JsonObject payload = doc.createNestedObject("payload");
  payload["motorStatus"] = Motor_Stat;
  payload["systemMode"] = System_Mode;
  payload["doorStatus"] = Door_Stat;
  payload["lastAction"] = lastAction;
  payload["wifiSignal"] = wifiSignal;
  payload["localIP"] = WiFi.localIP().toString();
  payload["wsHost"] = websocket_server_host;

  String jsonString;
  serializeJson(doc, jsonString);
  if (webSocket.isConnected())
    webSocket.sendTXT(jsonString);
}

void handleHardReset() {
  Serial.println("Hard Reset...");
  preferences.begin("esp-config", false);
  preferences.clear();
  preferences.end();
  preferences.begin("motor_state", false);
  preferences.clear();
  preferences.end();
  preferences.begin("motor_logs", false);
  preferences.clear();
  preferences.end();

  WiFiManager wm;
  wm.resetSettings();
  ESP.restart();
}

void sendLogPage(int page) {
  // Not used for Cloud Log system, but kept for compatibility
}

void addMotorLog(time_t onTimestamp, time_t offTimestamp) {
  Serial.print("Attempting to Log... Synced: ");
  Serial.println(timeSynced);
  if (!timeSynced || onTimestamp == 0 || onTimestamp > offTimestamp) {
    Serial.println("Log Failed: Time issue or invalid timestamp");
    return;
  }

  unsigned long duration = offTimestamp - onTimestamp;
  if (duration > 86400)
    return;

  char onTimeStr[20], offTimeStr[20];
  strftime(onTimeStr, sizeof(onTimeStr), "%d/%m %I:%M%p",
           localtime(&onTimestamp));
  strftime(offTimeStr, sizeof(offTimeStr), "%d/%m %I:%M%p",
           localtime(&offTimestamp));

  Serial.print("Log: ");
  Serial.print(duration);
  Serial.println("s");

  // Upload to Cloud
  if (webSocket.isConnected()) {
    DynamicJsonDocument doc(1024);
    doc["type"] = "uploadLog";
    JsonObject payload = doc.createNestedObject("payload");
    payload["mac"] = WiFi.macAddress();
    payload["onTime"] = String(onTimeStr);
    payload["offTime"] = String(offTimeStr);
    payload["duration"] = formatDuration(duration);

    String jsonString;
    serializeJson(doc, jsonString);
    webSocket.sendTXT(jsonString);
    Serial.println("Log Sent to Server");
  } else {
    Serial.println("Log Failed: WS Not Connected");
  }
}

String formatDuration(unsigned long totalSeconds) {
  if (totalSeconds < 60)
    return String(totalSeconds) + "s";
  unsigned long h = totalSeconds / 3600;
  totalSeconds %= 3600;
  unsigned long m = totalSeconds / 60;
  unsigned long s = totalSeconds % 60;
  String d = "";
  if (h > 0)
    d += String(h) + "h ";
  if (m > 0)
    d += String(m) + "m ";
  d += String(s) + "s";
  return d;
}