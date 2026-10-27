#pragma once

#define ESP32_DEVICE_HOSTNAME      "esp32-edge"
#define RX_PIN                     18
#define TX_PIN                     17

// HC-05 input
#define HC05_BUILTIN_READER        0
#define HC05_BAUD                  9600
#define HC05_RX_PIN                16
#define HC05_TX_PIN                17
#define HC05_ADDRESS               {0x98, 0xd3, 0x32, 0x20, 0x52, 0x41}

// Firebase
#define NODE_ID                    "ESP32_NODE_01"

#define DEVICE_ID                  "esp32-01"
#define NTP_SERVER                 "pool.ntp.org"
#define INTERVAL                   60000