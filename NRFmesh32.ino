#include <SPI.h>
#include <nRF24L01.h>
#include <RF24.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

// nrf24 pins
RF24 radio(4, 5); // CE, CSN

struct Settings {
  uint32_t signature;
  char deviceName[16];
  uint8_t channel;
  uint8_t powerLevel;
  uint32_t networkId;
  bool confirmedDelivery;
  char apSsid[32];
  char apPassword[32];
};

#define SETTINGS_SIGNATURE 0xDEADBEEF

//default settings
const Settings defaultSettings = {
  SETTINGS_SIGNATURE,
  "ESP32-Mesh",
  1,
  0,
  12345,
  false,
  "MeshNetwork",
  "mesh12345"
};

Settings settings;

struct MeshMessage {
  uint32_t fromId;
  char fromName[16];
  uint32_t networkId;
  uint8_t msgType;
  uint32_t msgId;
  char text[32];
  uint8_t hopCount;
  uint32_t timestamp;
};

WebServer server(80);

struct StoredMessage {
  MeshMessage msg;
  uint32_t receivedTime;
  int8_t rssi;
};

struct NodeInfo {
  uint32_t nodeId;
  char nodeName[16];
  uint32_t lastSeen;
  uint8_t signalStrength;
};

StoredMessage messages[50];
NodeInfo nodes[20];
int messageCount = 0;
int nodeCount = 0;

const uint64_t addresses[5] = {0xABCDABCD71LL, 0xABCDABCD72LL, 0xABCDABCD73LL, 0xABCDABCD74LL, 0xABCDABCD75LL};

const int ledPin = 2;

uint32_t lastPingTime = 0;
uint32_t myNodeId;
uint32_t lastMessageId = 0;
uint32_t messagesSent = 0;
uint32_t messagesReceived = 0;
uint32_t startupTime = 0;

void handlePingMessage(MeshMessage &msg);
void handleAckMessage(MeshMessage &msg);
void handleNodeInfo(MeshMessage &msg);
void sendAck(uint32_t originalMsgId, uint32_t toNodeId);
void rebroadcastMessage(MeshMessage &msg);
void cleanupNodes();

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("=== Starting Mesh Node ===");
  
  myNodeId = generateNodeId();
  
  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, HIGH);
  
  EEPROM.begin(512);
  loadSettings();
  
  Serial.println("Loaded settings:");
  Serial.print("Device: "); Serial.println(settings.deviceName);
  Serial.print("AP SSID: "); Serial.println(settings.apSsid);
  Serial.print("AP Password: "); Serial.println(settings.apPassword);
  Serial.print("Channel: "); Serial.println(settings.channel);
  Serial.print("Network ID: "); Serial.println(settings.networkId);
  
  startAccessPoint();
  
  initRadio();
  
  setupWebServer();
  
  startupTime = millis();
  
  Serial.println("=== Mesh Node Started ===");
  Serial.print("Web interface: http://");
  Serial.println(WiFi.softAPIP());
  Serial.println("=========================");
}

void loadSettings() {
  Serial.println("Loading settings from EEPROM...");
  
  EEPROM.get(0, settings);
  
  if (settings.signature != SETTINGS_SIGNATURE) {
    Serial.println("Invalid signature, loading default settings");
    
    memcpy(&settings, &defaultSettings, sizeof(Settings));
    
    saveSettings();
    Serial.println("Default settings saved to EEPROM");
  } else {
    Serial.println("Settings loaded successfully from EEPROM");
  }
  
  if (strlen(settings.deviceName) == 0) {
    strncpy(settings.deviceName, defaultSettings.deviceName, 16);
  }
  
  if (strlen(settings.apSsid) == 0) {
    strncpy(settings.apSsid, defaultSettings.apSsid, 32);
  }
  
  if (strlen(settings.apPassword) == 0) {
    strncpy(settings.apPassword, defaultSettings.apPassword, 32);
  }
  
  if (settings.channel > 125) settings.channel = 1;
  if (settings.powerLevel > 3) settings.powerLevel = 0;
  if (settings.networkId == 0) settings.networkId = 12345;
}

void saveSettings() {
  settings.signature = SETTINGS_SIGNATURE;
  
  EEPROM.put(0, settings);
  
  if (EEPROM.commit()) {
    Serial.println("Settings saved to EEPROM successfully");
  } else {
    Serial.println("ERROR: Failed to save settings to EEPROM");
  }
}

void startAccessPoint() {
  Serial.println("Starting Access Point...");
  
  WiFi.persistent(false);
  
  WiFi.disconnect(true);
  delay(100);
  
  Serial.print("Setting up AP with SSID: ");
  Serial.println(settings.apSsid);
  Serial.print("Password: ");
  Serial.println(settings.apPassword);
  
  bool apStarted = WiFi.softAP(settings.apSsid, settings.apPassword);
  
  if (!apStarted) {
    Serial.println("Failed to start Access Point with configured settings!");
    Serial.println("Trying fallback settings...");
    
    String fallbackSSID = "MeshNode-" + String(myNodeId, HEX);
    apStarted = WiFi.softAP(fallbackSSID.c_str(), NULL);
    
    if (apStarted) {
      Serial.println("Fallback AP started successfully");
      strncpy(settings.apSsid, fallbackSSID.c_str(), 32);
      settings.apPassword[0] = '\0';
      saveSettings();
    } else {
      Serial.println("CRITICAL: Failed to start Access Point!");
      return;
    }
  }
  
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());
  Serial.print("AP MAC address: ");
  Serial.println(WiFi.softAPmacAddress());
}

void initRadio() {
  if (!radio.begin()) {
    Serial.println("Radio init failed!");
    return;
  }
  
  setRadioChannel(settings.channel);
  radio.setPALevel(settings.powerLevel);
  radio.setDataRate(RF24_2MBPS);
  radio.setRetries(3, 5);
  radio.enableDynamicPayloads();
  radio.openReadingPipe(1, addresses[settings.channel % 5]);
  radio.startListening();
  
  Serial.println("Radio initialized on channel " + String(settings.channel));
}

void setRadioChannel(uint8_t channel) {
  if (channel > 125) channel = 125;
  radio.setChannel(channel);
  radio.openReadingPipe(1, addresses[channel % 5]);
}

void handleIncomingMessages() {
  if (radio.available()) {
    MeshMessage incoming;
    radio.read(&incoming, sizeof(incoming));
    
    if (incoming.networkId != settings.networkId && incoming.networkId != 0) {
      return;
    }
    
    messagesReceived++;
    
    switch(incoming.msgType) {
      case 0: handleTextMessage(incoming); break;
      case 1: handlePingMessage(incoming); break;
      case 2: handleAckMessage(incoming); break;
      case 3: handleNodeInfo(incoming); break;
    }
    
    digitalWrite(ledPin, LOW);
    delay(50);
    digitalWrite(ledPin, HIGH);
  }
}

void handleTextMessage(MeshMessage &msg) {
  Serial.print("Message from "); Serial.print(msg.fromName);
  Serial.print(": "); Serial.println(msg.text);
  
  if (messageCount < 50) {
    messages[messageCount].msg = msg;
    messages[messageCount].receivedTime = millis();
    messages[messageCount].rssi = -50;
    messageCount++;
  }
  
  updateNodeInfo(msg.fromId, msg.fromName);
  
  if (settings.confirmedDelivery) {
    sendAck(msg.msgId, msg.fromId);
  }
  
  if (msg.hopCount < 3) {
    msg.hopCount++;
    rebroadcastMessage(msg);
  }
}

void handlePingMessage(MeshMessage &msg) {
  Serial.print("Ping from "); Serial.println(msg.fromName);
  updateNodeInfo(msg.fromId, msg.fromName);
}

void handleAckMessage(MeshMessage &msg) {
  Serial.print("ACK from "); Serial.println(msg.fromName);
  updateNodeInfo(msg.fromId, msg.fromName);
}

void handleNodeInfo(MeshMessage &msg) {
  Serial.print("Node info from "); Serial.println(msg.fromName);
  updateNodeInfo(msg.fromId, msg.fromName);
}

void updateNodeInfo(uint32_t nodeId, const char* nodeName) {
  for (int i = 0; i < nodeCount; i++) {
    if (nodes[i].nodeId == nodeId) {
      nodes[i].lastSeen = millis();
      strncpy(nodes[i].nodeName, nodeName, 16);
      return;
    }
  }
  
  if (nodeCount < 20) {
    nodes[nodeCount].nodeId = nodeId;
    strncpy(nodes[nodeCount].nodeName, nodeName, 16);
    nodes[nodeCount].lastSeen = millis();
    nodes[nodeCount].signalStrength = -60;
    nodeCount++;
    Serial.println("New node discovered: " + String(nodeName));
  }
}

void sendTextMessage(const char* text) {
  MeshMessage msg;
  msg.fromId = myNodeId;
  strncpy(msg.fromName, settings.deviceName, 16);
  msg.networkId = settings.networkId;
  msg.msgType = 0;
  msg.msgId = ++lastMessageId;
  strncpy(msg.text, text, 32);
  msg.hopCount = 0;
  msg.timestamp = millis();
  
  radio.stopListening();
  radio.openWritingPipe(addresses[settings.channel % 5]);
  bool success = radio.write(&msg, sizeof(msg));
  radio.startListening();
  
  if (success) {
    messagesSent++;
    Serial.println("Message sent: " + String(text));
  } else {
    Serial.println("Failed to send message");
  }
}

void sendPing() {
  MeshMessage msg;
  msg.fromId = myNodeId;
  strncpy(msg.fromName, settings.deviceName, 16);
  msg.networkId = settings.networkId;
  msg.msgType = 1;
  msg.msgId = ++lastMessageId;
  strncpy(msg.text, "PING", 32);
  msg.hopCount = 0;
  msg.timestamp = millis();
  
  radio.stopListening();
  radio.openWritingPipe(addresses[settings.channel % 5]);
  radio.write(&msg, sizeof(msg));
  radio.startListening();
}

void sendAck(uint32_t originalMsgId, uint32_t toNodeId) {
  MeshMessage msg;
  msg.fromId = myNodeId;
  strncpy(msg.fromName, settings.deviceName, 16);
  msg.networkId = settings.networkId;
  msg.msgType = 2;
  msg.msgId = originalMsgId;
  strncpy(msg.text, "ACK", 32);
  msg.hopCount = 0;
  msg.timestamp = millis();
  
  radio.stopListening();
  radio.openWritingPipe(addresses[settings.channel % 5]);
  radio.write(&msg, sizeof(msg));
  radio.startListening();
}

void rebroadcastMessage(MeshMessage &msg) {
  radio.stopListening();
  radio.openWritingPipe(addresses[settings.channel % 5]);
  radio.write(&msg, sizeof(msg));
  radio.startListening();
}

uint32_t generateNodeId() {
  uint64_t mac = ESP.getEfuseMac();
  uint32_t id = (uint32_t)(mac >> 24);
  return id;
}

void cleanupNodes() {
  for (int i = 0; i < nodeCount; i++) {
    if (millis() - nodes[i].lastSeen > 120000) { 
      Serial.println("Node timeout: " + String(nodes[i].nodeName));
      for (int j = i; j < nodeCount - 1; j++) {
        nodes[j] = nodes[j + 1];
      }
      nodeCount--;
      i--;
    }
  }
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/send", HTTP_POST, handleSend);
  server.on("/messages", HTTP_GET, handleGetMessages);
  server.on("/nodes", HTTP_GET, handleGetNodes);
  server.on("/settings", HTTP_GET, handleGetSettings);
  server.on("/settings", HTTP_POST, handleSaveSettings);
  server.on("/stats", HTTP_GET, handleGetStats);
  server.on("/restart", HTTP_POST, handleRestart);
  
  server.begin();
  Serial.println("HTTP server started");
}

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>Mesh Network</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial, sans-serif; margin: 20px; background: #f0f0f0; }
        .container { max-width: 800px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; }
        .header { text-align: center; margin-bottom: 20px; }
        .ap-info { background: #d4edda; padding: 15px; border-radius: 5px; margin: 15px 0; }
        button { background: #3498db; color: white; border: none; padding: 10px 15px; margin: 5px; border-radius: 4px; cursor: pointer; }
        .message { border: 1px solid #ccc; padding: 10px; margin: 5px 0; border-radius: 4px; }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>Mesh Network</h1>
            <div class="ap-info">
                <strong>Access Point:</strong> )rawliteral" + String(settings.apSsid) + R"rawliteral(<br>
                <strong>IP:</strong> )rawliteral" + WiFi.softAPIP().toString() + R"rawliteral(<br>
                <strong>Device:</strong> )rawliteral" + String(settings.deviceName) + R"rawliteral(
            </div>
        </div>
        
        <div>
            <h3>Send Message</h3>
            <input type="text" id="messageText" placeholder="Type message..." style="width: 70%; padding: 8px;">
            <button onclick="sendMessage()">Send</button>
            <button onclick="sendPing()">Ping Network</button>
        </div>
        
        <div>
            <h3>Active Nodes: <span id="nodeCount">0</span></h3>
            <div id="nodes" style="margin-top: 10px;"></div>
        </div>
        
        <div>
            <h3>Messages</h3>
            <button onclick="loadMessages()">Refresh</button>
            <div id="messages" style="margin-top: 10px;"></div>
        </div>
        
        <div>
            <h3>Settings</h3>
            <button onclick="showSettings()">Change Settings</button>
            <div id="settings" style="display:none; margin-top: 10px;">
                <input type="text" id="newSsid" placeholder="New SSID" style="width: 200px; padding: 8px; margin: 5px;"><br>
                <input type="text" id="newPassword" placeholder="New Password" style="width: 200px; padding: 8px; margin: 5px;"><br>
                <input type="text" id="newDeviceName" placeholder="New Device Name" style="width: 200px; padding: 8px; margin: 5px;"><br>
                <button onclick="saveSettings()">Save Settings</button>
            </div>
        </div>
    </div>

    <script>
        function sendMessage() {
            var text = document.getElementById('messageText').value;
            if (!text) return;
            
            fetch('/send', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: 'message=' + encodeURIComponent(text)
            }).then(() => {
                document.getElementById('messageText').value = '';
                loadMessages();
            });
        }
        
        function sendPing() {
            fetch('/send', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: 'message=PING'
            });
        }
        
        function loadMessages() {
            fetch('/messages')
                .then(r => r.json())
                .then(data => {
                    var html = '';
                    if (data.messages && data.messages.length > 0) {
                        data.messages.forEach(msg => {
                            html += '<div class="message">';
                            html += '<strong>' + msg.fromName + '</strong>: ' + msg.text;
                            html += '</div>';
                        });
                    } else {
                        html = '<p>No messages yet</p>';
                    }
                    document.getElementById('messages').innerHTML = html;
                });
        }
        
        function loadNodes() {
            fetch('/nodes')
                .then(r => r.json())
                .then(data => {
                    var html = '';
                    if (data.nodes && data.nodes.length > 0) {
                        document.getElementById('nodeCount').textContent = data.nodes.length;
                        data.nodes.forEach(node => {
                            html += '<div style="border: 1px solid #ddd; padding: 8px; margin: 4px 0; border-radius: 4px;">';
                            html += '<strong>' + node.nodeName + '</strong>';
                            html += '</div>';
                        });
                    } else {
                        html = '<p>No nodes discovered</p>';
                        document.getElementById('nodeCount').textContent = '0';
                    }
                    document.getElementById('nodes').innerHTML = html;
                });
        }
        
        function showSettings() {
            document.getElementById('settings').style.display = 'block';
        }
        
        function saveSettings() {
            var ssid = document.getElementById('newSsid').value;
            var password = document.getElementById('newPassword').value;
            var deviceName = document.getElementById('newDeviceName').value;
            
            var settings = {};
            if (ssid) settings.apSsid = ssid;
            if (password) settings.apPassword = password;
            if (deviceName) settings.deviceName = deviceName;
            
            fetch('/settings', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify(settings)
            }).then(() => {
                alert('Settings saved! Device will restart.');
                setTimeout(() => location.reload(), 2000);
            });
        }
        
        // Auto-refresh
        setInterval(loadMessages, 3000);
        setInterval(loadNodes, 5000);
        
        // Initial load
        loadMessages();
        loadNodes();
    </script>
</body>
</html>
)rawliteral";
  
  server.send(200, "text/html", html);
}

void handleSend() {
  if (server.hasArg("message")) {
    String message = server.arg("message");
    sendTextMessage(message.c_str());
    server.send(200, "application/json", "{\"status\":\"sent\"}");
  }
}

void handleGetMessages() {
  DynamicJsonDocument doc(4096);
  JsonArray messagesArray = doc.createNestedArray("messages");
  
  for (int i = 0; i < messageCount; i++) {
    JsonObject msg = messagesArray.createNestedObject();
    msg["fromName"] = messages[i].msg.fromName;
    msg["text"] = messages[i].msg.text;
  }
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleGetNodes() {
  DynamicJsonDocument doc(2048);
  JsonArray nodesArray = doc.createNestedArray("nodes");
  
  for (int i = 0; i < nodeCount; i++) {
    JsonObject node = nodesArray.createNestedObject();
    node["nodeName"] = nodes[i].nodeName;
  }
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleGetSettings() {
  DynamicJsonDocument doc(512);
  doc["deviceName"] = settings.deviceName;
  doc["apSsid"] = settings.apSsid;
  doc["apPassword"] = settings.apPassword;
  doc["channel"] = settings.channel;
  doc["networkId"] = settings.networkId;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleSaveSettings() {
  String body = server.arg("plain");
  DynamicJsonDocument doc(512);
  deserializeJson(doc, body);
  
  if (doc.containsKey("deviceName")) {
    strncpy(settings.deviceName, doc["deviceName"], 16);
  }
  if (doc.containsKey("apSsid")) {
    strncpy(settings.apSsid, doc["apSsid"], 32);
  }
  if (doc.containsKey("apPassword")) {
    strncpy(settings.apPassword, doc["apPassword"], 32);
  }
  if (doc.containsKey("channel")) {
    settings.channel = doc["channel"];
  }
  if (doc.containsKey("networkId")) {
    settings.networkId = doc["networkId"];
  }
  
  saveSettings();
  
  server.send(200, "application/json", "{\"status\":\"saved\"}");
  
  delay(1000);
  startAccessPoint();
}

void handleGetStats() {
  DynamicJsonDocument doc(512);
  doc["deviceName"] = settings.deviceName;
  doc["apSsid"] = settings.apSsid;
  doc["apIp"] = WiFi.softAPIP().toString();
  doc["connectedClients"] = WiFi.softAPgetStationNum();
  doc["messagesSent"] = messagesSent;
  doc["messagesReceived"] = messagesReceived;
  doc["activeNodes"] = nodeCount;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleRestart() {
  server.send(200, "application/json", "{\"status\":\"restarting\"}");
  delay(1000);
  ESP.restart();
}

void loop() {
  server.handleClient();
  handleIncomingMessages();
  
  if (millis() - lastPingTime > 30000) {
    sendPing();
    lastPingTime = millis();
  }
  
  cleanupNodes();
  
  static unsigned long lastBlink = 0;
  if (millis() - lastBlink > 1000) {
    digitalWrite(ledPin, !digitalRead(ledPin));
    lastBlink = millis();
  }
  
  delay(10);
}
