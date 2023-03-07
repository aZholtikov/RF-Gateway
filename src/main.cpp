#include "ArduinoJson.h"
#include "ArduinoOTA.h"
#include "ESPAsyncWebServer.h" // https://github.com/aZholtikov/Async-Web-Server
#include "LittleFS.h"
#include "EEPROM.h"
#include "Ticker.h"
#include "RF24.h"
#include "ZHNetwork.h"
#include "ZHConfig.h"

void onBroadcastReceiving(const char *data, const uint8_t *sender);
void onUnicastReceiving(const char *data, const uint8_t *sender);
void onConfirmReceiving(const uint8_t *target, const uint16_t id, const bool status);

void loadConfig(void);
void saveConfig(void);
void setupWebServer(void);

void sendAttributesMessage(void);
void sendKeepAliveMessage(void);
void sendConfigMessage(void);
void sendSensorConfigMessage(uint8_t unit, uint8_t haComponentType, uint8_t rfSensorType, uint16_t rfSensorId, uint8_t haSensorDeviceClass, String valueTemplate,
                             String unitOfMeasurement = "", uint16_t expireAfter = 0, String payloadOn = "", String payloadOff = "");

void checkRadioDataAvailability(void);

typedef struct
{
  uint16_t id{0};
  char message[200]{0};
} espnow_message_t;

struct deviceConfig
{
  String espnowNetName{"DEFAULT"};
  String deviceName = "RF gateway " + String(ESP.getChipId(), HEX);
} config;

std::vector<espnow_message_t> espnowMessage;
std::vector<uint16_t> configMessage;

const String firmware{"1.0"};

bool wasMqttAvailable{false};

uint8_t gatewayMAC[6]{0};

ZHNetwork myNet;
AsyncWebServer webServer(80);
RF24 radio(4, 15);

Ticker gatewayAvailabilityCheckTimer;
bool isGatewayAvailable{false};
void gatewayAvailabilityCheckTimerCallback(void);

Ticker apModeHideTimer;
void apModeHideTimerCallback(void);

Ticker attributesMessageTimer;
bool attributesMessageTimerSemaphore{true};
void attributesMessageTimerCallback(void);

Ticker keepAliveMessageTimer;
bool keepAliveMessageTimerSemaphore{true};
void keepAliveMessageTimerCallback(void);

void setup()
{
  LittleFS.begin();

  Serial.begin(115200);

  loadConfig();

  radio.begin();
  radio.setChannel(120);
  radio.setDataRate(RF24_250KBPS);
  radio.setPALevel(RF24_PA_MAX);
  radio.setPayloadSize(14);
  radio.setAddressWidth(3);
  radio.setCRCLength(RF24_CRC_8);
  radio.openReadingPipe(0, 0xDDEEFF);
  radio.startListening();

  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  myNet.begin(config.espnowNetName.c_str());
  // myNet.setCryptKey("VERY_LONG_CRYPT_KEY"); // If encryption is used, the key must be set same of all another ESP-NOW devices in network.

  myNet.setOnBroadcastReceivingCallback(onBroadcastReceiving);
  myNet.setOnUnicastReceivingCallback(onUnicastReceiving);
  myNet.setOnConfirmReceivingCallback(onConfirmReceiving);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(("RF gateway " + String(ESP.getChipId(), HEX)).c_str(), "12345678");
  apModeHideTimer.once(300, apModeHideTimerCallback);

  setupWebServer();

  ArduinoOTA.begin();

  attributesMessageTimer.attach(60, attributesMessageTimerCallback);
  keepAliveMessageTimer.attach(10, keepAliveMessageTimerCallback);
}

void loop()
{
  if (attributesMessageTimerSemaphore)
    sendAttributesMessage();
  if (keepAliveMessageTimerSemaphore)
    sendKeepAliveMessage();
  if (isGatewayAvailable)
    checkRadioDataAvailability();
  myNet.maintenance();
  ArduinoOTA.handle();
}

void onBroadcastReceiving(const char *data, const byte *sender)
{
  esp_now_payload_data_t incomingData;
  memcpy(&incomingData, data, sizeof(esp_now_payload_data_t));
  if (incomingData.deviceType != ENDT_GATEWAY)
    return;
  if (myNet.macToString(gatewayMAC) != myNet.macToString(sender) && incomingData.payloadsType == ENPT_KEEP_ALIVE)
    memcpy(&gatewayMAC, sender, 6);
  if (myNet.macToString(gatewayMAC) == myNet.macToString(sender) && incomingData.payloadsType == ENPT_KEEP_ALIVE)
  {
    isGatewayAvailable = true;
    DynamicJsonDocument json(sizeof(esp_now_payload_data_t::message));
    deserializeJson(json, incomingData.message);
    bool temp = json["MQTT"] == "online" ? true : false;
    if (wasMqttAvailable != temp)
    {
      wasMqttAvailable = temp;
      if (temp)
      {
        sendConfigMessage();
        sendAttributesMessage();
        sendKeepAliveMessage();
      }
    }
    gatewayAvailabilityCheckTimer.once(15, gatewayAvailabilityCheckTimerCallback);
  }
}

void onUnicastReceiving(const char *data, const byte *sender)
{
  esp_now_payload_data_t incomingData;
  memcpy(&incomingData, data, sizeof(esp_now_payload_data_t));
  if (incomingData.deviceType != ENDT_GATEWAY || myNet.macToString(gatewayMAC) != myNet.macToString(sender))
    return;
  if (incomingData.payloadsType == ENPT_UPDATE)
  {
    WiFi.softAP(("RF gateway " + String(ESP.getChipId(), HEX)).c_str(), "12345678", 1, 0);
    webServer.begin();
    apModeHideTimer.once(300, apModeHideTimerCallback);
  }
  if (incomingData.payloadsType == ENPT_RESTART)
    ESP.restart();
}

void onConfirmReceiving(const uint8_t *target, const uint16_t id, const bool status)
{
  for (uint16_t i{0}; i < espnowMessage.size(); ++i)
  {
    espnow_message_t message = espnowMessage[i];
    if (message.id == id)
    {
      if (status)
        espnowMessage.erase(espnowMessage.begin() + i);
      else
      {
        message.id = myNet.sendUnicastMessage(message.message, gatewayMAC, true);
        espnowMessage.at(i) = message;
      }
    }
  }
}

void loadConfig()
{
  ETS_GPIO_INTR_DISABLE();
  EEPROM.begin(4096);
  if (EEPROM.read(4095) == 254)
  {
    EEPROM.get(0, config);
    EEPROM.end();
  }
  else
  {
    EEPROM.end();
    saveConfig();
  }
  delay(50);
  ETS_GPIO_INTR_ENABLE();
}

void saveConfig()
{
  ETS_GPIO_INTR_DISABLE();
  EEPROM.begin(4096);
  EEPROM.write(4095, 254);
  EEPROM.put(0, config);
  EEPROM.end();
  delay(50);
  ETS_GPIO_INTR_ENABLE();
}

void setupWebServer()
{
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
               { request->send(LittleFS, "/index.htm"); });

  webServer.on("/function.js", HTTP_GET, [](AsyncWebServerRequest *request)
               { request->send(LittleFS, "/function.js"); });

  webServer.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request)
               { request->send(LittleFS, "/style.css"); });

  webServer.on("/setting", HTTP_GET, [](AsyncWebServerRequest *request)
               {
        config.deviceName = request->getParam("deviceName")->value();
        config.espnowNetName = request->getParam("espnowNetName")->value();
        request->send(200);
        saveConfig(); });

  webServer.on("/config", HTTP_GET, [](AsyncWebServerRequest *request)
               {
        String configJson;
        DynamicJsonDocument json(192); // To calculate the buffer size uses https://arduinojson.org/v6/assistant.
        json["firmware"] = firmware;
        json["espnowNetName"] = config.espnowNetName;
        json["deviceName"] = config.deviceName;
        serializeJsonPretty(json, configJson);
        request->send(200, "application/json", configJson); });

  webServer.on("/restart", HTTP_GET, [](AsyncWebServerRequest *request)
               {request->send(200);
        ESP.restart(); });

  webServer.onNotFound([](AsyncWebServerRequest *request)
                       { request->send(404, "text/plain", "File Not Found"); });

  webServer.begin();
}

void sendAttributesMessage()
{
  if (!isGatewayAvailable)
    return;
  attributesMessageTimerSemaphore = false;
  uint32_t secs = millis() / 1000;
  uint32_t mins = secs / 60;
  uint32_t hours = mins / 60;
  uint32_t days = hours / 24;
  esp_now_payload_data_t outgoingData{ENDT_RF_GATEWAY, ENPT_ATTRIBUTES};
  espnow_message_t message;
  DynamicJsonDocument json(sizeof(esp_now_payload_data_t::message));
  json["Type"] = "RF gateway";
  json["MCU"] = "ESP8266";
  json["MAC"] = myNet.getNodeMac();
  json["Firmware"] = firmware;
  json["Library"] = myNet.getFirmwareVersion();
  json["Uptime"] = "Days:" + String(days) + " Hours:" + String(hours - (days * 24)) + " Mins:" + String(mins - (hours * 60));
  serializeJsonPretty(json, outgoingData.message);
  memcpy(&message.message, &outgoingData, sizeof(esp_now_payload_data_t));
  message.id = myNet.sendUnicastMessage(message.message, gatewayMAC, true);

  espnowMessage.push_back(message);
}

void sendKeepAliveMessage()
{
  if (!isGatewayAvailable)
    return;
  keepAliveMessageTimerSemaphore = false;
  esp_now_payload_data_t outgoingData{ENDT_RF_GATEWAY, ENPT_KEEP_ALIVE};
  espnow_message_t message;
  memcpy(&message.message, &outgoingData, sizeof(esp_now_payload_data_t));
  message.id = myNet.sendUnicastMessage(message.message, gatewayMAC, true);

  espnowMessage.push_back(message);
}

void sendConfigMessage()
{
  if (!isGatewayAvailable)
    return;
  esp_now_payload_data_t outgoingData{ENDT_RF_GATEWAY, ENPT_CONFIG};
  espnow_message_t message;
  DynamicJsonDocument json(sizeof(esp_now_payload_data_t::message));
  json[MCMT_DEVICE_NAME] = config.deviceName;
  json[MCMT_DEVICE_UNIT] = 1;
  json[MCMT_COMPONENT_TYPE] = HACT_BINARY_SENSOR;
  json[MCMT_DEVICE_CLASS] = HABSDC_CONNECTIVITY;
  json[MCMT_PAYLOAD_ON] = "online";
  json[MCMT_EXPIRE_AFTER] = 30;
  serializeJsonPretty(json, outgoingData.message);
  memcpy(&message.message, &outgoingData, sizeof(esp_now_payload_data_t));
  message.id = myNet.sendUnicastMessage(message.message, gatewayMAC, true);

  espnowMessage.push_back(message);
}

void sendSensorConfigMessage(uint8_t unit, uint8_t haComponentType, uint8_t rfSensorType, uint16_t rfSensorId, uint8_t haSensorDeviceClass, String valueTemplate,
                             String unitOfMeasurement, uint16_t expireAfter, String payloadOn, String payloadOff)
{
  esp_now_payload_data_t outgoingData{ENDT_RF_SENSOR, ENPT_CONFIG};
  espnow_message_t message;
  DynamicJsonDocument json(sizeof(esp_now_payload_data_t::message));
  json[MCMT_DEVICE_UNIT] = unit;
  json[MCMT_COMPONENT_TYPE] = haComponentType;
  json[MCMT_RF_SENSOR_TYPE] = rfSensorType;
  json[MCMT_RF_SENSOR_ID] = rfSensorId;
  json[MCMT_DEVICE_CLASS] = haSensorDeviceClass;
  json[MCMT_VALUE_TEMPLATE] = valueTemplate;
  if (unitOfMeasurement != "")
    json[MCMT_UNIT_OF_MEASUREMENT] = unitOfMeasurement;
  if (expireAfter)
    json[MCMT_EXPIRE_AFTER] = expireAfter;
  if (payloadOn != "")
    json[MCMT_PAYLOAD_ON] = payloadOn;
  if (payloadOff != "")
    json[MCMT_PAYLOAD_OFF] = payloadOff;
  serializeJsonPretty(json, outgoingData.message);
  memcpy(&message.message, &outgoingData, sizeof(esp_now_payload_data_t));
  message.id = myNet.sendUnicastMessage(message.message, gatewayMAC, true);

  espnowMessage.push_back(message);
}

void checkRadioDataAvailability()
{
  if (radio.available())
  {
    rf_transmitted_data_t receivedData;
    radio.read(&receivedData, sizeof(rf_transmitted_data_t));

    bool flag{false};
    for (uint16_t i{0}; i < configMessage.size(); ++i)
      if (configMessage[i] == receivedData.sensor_id)
        flag = true;

    if (!flag)
    {
      configMessage.push_back(receivedData.sensor_id);
      if (receivedData.sensor_type == RFST_BME280)
      {
        sendSensorConfigMessage(1, HACT_SENSOR, RFST_BME280, receivedData.sensor_id, HASDC_VOLTAGE, "battery", "V", 375);
        sendSensorConfigMessage(2, HACT_SENSOR, RFST_BME280, receivedData.sensor_id, HASDC_HUMIDITY, "humidity", "%", 375);
        sendSensorConfigMessage(3, HACT_SENSOR, RFST_BME280, receivedData.sensor_id, HASDC_TEMPERATURE, "temperature", "°C", 375);
        sendSensorConfigMessage(4, HACT_SENSOR, RFST_BME280, receivedData.sensor_id, HASDC_PRESSURE, "pressure", "мм", 375);
      }
      if (receivedData.sensor_type == RFST_BMP280)
      {
        sendSensorConfigMessage(1, HACT_SENSOR, RFST_BMP280, receivedData.sensor_id, HASDC_VOLTAGE, "battery", "V", 375);
        sendSensorConfigMessage(2, HACT_SENSOR, RFST_BMP280, receivedData.sensor_id, HASDC_TEMPERATURE, "temperature", "°C", 375);
        sendSensorConfigMessage(3, HACT_SENSOR, RFST_BMP280, receivedData.sensor_id, HASDC_PRESSURE, "pressure", "мм", 375);
      }
      if (receivedData.sensor_type == RFST_BME680) // Coming soon.
      {
      }
      if (receivedData.sensor_type == RFST_TOUCH_SWITCH)
        sendSensorConfigMessage(1, HACT_SENSOR, RFST_TOUCH_SWITCH, receivedData.sensor_id, HASDC_VOLTAGE, "battery", "V");
      if (receivedData.sensor_type == RFST_WATER_LEAKAGE)
      {
        sendSensorConfigMessage(1, HACT_SENSOR, RFST_WATER_LEAKAGE, receivedData.sensor_id, HASDC_VOLTAGE, "battery", "V");
        sendSensorConfigMessage(2, HACT_BINARY_SENSOR, RFST_WATER_LEAKAGE, receivedData.sensor_id, HABSDC_MOISTURE, "state", "", 4500, "ALARM", "DRY");
      }
      if (receivedData.sensor_type == RFST_PLANT_HUMIDITY) // Coming soon.
      {
      }
      if (receivedData.sensor_type == RFST_OPEN_CLOSE)
      {
        sendSensorConfigMessage(1, HACT_SENSOR, RFST_OPEN_CLOSE, receivedData.sensor_id, HASDC_VOLTAGE, "battery", "V");
        sendSensorConfigMessage(2, HACT_BINARY_SENSOR, RFST_OPEN_CLOSE, receivedData.sensor_id, HABSDC_DOOR, "state", "", 0, "OPEN", "CLOSE");
      }
    }

    esp_now_payload_data_t outgoingData{ENDT_RF_GATEWAY, ENPT_FORWARD};
    espnow_message_t message;
    DynamicJsonDocument json(sizeof(esp_now_payload_data_t::message));
    if (receivedData.sensor_type == RFST_BME280)
    {
      json["humidity"] = receivedData.value_2;
      json["temperature"] = receivedData.value_3;
      json["pressure"] = receivedData.value_4;
    }
    if (receivedData.sensor_type == RFST_BMP280)
    {
      json["temperature"] = receivedData.value_2;
      json["pressure"] = receivedData.value_3;
    }
    if (receivedData.sensor_type == RFST_BME680)
    {
      json["humidity"] = receivedData.value_2;
      json["temperature"] = receivedData.value_3;
      json["pressure"] = receivedData.value_4;
      json["quality"] = receivedData.value_5;
    }
    if (receivedData.sensor_type == RFST_WATER_LEAKAGE)
      json["state"] = receivedData.value_2 == ALARM ? "ALARM" : "DRY";
    if (receivedData.sensor_type == RFST_PLANT_HUMIDITY)
      json["humidity"] = receivedData.value_2;
    if (receivedData.sensor_type == RFST_OPEN_CLOSE)
      json["state"] = receivedData.value_2 == OPEN ? "OPEN" : "CLOSE";
    json["type"] = receivedData.sensor_type;
    json["id"] = receivedData.sensor_id;
    json["battery"] = double(receivedData.value_1) / 100;
    serializeJsonPretty(json, outgoingData.message);
    memcpy(&message.message, &outgoingData, sizeof(esp_now_payload_data_t));
    message.id = myNet.sendUnicastMessage(message.message, gatewayMAC, true);

    espnowMessage.push_back(message);
  }
}

void gatewayAvailabilityCheckTimerCallback()
{
  isGatewayAvailable = false;
  memset(&gatewayMAC, 0, 6);
  espnowMessage.clear();
  configMessage.clear();
}

void apModeHideTimerCallback()
{
  WiFi.softAP(("RF gateway " + String(ESP.getChipId(), HEX)).c_str(), "12345678", 1, 1);
  webServer.end();
}

void attributesMessageTimerCallback()
{
  attributesMessageTimerSemaphore = true;
}

void keepAliveMessageTimerCallback()
{
  keepAliveMessageTimerSemaphore = true;
}