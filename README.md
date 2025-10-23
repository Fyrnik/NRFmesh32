Device like a meshtastic but using NRF24 modules.

Pinout:
nRF24L01    ESP32 </br>
VCC    ->   3.3V</br>
GND    ->   GND</br>
CE     ->   GPIO4</br>
CSN    ->   GPIO5</br>
SCK    ->   GPIO18</br>
MOSI   ->   GPIO23</br>
MISO   ->   GPIO19</br>

You can use 2G4M27D module instad of nRF24L01.

To use the device:
1. Upload the sketch to esp32
2. Connect to WiFi network (Default ssid: MeshNetwork | Default password: mesh12345)
3. Connect to 192.168.4.1 using your device
4. Set device name, ssid and password in settings
