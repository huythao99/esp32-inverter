#include <WiFi.h>
#include "ESPAsyncWebServer.h"
#include "EEPROM.h"
#include "time.h"
#include "esp_sntp.h"
#include "esp_task_wdt.h"
#include <math.h>

#include "config.h"
#include "shared_state.h"
#include "storage.h"
#include "logic.h"
#include "net_http.h"
#include "net_mqtt.h"
#include "worker.h"
#include "ota_health.h"
#include "wifi_page.h"

// Task Watchdog timeout for the loop (Core 1). Must exceed the longest legitimate
// blocking section on Core 1 (MQTT connect bounded to 8s, WiFi scan ~1.5s, AP mode
// toggle ~4s) so it only fires on a genuine hang.
static const uint32_t WDT_TIMEOUT_S = 20;

// ---------------------------------------------------------------------------
// Core 1 (Arduino loop): MQTT, STM32 serial, applyCurrentValue(), web server.
// All blocking HTTP/OTA is delegated to the Core 0 worker (see worker.h).
// ---------------------------------------------------------------------------

AsyncWebServer server(80);

const char* PARAM_INPUT_1 = "ssid";
const char* PARAM_INPUT_2 = "password";
const char* PARAM_INPUT_3 = "uid";

// loop() timers
unsigned long previousMillis = 0;
unsigned long previousMillisWifi = 0;
unsigned long previousMillisMqtt = 0;
unsigned long previousMillisSetting = 0;
unsigned long previousMillisSchedule = 0;
unsigned long previousMillisMqttReconnect = 0;
unsigned long previousMillisApply = 0;
unsigned long previousMillisUpdateVersion = 0;

const long interval = 3000;
const long intervalWifi = 60000;
const long intervalMqtt = 1000;
const long intervalMqttReconnect = 10000;
const long invertalSetting = 60000;   // slow backstop poll; real-time via cmd/settings MQTT
const long intervalSchedule = 60000;  // slow backstop poll; real-time via cmd/schedule MQTT
const long intervalApply = 1000;      // re-evaluate the effective value every 1s
const long intervalUpdateVersion = 30000;  // retry firmware-version report every 30s

long double totalA = 0;
long double totalA2 = 0;

// Setup-page / connection control (Core 1 only)
bool isStartRegisterDevice = false;
bool isStartChangeModeWifi = false;
bool isStartConnect = true;
bool isStartMqtt = false;

// WiFi scan state for the setup page.
String scannedNetworksJson = "[]";
unsigned long previousMillisScan = 0;
unsigned long lastScanRequest = 0;
bool scanRequested = false;

// Setup-page connect attempt result: -1 in progress, 0 failed, 1 success.
int wifiConnectResult = -1;
unsigned long connectAttemptStart = 0;
const long connectTimeout = 15000;

// ---- Small helpers for the web routes -------------------------------------
static String connectSuccess() { return "success"; }
static String connectError()   { return "error"; }
static wl_status_t statusWifi() { return WiFi.status(); }
static int mqttStatus()         { return connectMqtt; }

void setup() {
  Serial.begin(9600);
  DBG_PRINTLN("\n\n=== ESP32 Inverter Controller ===");
  DBG_PRINT("Firmware Version: ");
  DBG_PRINTLN(currentFirmwareVersion);

  testSerial.begin(9600, SERIAL_8N1, STM_RX, STM_TX);
  DBG_PRINT("STM32 Serial: RX=");
  DBG_PRINT(STM_RX);
  DBG_PRINT(", TX=");
  DBG_PRINTLN(STM_TX);

  WiFi.mode(WIFI_AP_STA);
  EEPROM.begin(512);

  // Load WIFI_BROADCAST_SSID from Preferences (never changes once saved).
  wifiBroadcastSSID = loadWifiBroadcastSSID();
  preferences.begin("wifi_config", true);
  if (!preferences.isKey("broadcast_ssid")) {
    preferences.end();
    wifiBroadcastSSID = WIFI_BROADCAST_SSID;
    saveWifiBroadcastSSID(wifiBroadcastSSID);
  } else {
    preferences.end();
  }
  if (wifiBroadcastSSID.isEmpty()) {
    wifiBroadcastSSID = WIFI_BROADCAST_SSID;
  }

  WiFi.softAP(wifiBroadcastSSID.c_str(), "", 6);
  DBG_PRINT("Access Point SSID: ");
  DBG_PRINTLN(wifiBroadcastSSID);

  pinMode(STM_READY, INPUT);
  pinMode(STM_START, OUTPUT);
  WiFi.persistent(true);
  WiFi.setAutoReconnect(true);
  readWifi();

  // Load last setting from storage on startup.
  lastSetupValue = loadSettingFromStorage();
  if (!lastSetupValue.isEmpty()) {
    DBG_PRINT("Loaded setting from storage: ");
    DBG_PRINTLN(lastSetupValue);
  }

  netHttpInit();

  // MQTT init
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setKeepAlive(60);
  mqttClient.setSocketTimeout(8);  // bound connectToMqtt() well under the WDT
  setupMqttCallback();
  DBG_PRINT("MQTT Server: ");
  DBG_PRINT(MQTT_SERVER);
  DBG_PRINT(":");
  DBG_PRINTLN(MQTT_PORT);

  // Create RTOS primitives + start the Core 0 worker (before loop() runs so the
  // mutex/queues exist for the very first applyCurrentValue()/enqueue).
  workerInit();

  // Task Watchdog on the loop (Core 1): a hang reboots the device so a bad OTA
  // image that freezes never reaches otaHealthTick() and the bootloader rolls back.
  // Only the loopTask is watched — the Core 0 worker runs multi-minute OTA jobs.
  esp_task_wdt_init(WDT_TIMEOUT_S, true);  // panic=true -> reboot on timeout
  esp_task_wdt_add(NULL);                  // watch the current (loop) task

  // Detect a freshly-OTA'd image still awaiting health confirmation.
  otaHealthBegin();

  // ---- Web routes ----
  server.on("/connect", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", index_html);
  });

  server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request){
    lastScanRequest = millis();
    if (request->hasParam("refresh")) {
      scanRequested = true;
    }
    request->send(200, "application/json", scannedNetworksJson);
  });

  server.on("/wifi", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam(PARAM_INPUT_1) && request->hasParam(PARAM_INPUT_2)) {
      param_ssid = request->getParam(PARAM_INPUT_1)->value();
      param_password = request->getParam(PARAM_INPUT_2)->value();
      uid = request->getParam(PARAM_INPUT_3)->value();
      isStartConnect = true;
      lastScanRequest = 0;
      WiFi.disconnect();
      request->send_P(200, "text/plain", connectSuccess().c_str());
    } else {
      request->send_P(200, "text/plain", connectError().c_str());
    }
  });

  server.on("/wifi-status", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
    }
    request->send_P(200, "text/plain", String(statusWifi()).c_str());
  });

  server.on("/mqtt-status", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/plain", String(mqttStatus()).c_str());
  });

  server.on("/connect-status", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{\"wifi\":" + String((int)WiFi.status()) +
                  ",\"mqtt\":" + String(connectMqtt) +
                  ",\"result\":" + String(wifiConnectResult) + "}";
    request->send(200, "application/json", json);
  });

  server.on("/change-mode-wifi", HTTP_GET, [](AsyncWebServerRequest *request) {
    isStartChangeModeWifi = true;
    request->send_P(200, "text/plain", connectSuccess().c_str());
  });

  server.begin();
}

void loop() {
  unsigned long currentMillis = millis();

  esp_task_wdt_reset();  // feed the watchdog each iteration

  // MQTT service + flush any OTA status the worker produced.
  mqttClient.loop();
  drainOtaStatus();

  // Sync isMqttConnected flag with the real connection state.
  if (isMqttConnected && !mqttClient.connected()) {
    isMqttConnected = false;
    connectMqtt = 0;
  }

  // Confirm a freshly-OTA'd image once it's healthy (WiFi + MQTT up).
  otaHealthTick(WiFi.status() == WL_CONNECTED && mqttClient.connected());

  // Report firmware version once we're online — throttled + delegated to Core 0.
  if (!isUpdateVersion && WiFi.status() == WL_CONNECTED && isMqttConnected) {
    if (currentMillis - previousMillisUpdateVersion >= intervalUpdateVersion) {
      previousMillisUpdateVersion = currentMillis;
      requestUpdateVersion();
    }
  }

  // OTA trigger set by the MQTT callback — hand off to Core 0.
  if (otaPending) {
    otaPending = false;
    requestOTA();
  }

  // WiFi AP mode toggle (user-initiated, rare). Kept blocking: it only runs on
  // an explicit /change-mode-wifi request.
  if (isStartChangeModeWifi) {
    WiFi.softAPdisconnect(true);
    delay(1000);
    WiFi.mode(WIFI_STA);
    delay(3000);
    WiFi.softAP(wifiBroadcastSSID.c_str(), "12345678", 6);
    WiFi.mode(WIFI_AP_STA);
    isStartChangeModeWifi = false;
  }

  // Non-blocking WiFi-reset completion (kicked off by startWiFiReset()).
  if (wifiResetPending && currentMillis - wifiResetAt >= 1000) {
    wifiResetPending = false;
    WiFi.begin(param_ssid.c_str(), param_password.c_str());
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  }

  // Start a setup-page connection attempt.
  if (isStartConnect && param_ssid.isEmpty() == false) {
    scanRequested = false;
    WiFi.disconnect();
    // One clean attempt (no auto-reconnect) keeps the softAP stable on a wrong password.
    WiFi.setAutoReconnect(false);
    WiFi.begin(param_ssid.c_str(), param_password.c_str());
    WiFi.mode(WIFI_AP_STA);
    isStartConnect = false;
    isStartMqtt = true;
    wifiConnectResult = -1;
    connectAttemptStart = currentMillis;
  }

  // Resolve the setup-page connect attempt (success, or failure/timeout).
  if (connectAttemptStart != 0 && wifiConnectResult == -1) {
    wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
      wifiConnectResult = 1;
      connectAttemptStart = 0;
      WiFi.setAutoReconnect(true);
    } else if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL ||
               currentMillis - connectAttemptStart >= connectTimeout) {
      wifiConnectResult = 0;
      connectAttemptStart = 0;
      isStartMqtt = false;
      WiFi.disconnect();
    }
  }

  // Connect MQTT once WiFi is up.
  if (WiFi.status() == WL_CONNECTED && isStartMqtt) {
    sntp_set_time_sync_notification_cb(onNtpSync);
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    writeInfo(param_ssid, param_password, uid);

    if (connectToMqtt()) {
      isMqttConnected = true;
      isStartMqtt = false;
      // Fetch immediately (via Core 0) instead of waiting for the 60s timer.
      requestFetchSettings();
      requestFetchSchedule();
      previousMillisSetting = currentMillis;
      previousMillisSchedule = currentMillis;
      previousMillisUpdateVersion = currentMillis;
    }
    isStartRegisterDevice = true;
  }

  // Register the device (delegated to Core 0), then report the firmware version.
  // Order matters on first-ever add: registerDevice() (POST) creates the device,
  // so it must run before updateFirmwareVersion() (PATCH .../firmware), otherwise
  // the PATCH 404s on a device that doesn't exist yet. The worker is FIFO, so
  // enqueuing register first guarantees that order.
  if (isStartRegisterDevice && WiFi.status() == WL_CONNECTED) {
    if (!getUid().isEmpty()) {
      requestRegister();
      requestUpdateVersion();
    }
    isStartRegisterDevice = false;
  }

  // Slow backstop polls.
  if (currentMillis - previousMillisSetting >= invertalSetting) {
    previousMillisSetting = currentMillis;
    requestFetchSettings();
  }
  if (currentMillis - previousMillisSchedule >= intervalSchedule) {
    previousMillisSchedule = currentMillis;
    requestFetchSchedule();
  }

  // Debounced MQTT command syncs (server asked us to re-fetch on change).
  if (cmdSettingsPending && (currentMillis - cmdSettingsAt >= cmdDebounce)) {
    cmdSettingsPending = false;
    DBG_PRINTLN("[CMD] settings sync requested -> fetching setting");
    requestFetchSettings();
  }
  if (cmdSchedulePending && (currentMillis - cmdScheduleAt >= cmdDebounce)) {
    cmdSchedulePending = false;
    DBG_PRINTLN("[CMD] schedule sync requested -> fetching schedule");
    requestFetchSchedule();
  }

  // Share value pushed by the server. Uses its own small debounce so it tracks a
  // fast push cadence (e.g. 1s) without the 500ms command debounce delaying/starving it.
  if (sharePending && (currentMillis - shareAt >= shareDebounceMs)) {
    sharePending = false;
    activeShareValue = shareValue;
    activeShareAt = currentMillis;
    // Apply immediately (event-driven) instead of waiting for the periodic tick,
    // then realign the timer. applyCurrentValue() still runs periodically below to
    // handle share expiry + the STM32 keepalive, so the single-writer model holds.
    applyCurrentValue();
    previousMillisApply = currentMillis;
  }

  // Log NTP sync from loop() (safe) rather than the SNTP callback.
  static bool ntpSyncLogged = false;
  if (isNtpSynced && !ntpSyncLogged) {
    ntpSyncLogged = true;
    trackLog("NTP_SYNCED", "Time synchronized successfully");
  }

  // Single writer to the STM32: share > schedule > setting, every 1s.
  if (currentMillis - previousMillisApply >= intervalApply) {
    previousMillisApply = currentMillis;
    applyCurrentValue();
  }

  // On-demand WiFi scan for the setup page (briefly suspends the softAP).
  if (scanRequested && (previousMillisScan == 0 || currentMillis - previousMillisScan >= 3000)) {
    scanRequested = false;
    previousMillisScan = currentMillis;

    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect(false);
      delay(50);
    }

    WiFi.scanDelete();
    int n = WiFi.scanNetworks(false /*async*/, true /*show_hidden*/, false /*passive*/, 120 /*ms per channel*/);
    DBG_PRINT("WiFi scan found ");
    DBG_PRINT(n);
    DBG_PRINTLN(" networks");

    String json = "[";
    for (int i = 0; i < n; i++) {
      if (i) json += ",";
      json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",";
      json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
      json += "\"secure\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? 0 : 1) + "}";
    }
    json += "]";
    scannedNetworksJson = json;
    WiFi.scanDelete();
  }

  // Read STM32 data + publish over MQTT (every 3s).
  if (currentMillis - previousMillis >= interval && isMqttConnected) {
    previousMillis = currentMillis;

    DBG_PRINTLN("=== Reading STM32 Data ===");
    int available = testSerial.available();
    DBG_PRINT("Bytes available: ");
    DBG_PRINTLN(available);

    String res = testSerial.readString();
    res.trim();

    DBG_PRINT("Raw data: [");
    DBG_PRINT(res);
    DBG_PRINTLN("]");
    DBG_PRINT("Length: ");
    DBG_PRINTLN(res.length());

    if (res.isEmpty()) {
      DBG_PRINTLN("No data from STM32");
      DBG_PRINTLN("=======================");
    } else {
      int indexOf = res.indexOf("*");
      DBG_PRINT("Index of '*': ");
      DBG_PRINTLN(indexOf);

      res = res.substring(0, indexOf);
      DBG_PRINT("Data after trim: ");
      DBG_PRINTLN(res);

      long double pAfter = 0;
      long double p2After = 0;

      int startIndex = 0;
      int tokenCount = 0;
      while (startIndex < (int)res.length()) {
        int endIndex = res.indexOf('#', startIndex);
        if (endIndex == -1) endIndex = res.length();

        tokenCount++;
        String token = res.substring(startIndex, endIndex);

        if (tokenCount == 9) {
          pAfter = fabs(token.toDouble());
        } else if (tokenCount == 10) {
          p2After = fabs(token.toDouble());
          break;
        }
        startIndex = endIndex + 1;
      }

      totalA = totalA + pAfter / 1000000.0;
      totalA2 = totalA2 + p2After / 1000000.0;

      String jsonString = "{\"value\":\"" + res + "\",\"totalA2Capacity\":\"" + String((double)totalA2) + "\",\"totalACapacity\":\"" + String((double)totalA) + "\"}";

      DBG_PRINTLN("=== Publishing to MQTT ===");
      DBG_PRINT("Topic: ");
      DBG_PRINTLN(MQTT_TOPIC_DATA);
      DBG_PRINT("Payload: ");
      DBG_PRINTLN(jsonString);

      if (!MQTT_TOPIC_DATA.isEmpty()) {
        String signedJsonString = createSignedMessage(jsonString);
        bool dataPublished = mqttClient.publish(MQTT_TOPIC_DATA.c_str(), signedJsonString.c_str());
        DBG_PRINT("Publish result: ");
        DBG_PRINTLN(dataPublished ? "SUCCESS" : "FAILED");
      } else {
        DBG_PRINTLN("MQTT_TOPIC_DATA is empty!");
      }
      DBG_PRINTLN("=======================");
    }
  }

  // WiFi connection monitoring and reconnection.
  if ((WiFi.status() != WL_CONNECTED) && (currentMillis - previousMillisWifi >= intervalWifi)) {
    WiFi.reconnect();
    previousMillisWifi = currentMillis;
    isMqttConnected = false;
    connectMqtt = 0;
  }

  // Periodic online status publish.
  if (currentMillis - previousMillisMqtt >= intervalMqtt) {
    previousMillisMqtt = currentMillis;
    if (isMqttConnected && mqttClient.connected()) {
      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 10)) {   // short timeout: never blocks the loop
        char timeStringBuff[50];
        strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
        if (!MQTT_TOPIC_STATUS.isEmpty()) {
          String statusMsg = "{\"updatedAt\":\"" + String(timeStringBuff) + "\",\"status\":\"online\"}";
          String signedStatusMsg = createSignedMessage(statusMsg);
          mqttClient.publish(MQTT_TOPIC_STATUS.c_str(), signedStatusMsg.c_str());
        }
      }
    }
  }

  // Maintain the MQTT connection (WiFi up but MQTT down).
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected() && (currentMillis - previousMillisMqttReconnect >= intervalMqttReconnect)) {
      DBG_PRINTLN("=== MQTT Reconnection Required ===");
      DBG_PRINT("MQTT State: ");
      DBG_PRINTLN(mqttClient.state());
      DBG_PRINT("Fail count: ");
      DBG_PRINTLN(mqttFailCount);

      // Reset WiFi (non-blocking) after too many MQTT failures to clear DNS cache.
      if (mqttFailCount >= MQTT_MAX_FAIL_BEFORE_RESET) {
        DBG_PRINTLN("Too many MQTT failures, resetting WiFi...");
        trackLog("MQTT_FAILED", "MQTT connection failed " + String(mqttFailCount) + " times, resetting WiFi");
        startWiFiReset();
        mqttFailCount = 0;
      }

      isMqttConnected = false;
      if (connectToMqtt()) {
        DBG_PRINTLN("MQTT Reconnection successful");
        // Re-fetch state after reconnect (messages sent while offline were missed).
        requestFetchSettings();
        requestFetchSchedule();
      } else {
        DBG_PRINTLN("MQTT Reconnection failed, will retry in 10s");
      }
      previousMillisMqttReconnect = currentMillis;
      DBG_PRINTLN("===================================");
    }
  } else {
    if (isMqttConnected) {
      DBG_PRINTLN("WiFi down, cannot maintain MQTT connection");
      isMqttConnected = false;
    }
  }
}
