#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>

// WiFi credentials - replace with your actual credentials
const char* ssid = "TALKTALKAF872A";
const char* password = "MM6PB6F9";

// Static IP configuration - adjust to match your network
IPAddress local_IP(192, 168, 1, 100);      // The fixed IP you want for the ESP32
IPAddress gateway(192, 168, 1, 1);         // Your router's IP
IPAddress subnet(255, 255, 255, 0);        // Subnet mask
IPAddress primaryDNS(192, 168, 1, 1);      // Use router as DNS (same as gateway)
IPAddress secondaryDNS(0, 0, 0, 0);        // No secondary DNS

// NTP Server configuration
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0;              // GMT offset in seconds (0 for GMT/UTC)
const int daylightOffset_sec = 0;          // Daylight saving offset (0 if not applicable)

// API endpoint
const char* apiUrl = "https://www.solaxcloud.com/proxyApp/proxy/api/getRealtimeInfo.do?tokenId=20250923191659058329103&sn=SNEMEBBWXD";

// GPIO pin for relay/output
const int OUTPUT_PIN = 2;
const int INDICATOR_PIN = 4;

// Timing
const unsigned long POLL_INTERVAL = 5 * 60 * 1000; // 5 minutes in milliseconds
const unsigned long DATA_FRESHNESS_THRESHOLD = 15 * 60; // 15 minutes in seconds
unsigned long lastPollTime = 0;

// State variables
float currentSoc = 0.0;
float socThreshold = 90.0;
bool manualOverride = false;
bool manualState = false;
String lastUpdateTime = "";
bool lastApiSuccess = false;
bool dataIsFresh = false;
time_t lastDataTimestamp = 0;
int dataAgeMinutes = -1;

// Web server
WebServer server(80);

// Preferences for persistent storage
Preferences preferences;

// Function declarations
void connectWiFi();
void pollSolarAPI();
void updateOutputState();
void setupWebServer();
void handleRoot();
void handleSetThreshold();
void handleManualControl();
void handleGetStatus();
void saveSettings();
void loadSettings();
void initTime();
time_t parseUploadTime(String uploadTimeStr);
bool checkDataFreshness(String uploadTimeStr);

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n=== ESP32 Solar Monitor Starting ===");
  
  // Initialize GPIO
  pinMode(OUTPUT_PIN, OUTPUT);
  digitalWrite(OUTPUT_PIN, LOW);
  pinMode(INDICATOR_PIN, OUTPUT);
  digitalWrite(INDICATOR_PIN, LOW);
  
  // Load saved settings
  loadSettings();
  
  // Give system time to initialize before WiFi
  delay(500);
  
  // Connect to WiFi
  connectWiFi();
  
  // Initialize time from NTP
  initTime();
  
  // Setup web server
  setupWebServer();
  
  // Initial API poll
  pollSolarAPI();
  
  Serial.println("Setup complete!");
  Serial.print("Access web UI at: http://");
  Serial.println(WiFi.localIP());
}

void loop() {
  // Handle web server requests
  server.handleClient();
  
  // Poll API every 5 minutes
  if (millis() - lastPollTime >= POLL_INTERVAL) {
    pollSolarAPI();
  }
}

void connectWiFi() {
  // Ensure WiFi is properly initialized
  WiFi.mode(WIFI_STA);
  delay(100);
  
  // Configure static IP
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("Static IP configuration failed!");
  }
  
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFailed to connect to WiFi!");
  }
}

void initTime() {
  Serial.println("Initializing NTP time...");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  // Wait for time to be set
  int attempts = 0;
  time_t now = time(nullptr);
  while (now < 24 * 3600 && attempts < 20) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    attempts++;
  }
  
  if (now > 24 * 3600) {
    Serial.println("\nNTP time synchronized!");
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    Serial.print("Current time: ");
    Serial.println(asctime(&timeinfo));
  } else {
    Serial.println("\nFailed to synchronize NTP time!");
  }
}

time_t parseUploadTime(String uploadTimeStr) {
  // Parse "2026-01-18 11:59:46" format
  struct tm tm;
  memset(&tm, 0, sizeof(struct tm));
  
  int year, month, day, hour, minute, second;
  if (sscanf(uploadTimeStr.c_str(), "%d-%d-%d %d:%d:%d", 
             &year, &month, &day, &hour, &minute, &second) == 6) {
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    
    // Convert to time_t (assuming uploadTime is in GMT/UTC based on your timezone)
    // Adjust gmtOffset_sec at the top if uploadTime is in a different timezone
    return mktime(&tm) - gmtOffset_sec;
  }
  
  return 0;
}

bool checkDataFreshness(String uploadTimeStr) {
  time_t uploadTimestamp = parseUploadTime(uploadTimeStr);
  time_t currentTime = time(nullptr);
  
  if (uploadTimestamp == 0 || currentTime < 24 * 3600) {
    Serial.println("Failed to parse time or NTP not synchronized");
    dataAgeMinutes = -1;
    return false;
  }
  
  // Calculate age in seconds
  long ageSeconds = currentTime - uploadTimestamp;
  dataAgeMinutes = ageSeconds / 60;
  
  Serial.print("Data age: ");
  Serial.print(dataAgeMinutes);
  Serial.println(" minutes");
  
  bool isFresh = (ageSeconds >= 0 && ageSeconds <= DATA_FRESHNESS_THRESHOLD);
  
  if (!isFresh) {
    if (ageSeconds < 0) {
      Serial.println("WARNING: Upload time is in the future!");
    } else {
      Serial.println("WARNING: Data is stale (older than 15 minutes)");
    }
  }
  
  return isFresh;
}

void pollSolarAPI() {
  Serial.println("\n--- Polling Solar API ---");
  lastPollTime = millis();
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, reconnecting...");
    connectWiFi();
    return;
  }
  
  HTTPClient http;
  http.begin(apiUrl);
  http.setTimeout(10000);
  
  int httpCode = http.GET();
  
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    Serial.println("API Response received");
    
    // Parse JSON
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (error) {
      Serial.print("JSON parsing failed: ");
      Serial.println(error.c_str());
      lastApiSuccess = false;
      dataIsFresh = false;
    } else {
      bool success = doc["success"];
      
      if (success) {
        String uploadTime = doc["result"]["uploadTime"].as<String>();
        
        // Check data freshness
        dataIsFresh = checkDataFreshness(uploadTime);
        
        if (dataIsFresh) {
          // Only update SOC if data is fresh
          currentSoc = doc["result"]["soc"];
          lastUpdateTime = uploadTime;
          lastDataTimestamp = parseUploadTime(uploadTime);
          
          Serial.print("SOC: ");
          Serial.print(currentSoc);
          Serial.println("%");
          Serial.print("Last Update: ");
          Serial.println(lastUpdateTime);
          Serial.println("✓ Data is fresh - using for control");
          
          lastApiSuccess = true;
          
          // Update output based on SOC
          updateOutputState();
        } else {
          Serial.println("✗ Data is stale - NOT using for control");
          lastApiSuccess = false;
          // Keep previous SOC value but mark as stale
          lastUpdateTime = uploadTime;
          
          // In auto mode with stale data, turn off for safety
          if (!manualOverride) {
            Serial.println("Auto mode with stale data - turning OFF for safety");
            digitalWrite(OUTPUT_PIN, LOW);
            digitalWrite(INDICATOR_PIN, LOW);
          }
        }
      } else {
        Serial.println("API returned success=false");
        lastApiSuccess = false;
        dataIsFresh = false;
      }
    }
  } else {
    Serial.print("HTTP request failed, error code: ");
    Serial.println(httpCode);
    lastApiSuccess = false;
    dataIsFresh = false;
  }
  
  http.end();
}

void updateOutputState() {
  bool shouldBeOn;
  
  if (manualOverride) {
    shouldBeOn = manualState;
    Serial.println("Manual override active");
  } else {
    // Only use SOC data if it's fresh
    if (dataIsFresh) {
      shouldBeOn = (currentSoc >= socThreshold);
      Serial.print("Auto mode: SOC ");
      Serial.print(currentSoc);
      Serial.print("% ");
      Serial.print(shouldBeOn ? ">=" : "<");
      Serial.print(" threshold ");
      Serial.print(socThreshold);
      Serial.println("%");
    } else {
      // Default to OFF if data is stale
      shouldBeOn = false;
      Serial.println("Auto mode: Data stale - defaulting to OFF");
    }
  }
  
  digitalWrite(OUTPUT_PIN, shouldBeOn ? HIGH : LOW);
  digitalWrite(INDICATOR_PIN, shouldBeOn ? HIGH : LOW);
  Serial.print("Output: ");
  Serial.println(shouldBeOn ? "ON" : "OFF");
}

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/setThreshold", HTTP_POST, handleSetThreshold);
  server.on("/manualControl", HTTP_POST, handleManualControl);
  server.on("/getStatus", HTTP_GET, handleGetStatus);
  
  server.begin();
  Serial.println("Web server started");
}

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Solar Monitor Control</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 20px; background: #f0f0f0; }
    .container { max-width: 600px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
    h1 { color: #333; text-align: center; }
    .status-box { background: #e8f4f8; padding: 15px; border-radius: 5px; margin: 15px 0; }
    .status-item { margin: 8px 0; }
    .label { font-weight: bold; color: #555; }
    .value { color: #000; }
    .fresh { color: #4CAF50; font-weight: bold; }
    .stale { color: #f44336; font-weight: bold; }
    .control-section { margin: 20px 0; padding: 15px; background: #f9f9f9; border-radius: 5px; }
    input[type="number"] { width: 100px; padding: 8px; font-size: 16px; }
    button { background: #4CAF50; color: white; border: none; padding: 10px 20px; font-size: 16px; border-radius: 5px; cursor: pointer; margin: 5px; }
    button:hover { background: #45a049; }
    .btn-danger { background: #f44336; }
    .btn-danger:hover { background: #da190b; }
    .btn-warning { background: #ff9800; }
    .btn-warning:hover { background: #e68900; }
    .output-state { font-size: 24px; font-weight: bold; padding: 10px; text-align: center; border-radius: 5px; }
    .output-on { background: #4CAF50; color: white; }
    .output-off { background: #ccc; color: #666; }
    .warning-box { background: #fff3cd; border: 2px solid #ffc107; padding: 10px; border-radius: 5px; margin: 10px 0; color: #856404; }
  </style>
</head>
<body>
  <div class="container">
    <h1>☀️ Solar Monitor Control</h1>
    
    <div class="status-box">
      <h2>Current Status</h2>
      <div class="status-item"><span class="label">SOC:</span> <span class="value" id="soc">--</span>%</div>
      <div class="status-item"><span class="label">Last Update:</span> <span class="value" id="lastUpdate">--</span></div>
      <div class="status-item"><span class="label">Data Age:</span> <span class="value" id="dataAge">--</span></div>
      <div class="status-item"><span class="label">Data Status:</span> <span class="value" id="dataFreshness">--</span></div>
      <div class="status-item"><span class="label">API Status:</span> <span class="value" id="apiStatus">--</span></div>
      <div class="status-item"><span class="label">Mode:</span> <span class="value" id="mode">--</span></div>
    </div>
    
    <div id="staleWarning" class="warning-box" style="display:none;">
      ⚠️ Warning: Solar data is stale (older than 15 minutes). In auto mode, heater is turned OFF for safety.
    </div>
    
    <div class="output-state" id="outputState">--</div>
    
    <div class="control-section">
      <h3>Manual Control</h3>
      <button onclick="manualOn()">ON</button>
      <button class="btn-danger" onclick="manualOff()">OFF</button>
      <button class="btn-warning" onclick="autoMode()">Auto Mode</button>
      <div id="manualMsg"></div>
    </div>

    <div class="control-section">
      <h3>Threshold Setting</h3>
      <label>SOC Threshold (%): <input type="number" id="threshold" min="0" max="100" step="1" value="90"></label>
      <button onclick="setThreshold()">Set Threshold</button>
      <div id="thresholdMsg"></div>
    </div>
  </div>
  
  <script>
    function updateStatus() {
      fetch('/getStatus')
        .then(response => response.json())
        .then(data => {
          document.getElementById('soc').textContent = data.soc.toFixed(1);
          document.getElementById('lastUpdate').textContent = data.lastUpdate;
          
          // Data age display
          if (data.dataAgeMinutes >= 0) {
            document.getElementById('dataAge').textContent = data.dataAgeMinutes + ' minutes';
          } else {
            document.getElementById('dataAge').textContent = 'Unknown';
          }
          
          // Data freshness indicator
          const freshnessSpan = document.getElementById('dataFreshness');
          if (data.dataIsFresh) {
            freshnessSpan.textContent = '✓ Fresh';
            freshnessSpan.className = 'value fresh';
            document.getElementById('staleWarning').style.display = 'none';
          } else {
            freshnessSpan.textContent = '✗ Stale';
            freshnessSpan.className = 'value stale';
            if (!data.manualOverride) {
              document.getElementById('staleWarning').style.display = 'block';
            } else {
              document.getElementById('staleWarning').style.display = 'none';
            }
          }
          
          document.getElementById('apiStatus').textContent = data.apiSuccess ? '✓ Connected' : '✗ Error';
          document.getElementById('mode').textContent = data.manualOverride ? 'Manual' : 'Auto (Threshold: ' + data.threshold + '%)';
          document.getElementById('threshold').value = data.threshold;
          
          const outputDiv = document.getElementById('outputState');
          if (data.outputState) {
            outputDiv.textContent = 'Heater ON';
            outputDiv.className = 'output-state output-on';
          } else {
            outputDiv.textContent = 'Heater OFF'; 
            outputDiv.className = 'output-state output-off';
          }
        });
    }
    
    function setThreshold() {
      const threshold = document.getElementById('threshold').value;
      fetch('/setThreshold', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'threshold=' + threshold
      })
      .then(response => response.text())
      .then(data => {
        document.getElementById('thresholdMsg').textContent = '✓ Threshold updated';
        setTimeout(() => { document.getElementById('thresholdMsg').textContent = ''; }, 2000);
        updateStatus();
      });
    }
    
    function manualOn() {
      fetch('/manualControl', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'state=on'
      })
      .then(response => response.text())
      .then(data => {
        document.getElementById('manualMsg').textContent = '✓ Manual ON';
        setTimeout(() => { document.getElementById('manualMsg').textContent = ''; }, 2000);
        updateStatus();
      });
    }
    
    function manualOff() {
      fetch('/manualControl', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'state=off'
      })
      .then(response => response.text())
      .then(data => {
        document.getElementById('manualMsg').textContent = '✓ Manual OFF';
        setTimeout(() => { document.getElementById('manualMsg').textContent = ''; }, 2000);
        updateStatus();
      });
    }
    
    function autoMode() {
      fetch('/manualControl', {
        method: 'POST',
        headers: {'Content-Type': 'application/x-www-form-urlencoded'},
        body: 'state=auto'
      })
      .then(response => response.text())
      .then(data => {
        document.getElementById('manualMsg').textContent = '✓ Auto mode enabled';
        setTimeout(() => { document.getElementById('manualMsg').textContent = ''; }, 2000);
        updateStatus();
      });
    }
    
    // Update status every 2 seconds
    setInterval(updateStatus, 2000);
    updateStatus();
  </script>
</body>
</html>
)rawliteral";
  
  server.send(200, "text/html", html);
}

void handleSetThreshold() {
  if (server.hasArg("threshold")) {
    socThreshold = server.arg("threshold").toFloat();
    saveSettings();
    updateOutputState();
    Serial.print("Threshold updated to: ");
    Serial.println(socThreshold);
  }
  server.send(200, "text/plain", "OK");
}

void handleManualControl() {
  if (server.hasArg("state")) {
    String state = server.arg("state");
    
    if (state == "on") {
      manualOverride = true;
      manualState = true;
    } else if (state == "off") {
      manualOverride = true;
      manualState = false;
    } else if (state == "auto") {
      manualOverride = false;
    }
    
    saveSettings();
    updateOutputState();
    Serial.print("Manual control: ");
    Serial.println(state);
  }
  server.send(200, "text/plain", "OK");
}

void handleGetStatus() {
  JsonDocument doc;
  doc["soc"] = currentSoc;
  doc["threshold"] = socThreshold;
  doc["manualOverride"] = manualOverride;
  doc["manualState"] = manualState;
  doc["outputState"] = digitalRead(OUTPUT_PIN);
  doc["lastUpdate"] = lastUpdateTime;
  doc["apiSuccess"] = lastApiSuccess;
  doc["dataIsFresh"] = dataIsFresh;
  doc["dataAgeMinutes"] = dataAgeMinutes;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void saveSettings() {
  preferences.begin("solar", false);
  preferences.putFloat("threshold", socThreshold);
  preferences.putBool("manualOvr", manualOverride);
  preferences.putBool("manualSt", manualState);
  preferences.end();
}

void loadSettings() {
  // Open in read-write mode to create namespace if it doesn't exist
  if (!preferences.begin("solar", false)) {
    Serial.println("Failed to initialize preferences");
    return;
  }
  
  socThreshold = preferences.getFloat("threshold", 90.0);
  manualOverride = preferences.getBool("manualOvr", false);
  manualState = preferences.getBool("manualSt", false);
  preferences.end();
  
  Serial.println("Loaded settings:");
  Serial.print("  Threshold: ");
  Serial.println(socThreshold);
  Serial.print("  Manual Override: ");
  Serial.println(manualOverride);
}