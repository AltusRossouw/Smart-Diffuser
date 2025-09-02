#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <time.h>

// ================== Configuration model ==================
struct AppConfig {
  int triggerPin = 5; // GPIO5 (D1) default
  bool triggerActiveHigh = true;
  uint32_t triggerDurationMs = 1000;

  String mqttHost = "";
  uint16_t mqttPort = 1883;
  String mqttUser = "";
  String mqttPass = "";
  String mqttTopic = "diffuser/trigger";

  uint32_t intervalSeconds = 0;
  bool intervalEnabled = false;

  int timezoneOffsetMinutes = 0; // offset from UTC in minutes

  // e.g., ["08:00","12:30","18:45"]
  std::vector<String> scheduleTimes;
};

AppConfig config;

// ================== Globals ==================
ESP8266WebServer server(80);
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

unsigned long lastTriggerAt = 0;
bool triggerInProgress = false;

unsigned long nextIntervalAt = 0;

time_t lastTimeCheck = 0;
int lastDayOfYear = -1;
// One flag per schedule time to mark whether it fired today
std::vector<bool> scheduleFiredToday;

// ================== Utility ==================
static int parseTimeToMinutes(const String &hhmm) {
  // Expect "HH:MM"
  if (hhmm.length() < 4) return -1;
  int colon = hhmm.indexOf(':');
  if (colon < 0) return -1;
  int hh = hhmm.substring(0, colon).toInt();
  int mm = hhmm.substring(colon + 1).toInt();
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return -1;
  return hh * 60 + mm;
}

static String minutesToHHMM(int minutes) {
  int hh = minutes / 60;
  int mm = minutes % 60;
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", hh, mm);
  return String(buf);
}

static String htmlEscape(const String &in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c; break;
    }
  }
  return out;
}

void saveConfig() {
  if (!LittleFS.begin()) {
    LittleFS.format();
    LittleFS.begin();
  }
  DynamicJsonDocument doc(8192);
  doc["triggerPin"] = config.triggerPin;
  doc["triggerActiveHigh"] = config.triggerActiveHigh;
  doc["triggerDurationMs"] = config.triggerDurationMs;

  JsonObject mqtt = doc.createNestedObject("mqtt");
  mqtt["host"] = config.mqttHost;
  mqtt["port"] = config.mqttPort;
  mqtt["user"] = config.mqttUser;
  mqtt["pass"] = config.mqttPass;
  mqtt["topic"] = config.mqttTopic;

  doc["intervalSeconds"] = config.intervalSeconds;
  doc["intervalEnabled"] = config.intervalEnabled;

  doc["timezoneOffsetMinutes"] = config.timezoneOffsetMinutes;

  JsonArray sched = doc.createNestedArray("scheduleTimes");
  for (auto &t : config.scheduleTimes) {
    sched.add(t);
  }

  File f = LittleFS.open("/config.json", "w");
  if (!f) {
    Serial.println("Failed to open config file for writing");
    return;
  }
  serializeJsonPretty(doc, f);
  f.close();
  Serial.println("Config saved");
}

void loadConfig() {
  if (!LittleFS.begin()) {
    LittleFS.format();
    LittleFS.begin();
  }
  if (!LittleFS.exists("/config.json")) {
    Serial.println("No config.json, using defaults");
    return;
  }
  File f = LittleFS.open("/config.json", "r");
  if (!f) {
    Serial.println("Failed to open config.json");
    return;
  }
  DynamicJsonDocument doc(8192);
  auto err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf("Failed to parse config.json: %s\n", err.c_str());
    return;
  }

  config.triggerPin = doc["triggerPin"] | config.triggerPin;
  config.triggerActiveHigh = doc["triggerActiveHigh"] | config.triggerActiveHigh;
  config.triggerDurationMs = doc["triggerDurationMs"] | config.triggerDurationMs;

  if (doc.containsKey("mqtt")) {
    JsonObject mqtt = doc["mqtt"];
    config.mqttHost = String((const char*) (mqtt["host"] | config.mqttHost.c_str()));
    config.mqttPort = mqtt["port"] | config.mqttPort;
    config.mqttUser = String((const char*) (mqtt["user"] | config.mqttUser.c_str()));
    config.mqttPass = String((const char*) (mqtt["pass"] | config.mqttPass.c_str()));
    config.mqttTopic = String((const char*) (mqtt["topic"] | config.mqttTopic.c_str()));
  }

  config.intervalSeconds = doc["intervalSeconds"] | config.intervalSeconds;
  config.intervalEnabled = doc["intervalEnabled"] | config.intervalEnabled;

  config.timezoneOffsetMinutes = doc["timezoneOffsetMinutes"] | config.timezoneOffsetMinutes;

  config.scheduleTimes.clear();
  if (doc.containsKey("scheduleTimes") && doc["scheduleTimes"].is<JsonArray>()) {
    for (JsonVariant v : doc["scheduleTimes"].as<JsonArray>()) {
      String t = v.as<String>();
      if (parseTimeToMinutes(t) >= 0) {
        config.scheduleTimes.push_back(t);
      }
    }
  }

  Serial.println("Config loaded");
}

void applyTriggerPin() {
  pinMode(config.triggerPin, OUTPUT);
  // Set the pin to inactive level initially
  if (config.triggerActiveHigh) {
    digitalWrite(config.triggerPin, LOW);
  } else {
    digitalWrite(config.triggerPin, HIGH);
  }
}

// Non-blocking pulse trigger
void triggerPulse() {
  if (triggerInProgress) return;
  triggerInProgress = true;
  lastTriggerAt = millis();
  // drive active
  digitalWrite(config.triggerPin, config.triggerActiveHigh ? HIGH : LOW);
  Serial.println("Trigger: ON");
}

// Calculate local time with timezone offset
void getLocalTimeBrokenDown(struct tm &tm_local, time_t &epoch_local) {
  time_t now_utc = time(nullptr);
  epoch_local = now_utc + (time_t)(config.timezoneOffsetMinutes * 60);
  gmtime_r(&epoch_local, &tm_local); // gmtime on adjusted epoch gives local broken-down time
}

void resetScheduleFlagsForNewDay(const tm &tm_local) {
  // Reset flags daily
  scheduleFiredToday.assign(config.scheduleTimes.size(), false);
  lastDayOfYear = tm_local.tm_yday;
}

// MQTT
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();
  Serial.printf("MQTT msg on %s: '%s'\n", topic, msg.c_str());

  // Simple protocol
  // "TRIGGER" or "1" -> trigger
  // "INTERVAL:x" -> set interval seconds and enable
  // "STOP_INTERVAL" -> disable interval
  // "ADD_SCHEDULE:HH:MM"
  // "CLEAR_SCHEDULE"
  if (msg.equalsIgnoreCase("TRIGGER") || msg == "1") {
    triggerPulse();
  } else if (msg.startsWith("INTERVAL:")) {
    uint32_t s = msg.substring(String("INTERVAL:").length()).toInt();
    if (s > 0) {
      config.intervalSeconds = s;
      config.intervalEnabled = true;
      saveConfig();
      nextIntervalAt = millis() + (config.intervalSeconds * 1000UL);
    }
  } else if (msg.equalsIgnoreCase("STOP_INTERVAL")) {
    config.intervalEnabled = false;
    saveConfig();
  } else if (msg.startsWith("ADD_SCHEDULE:")) {
    String t = msg.substring(String("ADD_SCHEDULE:").length());
    t.trim();
    if (parseTimeToMinutes(t) >= 0) {
      config.scheduleTimes.push_back(t);
      saveConfig();
      scheduleFiredToday.assign(config.scheduleTimes.size(), false);
    }
  } else if (msg.equalsIgnoreCase("CLEAR_SCHEDULE")) {
    config.scheduleTimes.clear();
    saveConfig();
    scheduleFiredToday.assign(0, false);
  }
}

void connectMqtt() {
  if (config.mqttHost.length() == 0) return;
  mqttClient.setServer(config.mqttHost.c_str(), config.mqttPort);
  mqttClient.setCallback(mqttCallback);

  String clientId = "diffuser-" + String(ESP.getChipId(), HEX);
  bool ok;
  if (config.mqttUser.length() > 0) {
    ok = mqttClient.connect(clientId.c_str(), config.mqttUser.c_str(), config.mqttPass.c_str());
  } else {
    ok = mqttClient.connect(clientId.c_str());
  }
  if (ok) {
    Serial.println("MQTT connected");
    mqttClient.subscribe(config.mqttTopic.c_str());
    // Publish online status
    String statusTopic = config.mqttTopic + "/status";
    mqttClient.publish(statusTopic.c_str(), "online", true);
  } else {
    Serial.printf("MQTT connect failed, rc=%d\n", mqttClient.state());
  }
}

// ================== Web UI ==================
String renderHeader(const String &title) {
  String out;
  out += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  out += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  out += "<title>" + htmlEscape(title) + "</title>";
  out += "<style>body{font-family:sans-serif;max-width:720px;margin:20px auto;padding:0 12px}input,select,button{font-size:1rem;padding:6px}form{margin:10px 0}section{border:1px solid #ddd;border-radius:8px;padding:12px;margin:12px 0}h1,h2{margin:8px 0}code{background:#f5f5f5;padding:2px 4px;border-radius:4px}</style>";
  out += "</head><body>";
  out += "<header><h1>Diffuser Controller</h1><nav><a href='/'>Dashboard</a> | <a href='/config'>Config</a> | <a href='/api/wifi-portal'>WiFi Setup</a></nav><hr/></header>";
  return out;
}
String renderFooter() {
  String out;
  out += "<footer><hr/><small>ESP8266 " + String(ESP.getChipId(), HEX) + "</small></footer>";
  out += "</body></html>";
  return out;
}

String renderIndexPage() {
  // Status
  String ip = WiFi.isConnected() ? WiFi.localIP().toString() : String("Not connected");
  String out = renderHeader("Dashboard");
  out += "<section><h2>Status</h2>";
  out += "<div>WiFi: " + htmlEscape(ip) + "</div>";
  out += "<div>MQTT: " + String(mqttClient.connected() ? "Connected" : "Disconnected") + "</div>";
  out += "<div>Trigger Pin: GPIO" + String(config.triggerPin) + " (" + String(config.triggerActiveHigh ? "Active HIGH" : "Active LOW") + ")</div>";
  out += "<div>Trigger Duration: " + String(config.triggerDurationMs) + " ms</div>";
  out += "</section>";

  // Manual trigger
  out += "<section><h2>Manual Trigger</h2>";
  out += "<form method='POST' action='/api/trigger'><button type='submit'>Trigger Now</button></form>";
  out += "</section>";

  // Interval
  out += "<section><h2>Interval Trigger</h2>";
  out += "<form method='POST' action='/api/interval'>";
  out += "<label>Every (seconds): <input type='number' name='seconds' min='1' value='" + String(config.intervalSeconds) + "'></label><br/>";
  out += "<label><input type='checkbox' name='enabled' " + String(config.intervalEnabled ? "checked" : "") + "> Enabled</label><br/>";
  out += "<button type='submit'>Save</button>";
  out += "</form>";
  out += "</section>";

  // Schedule
  out += "<section><h2>Daily Schedule</h2>";
  out += "<div>Times:</div><ul>";
  for (size_t i = 0; i < config.scheduleTimes.size(); i++) {
    out += "<li>" + htmlEscape(config.scheduleTimes[i]) + " ";
    out += "<form style='display:inline' method='POST' action='/api/schedule/remove'><input type='hidden' name='idx' value='" + String(i) + "'><button type='submit'>Remove</button></form>";
    out += "</li>";
  }
  out += "</ul>";
  out += "<form method='POST' action='/api/schedule/add'>";
  out += "<label>Add HH:MM: <input type='time' name='time' required></label> ";
  out += "<button type='submit'>Add</button>";
  out += "</form>";
  out += "</section>";

  out += renderFooter();
  return out;
}

String renderConfigPage() {
  String out = renderHeader("Config");

  out += "<section><h2>Trigger</h2>";
  out += "<form method='POST' action='/api/config'>";
  // Pin select: common NodeMCU pins
  struct PinOption { const char* label; int gpio; } options[] = {
    {"D0 (GPIO16)", 16}, {"D1 (GPIO5)", 5}, {"D2 (GPIO4)", 4}, {"D3 (GPIO0)", 0},
    {"D4 (GPIO2)", 2}, {"D5 (GPIO14)", 14}, {"D6 (GPIO12)", 12}, {"D7 (GPIO13)", 13},
    {"D8 (GPIO15)", 15}
  };
  out += "<label>Trigger Pin: <select name='triggerPin'>";
  for (auto &opt: options) {
    out += "<option value='" + String(opt.gpio) + "'" + String(config.triggerPin == opt.gpio ? " selected" : "") + ">";
    out += htmlEscape(opt.label);
    out += "</option>";
  }
  out += "</select></label><br/>";
  out += "<label>Active Level: <select name='activeLevel'>";
  out += "<option value='HIGH'" + String(config.triggerActiveHigh ? " selected" : "") + ">HIGH</option>";
  out += "<option value='LOW'" + String(!config.triggerActiveHigh ? " selected" : "") + ">LOW</option>";
  out += "</select></label><br/>";
  out += "<label>Pulse Duration (ms): <input type='number' min='1' max='600000' name='pulseMs' value='" + String(config.triggerDurationMs) + "'></label>";
  out += "<br/><button type='submit'>Save</button>";
  out += "</form>";
  out += "</section>";

  out += "<section><h2>MQTT</h2>";
  out += "<form method='POST' action='/api/config'>";
  out += "<label>Host: <input type='text' name='mqttHost' value='" + htmlEscape(config.mqttHost) + "'></label><br/>";
  out += "<label>Port: <input type='number' name='mqttPort' min='1' max='65535' value='" + String(config.mqttPort) + "'></label><br/>";
  out += "<label>Username: <input type='text' name='mqttUser' value='" + htmlEscape(config.mqttUser) + "'></label><br/>";
  out += "<label>Password: <input type='password' name='mqttPass' value='" + htmlEscape(config.mqttPass) + "'></label><br/>";
  out += "<label>Topic (subscribe): <input type='text' name='mqttTopic' value='" + htmlEscape(config.mqttTopic) + "'></label><br/>";
  out += "<button type='submit'>Save</button>";
  out += "</form>";
  out += "</section>";

  out += "<section><h2>Time</h2>";
  out += "<form method='POST' action='/api/config'>";
  out += "<label>Timezone offset (minutes from UTC): <input type='number' name='tz' min='-720' max='840' value='" + String(config.timezoneOffsetMinutes) + "'></label><br/>";
  out += "<button type='submit'>Save</button>";
  out += "</form>";
  out += "</section>";

  out += renderFooter();
  return out;
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", renderIndexPage());
}

void handleConfigPage() {
  server.send(200, "text/html; charset=utf-8", renderConfigPage());
}

void handleTriggerPost() {
  triggerPulse();
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleIntervalPost() {
  if (!server.hasArg("seconds")) {
    server.send(400, "text/plain", "Missing seconds");
    return;
  }
  uint32_t s = server.arg("seconds").toInt();
  bool en = server.hasArg("enabled");
  config.intervalSeconds = s;
  config.intervalEnabled = en && s > 0;
  saveConfig();
  if (config.intervalEnabled) {
    nextIntervalAt = millis() + (config.intervalSeconds * 1000UL);
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleScheduleAdd() {
  if (!server.hasArg("time")) {
    server.send(400, "text/plain", "Missing time");
    return;
  }
  String t = server.arg("time");
  if (parseTimeToMinutes(t) < 0) {
    server.send(400, "text/plain", "Invalid time format, expected HH:MM");
    return;
  }
  config.scheduleTimes.push_back(t);
  saveConfig();
  scheduleFiredToday.assign(config.scheduleTimes.size(), false);
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleScheduleRemove() {
  if (!server.hasArg("idx")) {
    server.send(400, "text/plain", "Missing idx");
    return;
  }
  int idx = server.arg("idx").toInt();
  if (idx < 0 || (size_t)idx >= config.scheduleTimes.size()) {
    server.send(400, "text/plain", "Invalid idx");
    return;
  }
  config.scheduleTimes.erase(config.scheduleTimes.begin() + idx);
  saveConfig();
  scheduleFiredToday.assign(config.scheduleTimes.size(), false);
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleConfigPost() {
  bool needApplyPin = false;
  bool needReconnectMqtt = false;

  if (server.hasArg("triggerPin")) {
    int newPin = server.arg("triggerPin").toInt();
    if (newPin != config.triggerPin) {
      config.triggerPin = newPin;
      needApplyPin = true;
    }
  }
  if (server.hasArg("activeLevel")) {
    bool high = (server.arg("activeLevel") == "HIGH");
    config.triggerActiveHigh = high;
  }
  if (server.hasArg("pulseMs")) {
    uint32_t ms = max(1, server.arg("pulseMs").toInt());
    config.triggerDurationMs = ms;
  }
  if (server.hasArg("mqttHost")) {
    String newHost = server.arg("mqttHost");
    if (newHost != config.mqttHost) { config.mqttHost = newHost; needReconnectMqtt = true; }
  }
  if (server.hasArg("mqttPort")) {
    uint16_t newPort = (uint16_t) server.arg("mqttPort").toInt();
    if (newPort != config.mqttPort) { config.mqttPort = newPort; needReconnectMqtt = true; }
  }
  if (server.hasArg("mqttUser")) {
    String u = server.arg("mqttUser");
    if (u != config.mqttUser) { config.mqttUser = u; needReconnectMqtt = true; }
  }
  if (server.hasArg("mqttPass")) {
    String p = server.arg("mqttPass");
    if (p != config.mqttPass) { config.mqttPass = p; needReconnectMqtt = true; }
  }
  if (server.hasArg("mqttTopic")) {
    String t = server.arg("mqttTopic");
    if (t != config.mqttTopic) { config.mqttTopic = t; needReconnectMqtt = true; }
  }
  if (server.hasArg("tz")) {
    config.timezoneOffsetMinutes = server.arg("tz").toInt();
  }

  saveConfig();

  if (needApplyPin) {
    applyTriggerPin();
  }
  if (needReconnectMqtt) {
    if (mqttClient.connected()) mqttClient.disconnect();
    connectMqtt();
  }

  server.sendHeader("Location", "/config");
  server.send(303);
}

void handleStatusJson() {
  DynamicJsonDocument doc(1024);
  doc["ip"] = WiFi.isConnected() ? WiFi.localIP().toString() : "Not connected";
  doc["mqttConnected"] = mqttClient.connected();
  doc["triggerPin"] = config.triggerPin;
  doc["triggerActiveHigh"] = config.triggerActiveHigh;
  doc["triggerDurationMs"] = config.triggerDurationMs;
  doc["intervalEnabled"] = config.intervalEnabled;
  doc["intervalSeconds"] = config.intervalSeconds;
  doc["timezoneOffsetMinutes"] = config.timezoneOffsetMinutes;
  JsonArray sched = doc.createNestedArray("scheduleTimes");
  for (auto &t : config.scheduleTimes) sched.add(t);
  String body;
  serializeJson(doc, body);
  server.send(200, "application/json", body);
}

void handleWifiPortal() {
  // Starts a blocking WiFiManager config portal
  server.send(200, "text/html; charset=utf-8",
              "<!DOCTYPE html><html><body><p>Starting WiFi config portal...</p><p><a href='/'>Back</a></p></body></html>");
  server.client().stop(); // close client so the portal can take over
  delay(200);
  WiFiManager wm;
  String apName = "Diffuser-" + String(ESP.getChipId(), HEX);
  wm.startConfigPortal(apName.c_str());
}

// ================== Setup/Loop ==================
void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/config", HTTP_GET, handleConfigPage);
  server.on("/api/trigger", HTTP_POST, handleTriggerPost);
  server.on("/api/interval", HTTP_POST, handleIntervalPost);
  server.on("/api/schedule/add", HTTP_POST, handleScheduleAdd);
  server.on("/api/schedule/remove", HTTP_POST, handleScheduleRemove);
  server.on("/api/config", HTTP_POST, handleConfigPost);
  server.on("/api/status", HTTP_GET, handleStatusJson);
  server.on("/api/wifi-portal", HTTP_GET, handleWifiPortal);
  server.begin();
  Serial.println("HTTP server started");
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setConfigPortalBlocking(true);
  wm.setConfigPortalTimeout(180); // 3 minutes
  String apName = "Diffuser-" + String(ESP.getChipId(), HEX);
  if (!wm.autoConnect(apName.c_str())) {
    Serial.println("WiFi: Failed to connect or portal timeout. Rebooting...");
    delay(1000);
    ESP.restart();
  }
  Serial.print("WiFi connected: ");
  Serial.println(WiFi.localIP());
}

void setupTime() {
  // Get UTC; we'll apply offset manually
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("NTP requested");
}

void setupMQTT() {
  if (config.mqttHost.length() == 0) {
    Serial.println("MQTT host not set, skipping MQTT");
    return;
  }
  connectMqtt();
}

void setup() {
  Serial.begin(115200);
  delay(200);

  loadConfig();
  applyTriggerPin();

  setupWiFi();
  setupTime();

  setupWebServer();
  setupMQTT();

  scheduleFiredToday.assign(config.scheduleTimes.size(), false);
  Serial.println("Setup complete");
}

void loop() {
  // Web server
  server.handleClient();

  // MQTT service
  if (config.mqttHost.length() > 0) {
    if (!mqttClient.connected()) {
      static unsigned long lastReconnectAttempt = 0;
      if (millis() - lastReconnectAttempt > 5000) {
        lastReconnectAttempt = millis();
        connectMqtt();
      }
    } else {
      mqttClient.loop();
    }
  }

  // Finish trigger pulse
  if (triggerInProgress) {
    if (millis() - lastTriggerAt >= config.triggerDurationMs) {
      // drive inactive
      digitalWrite(config.triggerPin, config.triggerActiveHigh ? LOW : HIGH);
      triggerInProgress = false;
      Serial.println("Trigger: OFF");
    }
  }

  // Interval handling
  if (config.intervalEnabled && config.intervalSeconds > 0) {
    if (nextIntervalAt == 0) {
      nextIntervalAt = millis() + (config.intervalSeconds * 1000UL);
    } else if ((long)(millis() - nextIntervalAt) >= 0) {
      triggerPulse();
      nextIntervalAt += (config.intervalSeconds * 1000UL);
    }
  }

  // Schedule handling (check once per second)
  time_t now_utc = time(nullptr);
  if (now_utc != lastTimeCheck) {
    lastTimeCheck = now_utc;
    struct tm tm_local;
    time_t epoch_local;
    getLocalTimeBrokenDown(tm_local, epoch_local);

    // New day?
    if (tm_local.tm_yday != lastDayOfYear) {
      resetScheduleFlagsForNewDay(tm_local);
      Serial.println("New day: reset schedule flags");
    }

    int currentMin = tm_local.tm_hour * 60 + tm_local.tm_min;
    int sec = tm_local.tm_sec;

    if (sec == 0) {
      for (size_t i = 0; i < config.scheduleTimes.size(); i++) {
        int schedMin = parseTimeToMinutes(config.scheduleTimes[i]);
        if (schedMin >= 0 && !scheduleFiredToday[i] && schedMin == currentMin) {
          triggerPulse();
          scheduleFiredToday[i] = true;
          Serial.printf("Scheduled trigger at %s\n", config.scheduleTimes[i].c_str());
        }
      }
    }
  }
}