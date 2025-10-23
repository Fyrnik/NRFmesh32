Device like a meshtastic but using NRF24 modules.

Pinout:
nRF24L01    ESP32
VCC    ->   3.3V
GND    ->   GND
CE     ->   GPIO4
CSN    ->   GPIO5
SCK    ->   GPIO18
MOSI   ->   GPIO23
MISO   ->   GPIO19

You can use 2G4M27D module instad of nRF24L01.

To use the device:
1. Upload the sketch to esp32
2. Connect to WiFi network (Default ssid: MeshNetwork | Default password: mesh12345)
3. Connect to 192.168.4.1 using your device
4. Set device name, ssid and password in settings
