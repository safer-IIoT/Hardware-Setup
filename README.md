<p align="center"><img src="" alt="logo"/></p>

<div align="center">
    
  ![Dependencies](https://img.shields.io/badge/dependencies-up%20to%20date-brightgreen.svg)
  [![GitHub Issues](https://img.shields.io/github/issues/safer-IIoT/Hardware-Setup.svg)](https://github.com/safer-IIoT/Hardware-Setup/issues)
  [![GitHub license](https://img.shields.io/github/license/safer-IIoT/Hardware-Setup)](https://github.com/safer-IIoT/Hardware-Setup/blob/master/LICENSE)

</div>

<p align="left">This repository hosts the ESP32 and ESP32S3 sketches (and the machine learning model) used in safer-IIoT</p>

# Hardware components

The following hardware need to be wired to complete the setup.

| Component                                   | Functionality in the setup                                                                                                                                                                                                                                                                                                                                                                                                             |
|---------------------------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| MQ-2                                        | Sense gas, vapors and/or smoke in the air and send readings to ESP32S3                                                                                                                                                                                                                                                                                                                                                                    |
| ESP32S3 (QFN56) (revision v0.2)                                    | Read sensor readings from MQ-2 every 1 sec and aggregate data into a C++ double array every 15 seconds. Then send that to ESP32 over UART. Arrays aggregated per 1 minute (4 * 15 sec arrays) will be sent to the Firebase database. Furthermore, detect gas, vapors and/or smoke from data in these 15 second arrays every 15 second using the machine learning model. If a gas, vapor and/or smoke is detected, a LED will be lit in the breadboard to display that it was detected. |
| ESP32-D0WD-V3 (revision v3.1)                                       | Read sensor readings from ESP32S3 over UART. Arrays read will be sent to Azure IoT Hub every 15 seconds. If the cloud machine learning model detects and responds then a LED will be lit in the breadboard to display that it was detected.                                                                                                                                                                                                                                                                                                                                                                                                    |
| 2 x 10 kΩ resistors                         | Create the voltage divider between ESP32S3, which needs 3.3V, and MQ-2, which needs 5V                                                                                                                                                                                                                                                                                                                                                |
| LED                                         | Light if a gas, vapor and/or smoke is detected from ESP32S3                                                                                                                                                                                                                                                                                                                                                                            |
| LED                                         | Light if a gas, vapor and/or smoke is detected from ESP32                                                                                                                                                                                                                                                                                                                                                                              |
| 2 x 10 kΩ resistors                         | Limit the current used in each LED                                                                                                                                                                                                                                                                                                                                                                                                     |
| Arduino Uno R3                                 | Power MQ-2. If necessary, an alternative to provide the power can be used.                                                                                                                                                                                                                                                                                                                                                             |
| 11 Male-Female and 3 Male-Male jumper wires | Wire above hardware components with the breadboard and the Arduino Uno                                                                                                                                                                                                                                                                                                                                                                     |
| Breadboard                                  | Wire all hardware components                                                                                                                                                                                                                                                                                                                                                                                                           |

# Cloud services setup

## Firebase database

The Firebase database will store data received from ESP32S3. These will be 15 second MQ-2 sensor data that were collected per every 1 minute, i.e. in every 1 minute 60 sensor data will be saved. The database will be used by the Android application to read these data when the link between it and Azure services is unavailable.

The following needs to be followed to configure it:

1. Go to the [Firebase Console](https://console.firebase.google.com/) and create a new project. Name it ```Safer```.
2. Navigate to the Realtime Database and click Create Database. Select Start in locked mode and Enable.
3. Set read/write rules to true
4. In Project Overview menu of the console, select the General tab and select Add app. Then select Web and give a name and create.
5. Copy your Database URL (Host Name). Under the General tab, scroll down to the Your apps section. Copy the exact string found in the ```apiKey``` field of your Web App configuration. Save those under the ```DATABASE_URL``` variable and the ```API_KEY``` variable respectively in [secretconfig.h](/ESP32FirebaseSetup/secretconfig.h).
6. In Authentication menu of the console, select Sign-in method tab and select Add New Provider. In it, add Email/Password provider to authenticate and make it enabled. 
7. Then select Users tab and select Add User. Create a user with an email and a password. Save them under the ```EMAIL``` variable and the ```PASSWORD``` variable respectively in [secretconfig.h](/ESP32FirebaseSetup/secretconfig.h).

### Sample data

The JSON model of the database is:

```json
{
  "data": {
    "esp32-01": {  // name of the node    
      "20261030200011": { // timestamp
        "values": [ // double array containing data of the last 1 minute
          0.934880722,
          0.419084462,
          1.38620245,
          0.999355255,
          3.90070922,
          2.965828498,
          1.966473243,
          2.482269504,
          2.321083172,
          2.482269504,
          2.836879433,
          2.836879433,
          5.351386202,
          6.511927789,
          6.93101225,
          6.834300451,
          5.512572534,
          5.641521599,
          6.447453256,
          12.121212121,
          15.022566086,
          10.186976144,
          3.578336557,
          5.770470664,
          4.867827208,
          4.577691812,
          2.933591231,
          0.87040619,
          2.288845906,
          3.707285622,
          5.963894262,
          6.511927789,
          5.899419729,
          8.478401032,
          8.961960026,
          10.735009671,
          9.70341715,
          3.384912959,
          4.545454545,
          2.836879433,
          3.578336557,
          5.061250806,
          4.996776273,
          4.771115409,
          4.384268214,
          1.418439716,
          1.289490651,
          2.192134107,
          2.804642166,
          3.352675693,
          3.449387492,
          7.317859446,
          8.865248227,
          6.189555126,
          8.446163765,
          7.575757576,
          6.028368794,
          5.415860735,
          9.929078014,
          6.608639587
        ]
      }
    }
  }
}
```
