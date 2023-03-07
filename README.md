# RF gateway for ESP8266

Gateway for data exchange between nRF24 devices and ESP-NOW network.

## Features

1. After turn on (or after rebooting) creates an access point named "RF gateway XXXXXXXXXXXX" with password "12345678" (IP 192.168.4.1). Access point will be shown during 5 minutes. The rest of the time access point is a hidden.
2. Periodically transmission of system information (every 60 seconds) and availability status (every 10 seconds) to the gateway.
3. Automatically adds gateway configuration to Home Assistan via MQTT discovery as a binary_sensor.
4. Automatically adds supported nRF24 sensors configurations to Home Assistan via MQTT discovery.
5. Possibility firmware update over OTA.
6. Web interface for settings.
  
## Notes

1. ESP-NOW mesh network based on the library [ZHNetwork](https://github.com/aZholtikov/ZHNetwork).
2. Regardless of the status of connection to gateway the device perform ESP-NOW node function.
3. For show the access point for setting or firmware update, send the command "update" to the device's root topic (example - "homeassistant/espnow_rf_gateway/E8DB849CA148"). Access point will be shown during 5 minutes. Similarly, for restart send the command "restart".
4. nRF24 connection:

```text
GPIO04 - CE, GPIO15 - CSN, GPIO14 - SCK, GPIO12 - MISO, GPIO13 - MOSI.
```

## Attention

1. A gateway is required. For details see [ESP-NOW Gateway](https://github.com/aZholtikov/ESP-NOW-Gateway).
2. ESP-NOW network name must be set same of all another ESP-NOW devices in network.
3. If encryption is used, the key must be set same of all another ESP-NOW devices in network.
4. Upload the "data" folder (with web interface) into the filesystem before flashing.

## Supported devices

1. [nRF24 Climate Sensor (BME280)](https://github.com/aZholtikov/RF-Climate-Sensor-BME280)
2. [nRF24 Climate Sensor (BMP280)](https://github.com/aZholtikov/RF-Climate-Sensor-BMP280)
3. nRF24 Climate Sensor (BME680) Coming soon.
4. [nRF24 Open/Close Sensor](https://github.com/aZholtikov/RF-Open-Close-Sensor)
5. nRF24 Plant Humidity Sensor Coming soon.
6. [nRF24 Touch Switch](https://github.com/aZholtikov/RF-Touch-Switch)
7. [nRF24 Water Leakage Sensor](https://github.com/aZholtikov/RF-Water-Leakage-Sensor)

Any feedback via [e-mail](mailto:github@zh.com.ru) would be appreciated. Or... [Buy me a coffee](https://paypal.me/aZholtikov).
