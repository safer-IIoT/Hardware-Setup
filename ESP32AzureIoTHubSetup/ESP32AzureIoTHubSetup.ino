#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <time.h>
#include <algorithm>

#include <config.h>
#include <secretconfig.h>

const char* WIFISSID = WIFI_SSID;
const char* WIFIPASSWORD = WIFI_PASSWORD;

const char* IOTHUBHOST = IOT_HUB_HOST;
const char* DEVICEID = DEVICE_ID;
const char* DEVICEKEY = DEVICE_KEY;
const uint32_t PUBLISH_INTERVAL_MS = IOTHUB_INTERVAL;
const uint32_t SAS_TTL_S = 3600;
const long TIME_OFFSET_S = 19800;

constexpr uint32_t BAUDRATE = 115200;
constexpr size_t MAX_VALUES = 32;
constexpr size_t RX_LINE_LEN = 512;

static const char AZURE_ROOT_CA[] = AZURE_ROOT_CA_PEM;

WiFiClientSecure tls;
PubSubClient mqtt(tls);

String mqttUser, telemetryTopic;
uint32_t tokenExpiry = 0;

char   lineBuffer[RX_LINE_LEN];
size_t lineLength = 0;
bool   inFrame = false;

double   rxValues[MAX_VALUES];
size_t   rxCount    = 0;
bool     rxFresh    = false;
uint32_t validFrames   = 0;
uint32_t invalidFrames  = 0;
uint32_t lastPublishMs = 0;
bool timeSynced = false;

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool parseFrame(char* s, size_t len, double* out, size_t maxOut, size_t& count) {
  if (len && s[len - 1] == '\r') s[--len] = '\0';
  char* star = strchr(s, '*');
  if (!star || (size_t)(s + len - star) != 3) return false;

  int high = hexValue(star[1]), low = hexValue(star[2]);
  if (high < 0 || low < 0) return false;

  uint8_t cs = 0;
  for (char* p = s; p < star; p++) cs ^= (uint8_t)*p;
  if (cs != (uint8_t)((high << 4) | low)) return false;

  *star = '\0';
  count = 0;
  char* p = s;
  while (true) {
    if (count >= maxOut) return false;
    char* end;
    double d = strtod(p, &end);
    if (end == p) return false;
    out[count++] = d;
    if (*end == ',') p = end + 1;
    else if (*end == '\0') break;
    else return false;
  }
  return true;
}

void onReceiveValues(const double* v, size_t n) {
  Serial.printf("Received values(%u):", (unsigned)n);

  for (size_t i = 0; i < n; i++) Serial.printf(" %.4f", v[i]);

  Serial.printf(" | Valid=%lu Invalid=%lu\n", (unsigned long)validFrames, (unsigned long)invalidFrames);
}

void pollLink() {
  size_t maxReads = 128; 
  while (Serial1.available() && maxReads-- > 0) {
    char c = (char)Serial1.read();

    if (c == '$') { inFrame = true; lineLength = 0; continue; }
    if (!inFrame) continue;

    if (c == '\n') {
      inFrame = false;
      lineBuffer[lineLength] = '\0';
      double tmp[MAX_VALUES];
      size_t n = 0;
      if (parseFrame(lineBuffer, lineLength, tmp, MAX_VALUES, n)) {
        memcpy(rxValues, tmp, n * sizeof(double));
        rxCount = n;
        rxFresh = true;
        validFrames++;
        onReceiveValues(rxValues, rxCount);
      } else {
        invalidFrames++;
      }
      continue;
    }

    if (lineLength >= RX_LINE_LEN - 1) { inFrame = false; invalidFrames++; continue; }
    lineBuffer[lineLength++] = c;
  }
}

void waitPolling(uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    pollLink();
    delay(5);
  }
}

String encodeUrl(const String& in) {
  const char* hex = "0123456789ABCDEF";
  String out;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~')
      out += c;
    else {
      out += '%'; out += hex[(c >> 4) & 0xF]; out += hex[c & 0xF];
    }
  }
  return out;
}

bool createSasToken(uint32_t expiry, String& token) {
  uint8_t key[64]; size_t keyLen = 0;

  if (mbedtls_base64_decode(key, sizeof(key), &keyLen, (const uint8_t*)DEVICEKEY, strlen(DEVICEKEY)) != 0)
    return false;

  String resource = encodeUrl(String(IOTHUBHOST) + "/devices/" + DEVICEID);
  String toSign = resource + "\n" + String(expiry);

  uint8_t mac[32];

  if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, keyLen, (const uint8_t*)toSign.c_str(), toSign.length(), mac) != 0)
    return false;

  uint8_t sig[64]; 
  size_t sigLen = 0;

  if (mbedtls_base64_encode(sig, sizeof(sig), &sigLen, mac, sizeof(mac)) != 0)
    return false;

  token = "SharedAccessSignature sr=" + resource + "&sig=" + encodeUrl(String((const char*)sig)) + "&se="  + String(expiry);
  return true;
}

void handleWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  static uint32_t lastPrintTime = 0;
  static uint32_t lastRetryTime = 0;
  uint32_t currentTime = millis();

  if (currentTime - lastPrintTime > 3000) {
    lastPrintTime = currentTime;
    int currentStatus = WiFi.status();
    Serial.printf("Wi-Fi Status Code: %d ", currentStatus);

    if(currentStatus == 6 && (currentTime - lastRetryTime > 7000))
    {
      lastRetryTime = currentTime;
      Serial.println("[Wi-Fi] Re-issuing fresh connect request");
      WiFi.begin(WIFISSID, WIFIPASSWORD);
    }
  }
}

void handleNTP() {
  if (WiFi.status() != WL_CONNECTED) {
    timeSynced = false;
    return;
  }

  if (timeSynced) return;

  static uint32_t lastNtpCheck = 0;
  time_t now = time(nullptr);

  if (now < 1700000000) {
    if (millis() - lastNtpCheck > 5000) { 
      lastNtpCheck = millis();
      Serial.println("NTP: Time not set yet");
      configTime(0, 0, "pool.ntp.org", "://google.com"); 
    }
  } else {
    timeSynced = true;
    Serial.println("NTP: Time successfully synchronized!");
    
    char timestamp[32];
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S UTC", &timeinfo);
    Serial.printf("Current time: %s\n", timestamp);
  }
}

bool connectAzure() {
  uint32_t expiry = (uint32_t)time(nullptr) + SAS_TTL_S;
  String sas;
  if (!createSasToken(expiry, sas)) {
    Serial.println("Azure: SAS generation failed.");
    return false;
  }

  String iotHubHost = String(IOTHUBHOST);
  iotHubHost.toLowerCase();

  String deviceId = String(DEVICEID);
  deviceId.toLowerCase();

  mqttUser = iotHubHost + String("/") + deviceId + "/?api-version=2021-04-12";

  Serial.printf("Azure: connecting to %s ... ", IOTHUBHOST);
  if (!mqtt.connect(DEVICEID, mqttUser.c_str(), sas.c_str())) {
    Serial.printf("failed, state=%d\n", mqtt.state());
    return false;
  }
  
  tokenExpiry = expiry;
  Serial.println("Connected to Azure IoT Hub successfully");
  return true;
}

void ensureAzure() {
  if (!timeSynced) return;

  if (mqtt.connected() && (uint32_t)time(nullptr) + 300 >= tokenExpiry) {
    Serial.println("Azure: renewing SAS token");
    mqtt.disconnect();
  }

  if (!mqtt.connected()) {
    static uint32_t lastConnectAttempt = 0;
    if (millis() - lastConnectAttempt > 10000) {
      lastConnectAttempt = millis();
      connectAzure();
    }
  }
}

void formatTimestamp(char* out, size_t cap) {
  time_t now = time(nullptr) + TIME_OFFSET_S;
  struct tm t;
  gmtime_r(&now, &t);
  strftime(out, cap, "%Y%m%d%H%M%S", &t);
}

size_t buildPayload(char* buf, size_t cap, const double* v, size_t n) {
  char ts[15];
  formatTimestamp(ts, sizeof(ts));
  int len = snprintf(buf, cap, "{\"deviceId\":\"%s\",\"ts\":\"%s\",\"values\":[", DEVICEID, ts);
  for (size_t i = 0; i < n && len < (int)cap; i++)
    len += snprintf(buf + len, cap - len, "%s%.4f", i ? "," : "", v[i]);
  if (len < (int)cap)
    len += snprintf(buf + len, cap - len, "]}");
  return (len < (int)cap) ? (size_t)len : 0;
}

void publishValues() {
  if (rxCount == 0) {
    Serial.println("[PUB] nothing received from S3 yet - skipped");
    return;
  }
  if (!rxFresh)
    Serial.println("[PUB] no new data since last publish - sending last values");

  char payload[768];
  size_t len = buildPayload(payload, sizeof(payload), rxValues, rxCount);

  if (len == 0) {
    Serial.println("Payload too large for buffer");
    return;
  }

  bool publish = mqtt.publish(telemetryTopic.c_str(), (const uint8_t*)payload, len, false);
  Serial.printf("[%s] %s\n", publish ? "SENT" : "FAIL", payload);
  if (publish) rxFresh = false;
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial1.begin(BAUDRATE, SERIAL_8N1, 25, 26);
  Serial1.setRxBufferSize(1024);

  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFISSID, WIFIPASSWORD);

  tls.setCACert(AZURE_ROOT_CA);
  mqtt.setServer(IOTHUBHOST, 8883);
  mqtt.setBufferSize(2048); 
  mqtt.setKeepAlive(60);

  mqttUser = String(IOTHUBHOST) + "/" + DEVICEID + "/?api-version=2021-04-12";
  telemetryTopic = String("devices/") + DEVICEID + "/messages/events/$.ct=application%2Fjson&$.ce=utf-8";

  lastPublishMs = millis();
}

void loop() {
  pollLink();

  handleWiFi();

  handleNTP();

  if (WiFi.status() == WL_CONNECTED && timeSynced) {
     ensureAzure();
     mqtt.loop();
     
     if (millis() - lastPublishMs >= PUBLISH_INTERVAL_MS) {
       lastPublishMs = millis();
       publishValues();
     }
  }

  yield();
}