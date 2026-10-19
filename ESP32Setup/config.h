#pragma once

// Wi-Fi
#define WIFI_SSID                  "SLT-Fiber-2.4G_d1e0"
#define WIFI_PASSWORD              "code7976"
#define ESP32_DEVICE_HOSTNAME      "esp32-edge"

// HC-05 input
#define HC05_BUILTIN_READER  0
#define HC05_BAUD            9600
#define HC05_RX_PIN          16
#define HC05_TX_PIN          17
#define HC05_ADDRESS         {0x98, 0xd3, 0x32, 0x20, 0x52, 0x41}

// Firebase
#define NODE_ID "ESP32_NODE_01"
#define DATABASE_URL "https://safer-6c654-default-rtdb.firebaseio.com"
#define API_KEY "AIzaSyA61uaW0SOVf08CyyH8pw3xTj88oDCpo60"
#define EMAIL "esp32@project.local"
#define PASSWORD "password123"