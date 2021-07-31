/*
  1.0.0.0
    - Initial version.
    - Using AsyncElegantOTA library for asynchronous over the air updates.
    - Using AsyncMqttClient library for asynchronous mqtt communication.
    - Using ESPAsyncWebServer library for asynchronous web server to provide information and reboot possibility.
    - Using ESPAsyncWiFiManager library for asynchronous wifi setup with custom parameters for mqtt.
    - Using mideaAC library for serial communication with the air conditioner.
*/

#include <Arduino.h>
#include <ArduinoJson.h>
#include <AsyncElegantOTA.h>
#include <AsyncMqttClient.h>
#include <ESPAsyncWebServer.h> 
#include <ESPAsyncWiFiManager.h>
#include <mideaAC.h>
#include <Ticker.h>

#if defined ESP8266
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#elif defined ESP32
#include <SPIFFS.h>
#include <WiFi.h>
#endif

#define VERSION "1.0.0"

// sketch version
String sketchVersion = "midea-ac-async:" + String(VERSION);

// web server
AsyncWebServer webServer(80);

// mqtt client
AsyncMqttClient mqttClient;
Ticker mqttReconnectTimer;
char command[32];
char localIp[32];
char lwt[32];
char rssi[32];
char state[32];
char version[32];

// wifi
DNSServer dns;
AsyncWiFiManager wifiManager(&webServer,&dns);

char hostname[24];

// default mqtt configuration
char mqttServer[40] = "192.168.178.79";
char mqttPort[6] = "1883";

// flag for saving custom configuration
bool shouldSaveConfig = false;

// ac serial
acSerial acSerialClient;
long lastAcSerialMessage = 0;

// callback notifying us of the need to save custom configuration
void onSaveConfig () {
  shouldSaveConfig = true;
}

// method for connecting to mqtt server
void connectToMqtt() {
  mqttClient.setClientId(hostname);
  mqttClient.setWill(lwt, 0, true, "Offline");

  mqttClient.connect();
}

// callback on mqtt connection established configuring subscriptions and publish state information
void onMqttConnect(bool isSessionPresent) {
  mqttClient.subscribe(command, 0);
  
  mqttClient.publish(lwt, 0, true, "Online");
  mqttClient.publish(localIp, 0, true, WiFi.localIP().toString().c_str());
  mqttClient.publish(rssi, 0, true, String(WiFi.RSSI()).c_str());
  mqttClient.publish(version, 0, true, sketchVersion.c_str());
}

// callback on mqtt connection lost
// if wifi is connected, trying to connect every n seconds to mqtt server
void onMqttDisconnect(AsyncMqttClientDisconnectReason disconnectReason) {
  if (WiFi.isConnected()) {
    mqttReconnectTimer.once(2, connectToMqtt);
  }
}

// callback on mqtt message on subscribed topic received
void onMqttMessageReceived(char* topic, char* message, AsyncMqttClientMessageProperties properties, size_t length, size_t index, size_t total) {
  StaticJsonDocument<256> doc;
  deserializeJson(doc, message, length);

  if (strcmp(topic, command) == 0) {
    bool on = doc["on"];
    bool turbo = doc["turbo"];
    bool eco = doc["eco"];
    ac_mode_t mode = (ac_mode_t)((uint8_t)doc["mode"]);
    bool lamelle = doc["lamelle"];
    uint8_t fan = doc["fan"];
    uint8_t soll = doc["soll"];

    acSerialClient.send_conf_h(on, soll, fan, mode, lamelle, turbo, eco);
  }
}

// callback on wifi event
// if connection established, trying to connect to mqtt server
// if connection lost, deactiving time for mqtt reconnect attempts
void onWifiEvent(WiFiEvent_t event) {
  switch (event) {
#if defined ESP8266
    case WIFI_EVENT_STAMODE_GOT_IP:
#elif defined ESP32
    case SYSTEM_EVENT_STA_GOT_IP:
#endif
      connectToMqtt();
      break;
#if defined ESP8266
    case WIFI_EVENT_STAMODE_DISCONNECTED:
#elif defined ESP32
    case SYSTEM_EVENT_STA_DISCONNECTED:
#endif
      mqttReconnectTimer.detach();
      break;
  }
}

// callback providing basic information on web request
void onRoot(AsyncWebServerRequest *request) {
  String message;
  message += "</p>\n";
#if defined ESP8266
  message += "<H2>ESP8266</H2>\n<p>";
  message += "Chip ID: " + String(ESP.getChipId()) + "<br />\n";
#elif defined ESP32
  message += "<H2>ESP32</H2>\n<p>";
#endif
  message += "CPU Frequenz: " + String(ESP.getCpuFreqMHz()) + " MHz<br />\n";
  message += "SDK Version: " + String(ESP.getSdkVersion()) + "<br />\n";
  message += "Sketch Version: " + sketchVersion + "<br />\n";
  message += "</p>\n";
  message += "<H2>WiFi</H2>\n<p>";
  message += "IP: " + WiFi.localIP().toString() + "<br />\n";
  message += "Hostname: " + String(WiFi.getHostname()) + "<br />\n";
  message += "MAC: " + WiFi.macAddress() + "<br />\n";
  message += "SSID: " + String(WiFi.SSID()) + "<br />\n";
  message += "Mask: " + WiFi.subnetMask().toString() + "<br />\n";
  message += "GW: " + WiFi.gatewayIP().toString() + "<br />\n";
  message += "RSSI: " + String(WiFi.RSSI()) + "<br />\n";
  message += "</p>\n";
  message += "<H2>Flash</H2>\n<p>";
#if defined ESP8266
  message += "Heap Fragmentation: " + String(ESP.getHeapFragmentation()) + "<br />\n";
#elif defined ESP32
  message += "Heap Size: " + String(ESP.getHeapSize()) + "<br />\n";
#endif
  message += "Free Heap Size: " + String(ESP.getFreeHeap()) + "<br />\n";
  message += "Sketch Size: " + String(ESP.getSketchSize()) + "<br />\n";
  message += "Free Sketch Size: " + String(ESP.getFreeSketchSpace()) + "<br />\n";
  message += "</p>\n";

  request->send(200, "text/html", message);
}

// callback providing not found message on invalid web request
void onNotFound(AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found...");
}

// callback providing reset wifi settings on web request
void onReset(AsyncWebServerRequest *request) {
  request->send(200, "text/plain", "Reset settings...");
  wifiManager.resetSettings();
}

// callback providing reboot device on web request
void onReboot(AsyncWebServerRequest *request) {
  request->send(200, "text/plain", "Reboot device...");
  ESP.restart();
}

// callback on ac serial status events, publishing state to mqtt broker
void onAcSerialEvent(ac_status_t * status) {
  if (mqttClient.connected()) {
    DynamicJsonDocument doc(1024);
    JsonArray array = doc.to<JsonArray>();
    
    JsonObject statusObj = array.createNestedObject();
    statusObj["ist"] = status->ist;
    statusObj["aussen"] = status->aussen;

    JsonObject confObj = statusObj.createNestedObject("conf");
    confObj["on"] = status->conf.on;
    confObj["turbo"] = status->conf.turbo;
    confObj["eco"] = status->conf.eco;
    confObj["soll"] = status->conf.soll;
    confObj["lamelle"] = status->conf.lamelle != acLamelleOff;
    confObj["mode"] = (uint8_t)status->conf.mode;

    switch (status->conf.fan) {
      case acFAN1:
        confObj["fan"] = 1;
        break;
      case acFAN2:
        confObj["fan"] = 2;
        break;
      case acFAN3:
        confObj["fan"] = 3;
        break;
      case acFANA:
        confObj["fan"] = 0;
        break;
      default:
        confObj["fan"] = (uint8_t)status->conf.fan;
        break;
    }

    String output;
    serializeJson(doc, output);
    char charBuf[output.length() + 1];
    output.toCharArray(charBuf, output.length() + 1);

    mqttClient.publish(state, 0, false, charBuf);
  } 
}

// generating unique hostname based on mac address of the device
void setupHostname() {
#if defined ESP8266
  sprintf(hostname, "ESP8266-%06X", ESP.getChipId());
#elif defined ESP32
  sprintf(hostname, "ESP32-%06llX", ESP.getEfuseMac());
#endif
}

// trying to mount the file system and read in the custom configuration parameters from config file
void setupFileSystem() {
#if defined ESP8266
  // if first try to open file system does not work, then format file system for further usage
  if (!LittleFS.begin()) {
    LittleFS.format();
  }

  if (LittleFS.begin()) {
    if (LittleFS.exists("/config.json")) {
      File configFile = LittleFS.open("/config.json", "r");
#elif defined ESP32
  if (SPIFFS.begin(true)) {
    if (SPIFFS.exists("/config.json")) {
      File configFile = SPIFFS.open("/config.json", "r");
#endif
      if (configFile) {
        String json;
        while (configFile.available()) {
          json += (char)configFile.read();
        }
        configFile.close();

        DynamicJsonDocument jsonDocument(2048);
        DeserializationError err = deserializeJson(jsonDocument, json);
        
        if (!err) {
          strcpy(mqttServer, jsonDocument["mqttServer"]);
          strcpy(mqttPort, jsonDocument["mqttPort"]);
        }
      } 
    } 
  } 
}

// configuring mqtt client and setting callbacks for connection and message handling
void setupMqtt() {
  strcpy(command, hostname);
  strcat(command, "/command");
  strcpy(localIp, hostname);
  strcat(localIp, "/localIp");
  strcpy(lwt, hostname);
  strcat(lwt, "/lwt");
  strcpy(rssi, hostname);
  strcat(rssi, "/rssi");
  strcpy(state, hostname);
  strcat(state, "/state");
  strcpy(version, hostname);
  strcat(version, "/version");

  mqttClient.setServer(mqttServer, atol(mqttPort));

  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onMessage(onMqttMessageReceived);
}

// when device starts up, it is set up in station mode and tries to connect to a previously saved access point
// if this is unsuccessful or if no previous network was saved it moves the device into access point mode 
// and spins up a dns and a web server with default ip 192.168.4.1 for further configuration
void setupWifi() {
  WiFi.setHostname(hostname);
  WiFi.onEvent(onWifiEvent);

  AsyncWiFiManagerParameter customMqttServer("server", "mqtt server", mqttServer, 40);
  AsyncWiFiManagerParameter customMqttPort("port", "mqtt port", mqttPort, 6);

  wifiManager.addParameter(&customMqttServer);
  wifiManager.addParameter(&customMqttPort);
  wifiManager.setSaveConfigCallback(onSaveConfig);
  wifiManager.autoConnect();

  strcpy(mqttServer, customMqttServer.getValue());
  strcpy(mqttPort, customMqttPort.getValue());

  if (shouldSaveConfig) {
    DynamicJsonDocument jsonDocument (2048);
    jsonDocument["mqttServer"] = mqttServer;
    jsonDocument["mqttPort"] = mqttPort;

#if defined ESP8266
    File configFile = LittleFS.open("/config.json", "w");
#elif defined ESP32
    File configFile = SPIFFS.open("/config.json", "w");
#endif
    serializeJson(jsonDocument, configFile);
    configFile.close();
    shouldSaveConfig = false;
  }
}

// configure web server to provide ota update
void setupOtaUpdate() {
  AsyncElegantOTA.begin(&webServer);
}

// configure and start web server to provide static content, some basic functions like reset and reboot 
void setupWebServer() {
  webServer.on("/", onRoot);
  webServer.on("/reset", onReset);
  webServer.on("/reboot", onReboot);
  webServer.onNotFound(onNotFound);
  webServer.begin();
}

// configure the ac serial communication over uart and set callback on ac status events
void setupAcSerial(){
  acSerialClient.begin((Stream *)&Serial, "Serial");
  acSerialClient.send_getSN();
  acSerialClient.onStatusEvent(onAcSerialEvent);
}

// the setup function runs once when you press reset or power the board
void setup() {
  Serial.begin(9600);

  setupHostname();
  setupFileSystem();
  setupMqtt();
  setupWifi();
  setupOtaUpdate();
  setupWebServer();
  setupAcSerial();
}

// the loop function runs over and over again forever
void loop() {
  acSerialClient.loop();
  long now = millis();
  if (now - lastAcSerialMessage > 30000) {
    lastAcSerialMessage = now;
    acSerialClient.send_status(mqttClient.connected(), true);
    acSerialClient.request_status();
  }
}