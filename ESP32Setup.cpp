#include "BluetoothSerial.h"

BluetoothSerial ESP_BT;

uint8_t hc05_address[] = {0x98, 0xd3, 0x32, 0x20, 0x52, 0x41}; 

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  if (!ESP_BT.begin("ESP32_Master", true)) {
    Serial.println("Error: Bluetooth failed to initialize.");
    return;
  }
  
  Serial.println("ESP32 Master Active. Connecting to HC-05...");
  delay(3000);
  connectToHC05();
}

void connectToHC05() {
  while (!ESP_BT.connect(hc05_address)) {
    Serial.print("Not connected");
    delay(2000); 
  }
  Serial.println("\nConnected securely to Arduino <-> HC-05!");
}

void loop() {
  if (ESP_BT.connected() && ESP_BT.available()) {
    String incomingData = ESP_BT.readStringUntil('\n');
    incomingData.trim(); 
    Serial.println(incomingData);
  }

  if (!ESP_BT.connected()) {
    Serial.println("\n Link lost. Reconnecting...");
    delay(1500);
    connectToHC05();
  }
}
