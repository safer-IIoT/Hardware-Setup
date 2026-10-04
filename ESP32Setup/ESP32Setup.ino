#include "BluetoothSerial.h"
#include <WiFi.h>
#include "esp_log.h"
#include "config.h"

BluetoothSerial ESP_BT;
uint8_t hc05_address[] = HC05_ADDRESS;
TaskHandle_t WiFiTaskHandle = NULL;
TaskHandle_t BluetoothTaskHandle = NULL;

volatile bool isWifiLinked = false; 
volatile bool wifiDisconnected = false;

void connectToWiFi(void * pvParameters);
void connectToHC05(void * pvParameters);
void WiFiEvent(WiFiEvent_t event);

void setup() 
{
  esp_log_level_set("*", ESP_LOG_NONE); 
  Serial.begin(115200);
  delay(1000);
  
  //Connect to Wi-Fi
  WiFi.onEvent(WiFiEvent);
  xTaskCreatePinnedToCore(connectToWiFi, "WiFiTask", 4096, NULL, 1, NULL, 0);

  //Connect to Arduino <-> HC-05
  xTaskCreatePinnedToCore(connectToHC05, "BluetoothTask", 4096, NULL, 1, NULL, 1);
}

void loop() 
{
  vTaskDelay(pdMS_TO_TICKS(1000)); 
}

void WiFiEvent(WiFiEvent_t event) {
  switch(event) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      wifiDisconnected = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      wifiDisconnected = false;
      break;
    default:
      break;
  }
}

//Checks whether Wi-Fi is connected
void connectToWiFi(void * pvParameters)
{
  Serial.println("Wi-Fi setup initialized.");
  
  WiFi.persistent(false);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_11dBm); 
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 50) {
    vTaskDelay(pdMS_TO_TICKS(500));
    attempts++;
  }

  bool wasConnected = false;

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false);    
    Serial.println("Wi-Fi connected successfully");
    wasConnected = true;
    
    isWifiLinked = true; 
  } else {
    wifiDisconnected = true;
  }

  while(1) {
    vTaskDelay(pdMS_TO_TICKS(10000));

    bool isDown = (wifiDisconnected || WiFi.status() != WL_CONNECTED);

    if (!isDown) {
      int currentRSSI = WiFi.RSSI();
      if (currentRSSI >= 0) { 
        isDown = true; 
      }
    }

    if (isDown) {
      if (wasConnected) {
        Serial.println("Wi-Fi link lost. (Bluetooth will keep running independently)...");
        wasConnected = false; 
      }
      
      WiFi.disconnect(true);
      vTaskDelay(pdMS_TO_TICKS(1000)); 
      
      WiFi.mode(WIFI_STA);
      WiFi.setTxPower(WIFI_POWER_11dBm);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      
      int retryAttempts = 0;
      while (WiFi.status() != WL_CONNECTED && retryAttempts < 30) { 
        vTaskDelay(pdMS_TO_TICKS(500));
        retryAttempts++;
      }
      
      if (WiFi.status() == WL_CONNECTED && WiFi.RSSI() < 0) {
        WiFi.setSleep(false);
        wifiDisconnected = false; 
        wasConnected = true;
        Serial.println("Wi-Fi reconnected successfully.");
      }
      
    } else {
      wasConnected = true;
      wifiDisconnected = false;
      isWifiLinked = true;
      Serial.print("[RSSI: ");
      Serial.print(WiFi.RSSI());
      Serial.println(" dBm] Wi-Fi Link stable.");
    }
  }
}

//Checks whether Arduino <-> HC-05 is connected
void connectToHC05(void * pvParameters) 
{
  Serial.println("Bluetooth setup initialized. Standing by for initial Wi-Fi link...");

  // ONE-TIME INITIAL GATEKEEPER: Safely polls the global flag
  // Uses virtually 0% CPU cycles while waiting by yielding to the RTOS scheduler
  while (!isWifiLinked) {
    vTaskDelay(pdMS_TO_TICKS(100)); 
  }
  
  Serial.println("Wi-Fi link verified. Starting Bluetooth setup.");

  // Initialize Bluetooth hardware once
  if (!ESP_BT.begin("ESP32_Master", true)) {
    Serial.println("Error: Bluetooth failed to initialize.");
    vTaskDelete(NULL); 
  }
  Serial.println("Connecting to HC-05...");

  // This loop runs forever independently of Wi-Fi state shifts
  while(1) {
    if (!ESP_BT.connected()) {
      Serial.println("Attempting to reconnect to HC-05...");
      
      if (ESP_BT.connect(hc05_address)) {
        Serial.println("Connected securely to Arduino <-> HC-05");
      } else {
        Serial.println("Not connected to Arduino <-> HC-05. Retrying...");
        vTaskDelay(pdMS_TO_TICKS(2000)); 
        continue; 
      }
    }

    if (ESP_BT.available()) {
      String incomingData = ESP_BT.readStringUntil('\n');
      incomingData.trim(); 
      Serial.println(incomingData);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}