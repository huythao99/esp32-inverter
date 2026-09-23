#include <WiFi.h>
#include "ESPAsyncWebServer.h"
#include "EEPROM.h"
#include "time.h"
#include "esp_sntp.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
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

// ---------------------------------------------------------------------------
// STM32 UART diagnostics
//
// Tells line/electrical problems apart from ESP32-side losses:
//  - hardware errors reported by the UART driver (onReceiveError):
//      FRAME / PARITY / BREAK  -> bit errors on the wire (noise, bad GND,
//                                 baud mismatch, floating RX)
//      FIFO_OVF / BUFFER_FULL  -> the ESP32 did not read fast enough
//  - frames rejected by the parser (wrong field count, non-numeric field,
//    non-printable byte, over-long line).
// Counters are reported as deltas every kUartReportMs via trackLog
// ("UART_STATS") together with one escaped sample of a rejected frame
// ("STM32_BAD_FRAME"). Only sent when something went wrong.
// ---------------------------------------------------------------------------
enum UartCounter : uint8_t {
  UC_OK = 0,        // valid frames accepted
  UC_HW_FRAME,      // UART framing error (stop bit not found)
  UC_HW_PARITY,     // parity error (8N1 -> should stay 0)
  UC_HW_BREAK,      // line held low (disconnected / reset STM32)
  UC_HW_FIFO_OVF,   // hardware FIFO overflow
  UC_HW_BUF_FULL,   // RX ring buffer full (loop() blocked too long)
  UC_DROP_HWERR,    // frames dropped because a HW error hit them
  UC_BAD_FIELDS,    // field count not 10/12
  UC_BAD_NUMBER,    // a field is not a plain number
  UC_BAD_CHAR,      // frame contained a non-printable byte
  UC_TOO_LONG,      // no terminator within 256 bytes
  UC_COUNT
};
static volatile uint32_t s_uartCounters[UC_COUNT] = {0};
static portMUX_TYPE     s_uartMux = portMUX_INITIALIZER_UNLOCKED;
// Set by the UART error callback: the frame being assembled is corrupt.
static volatile bool    s_uartErrorInFrame = false;
static String           s_lastBadFrame;        // escaped sample, Core 1 only
static unsigned long    s_lastUartReport = 0;
static const unsigned long kUartReportMs = 300000;   // 5 min

static inline void uartCount(UartCounter c) {
  portENTER_CRITICAL(&s_uartMux);
  s_uartCounters[c]++;
  portEXIT_CRITICAL(&s_uartMux);
}

// Runs on the UART event task (not Core 1's loop) — keep it tiny.
static void onStmUartError(hardwareSerial_error_t err) {
  switch (err) {
    case UART_FRAME_ERROR:       uartCount(UC_HW_FRAME);    break;
    case UART_PARITY_ERROR:      uartCount(UC_HW_PARITY);   break;
    case UART_BREAK_ERROR:       uartCount(UC_HW_BREAK);    break;
    case UART_FIFO_OVF_ERROR:    uartCount(UC_HW_FIFO_OVF); break;
    case UART_BUFFER_FULL_ERROR: uartCount(UC_HW_BUF_FULL); break;
    default: return;
  }
  s_uartErrorInFrame = true;
}

// Printable copy of a frame for the log: non-printable bytes as \xNN,
// truncated so it fits the 100-byte trackLog message.
static String escapeFrame(const String& in) {
  String out;
  out.reserve(80);
  for (unsigned int i = 0; i < in.length() && out.length() < 72; i++) {
    uint8_t b = (uint8_t)in[i];
    if (b >= 0x20 && b <= 0x7E) {
      out += (char)b;
    } else {
      char hex[5];
      snprintf(hex, sizeof(hex), "\\x%02X", b);
      out += hex;
    }
  }
  if (in.length() > 0 && out.length() >= 72) out += "...";
  return out;
}

// A field may be: optional leading '$' (first field only), optional sign,
// digits with at most one '.'.
static bool isNumericField(const String& f, bool first) {
  unsigned int i = 0;
  if (first && i < f.length() && f[i] == '$') i++;
  if (i < f.length() && (f[i] == '-' || f[i] == '+')) i++;
  bool digit = false, dot = false;
  for (; i < f.length(); i++) {
    char c = f[i];
    if (c >= '0' && c <= '9') { digit = true; continue; }
    if (c == '.' && !dot) { dot = true; continue; }
    return false;
  }
  return digit;
}

static void rejectFrame(UartCounter reason, const String& frame) {
  uartCount(reason);
  s_lastBadFrame = escapeFrame(frame);
}

// Send the counters (as deltas) when anything went wrong in the last period.
static void reportUartStats(unsigned long now) {
  if (now - s_lastUartReport < kUartReportMs) return;
  s_lastUartReport = now;

  uint32_t c[UC_COUNT];
  portENTER_CRITICAL(&s_uartMux);
  for (int i = 0; i < UC_COUNT; i++) { c[i] = s_uartCounters[i]; s_uartCounters[i] = 0; }
  portEXIT_CRITICAL(&s_uartMux);

  uint32_t problems = 0;
  for (int i = 1; i < UC_COUNT; i++) problems += c[i];
  if (problems == 0) { s_lastBadFrame = ""; return; }

  char msg[100];
  snprintf(msg, sizeof(msg),
           "ok=%lu fe=%lu pe=%lu brk=%lu ovf=%lu full=%lu drop=%lu fld=%lu num=%lu chr=%lu long=%lu",
           (unsigned long)c[UC_OK], (unsigned long)c[UC_HW_FRAME], (unsigned long)c[UC_HW_PARITY],
           (unsigned long)c[UC_HW_BREAK], (unsigned long)c[UC_HW_FIFO_OVF],
           (unsigned long)c[UC_HW_BUF_FULL], (unsigned long)c[UC_DROP_HWERR],
           (unsigned long)c[UC_BAD_FIELDS], (unsigned long)c[UC_BAD_NUMBER],
           (unsigned long)c[UC_BAD_CHAR], (unsigned long)c[UC_TOO_LONG]);
  trackLog("UART_STATS", String(msg), kUartReportMs - 1000);
  DBG_PRINT("[UART] "); DBG_PRINTLN(msg);
  if (!s_lastBadFrame.isEmpty()) {
    trackLog("STM32_BAD_FRAME", s_lastBadFrame, kUartReportMs - 1000);
    DBG_PRINT("[UART] bad frame sample: "); DBG_PRINTLN(s_lastBadFrame);
    s_lastBadFrame = "";
  }
}

// Delay between receiving cmd/restart and actually rebooting.
static const unsigned long kRestartDelayMs = 1500;

// SNTP state (Core 1 only).
static bool          ntpStarted = false;
static unsigned long ntpStartedAt = 0;
static const unsigned long kNtpRetryMs = 60000;

static void startNtp(unsigned long now) {
  sntp_set_time_sync_notification_cb(onNtpSync);
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer, ntpServer2, ntpServer3);
  ntpStarted = true;
  ntpStartedAt = now;
}

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

  testSerial.setRxBufferSize(512);   // must precede begin(); headroom for backlog
  testSerial.begin(9600, SERIAL_8N1, STM_RX, STM_TX);
  // Count hardware receive errors (framing/parity/break/overflow) so we can
  // tell wire noise from ESP32-side losses. See "STM32 UART diagnostics".
  testSerial.onReceiveError(onStmUartError);
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

  // Remote restart requested over MQTT (cmd/restart). Wait a moment so the
  // QoS 1 PUBACK actually leaves the socket (no redelivery after boot), and
  // never reboot in the middle of an OTA flash. The STM32 keeps running on its
  // last value meanwhile; applyCurrentValue() re-sends it after boot.
  if (restartPending && !otaPending && !otaInProgress &&
      currentMillis - restartAt >= kRestartDelayMs) {
    restartPending = false;
    // No "restarting" status publish: apps treat any status message as
    // "online" and would briefly show the device online while it reboots.
    mqttClient.disconnect();
    delay(200);   // let the TCP stack flush the disconnect
    ESP.restart();
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
  }

  // NTP: start SNTP once WiFi is up, then leave it running (it re-syncs by
  // itself every hour). While the clock is STILL unset, restart it every
  // kNtpRetryMs: after failed attempts lwIP backs off exponentially, and at
  // boot the first request often fails because DNS is not ready yet.
  // (Previously configTime() ran on every loop pass while MQTT kept failing,
  // which aborted each in-flight NTP request so the clock never got set.)
  if (WiFi.status() == WL_CONNECTED &&
      (!ntpStarted || (!isTimeValid() && currentMillis - ntpStartedAt >= kNtpRetryMs))) {
    startNtp(currentMillis);
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

  // Report why the device (re)booted, once it is online, so a remote restart
  // (or a crash / watchdog reset) is visible in the backend error log.
  static bool bootReasonLogged = false;
  if (!bootReasonLogged && isMqttConnected) {
    bootReasonLogged = true;
    trackLog("BOOT", "reset_reason=" + String((int)esp_reset_reason()) +
                     " fw=" + currentFirmwareVersion);
  }

  // Log NTP sync from loop() (safe) rather than the SNTP callback.
  static bool ntpSyncLogged = false;
  if (isNtpSynced && !ntpSyncLogged) {
    ntpSyncLogged = true;
    trackLog("NTP_SYNCED", "Time synchronized successfully");
  }

  // Clock set by the HTTP Date fallback while NTP is still unreachable: report
  // once so the backend can see which devices have UDP 123 blocked.
  static bool httpTimeLogged = false;
  if (!isNtpSynced && !httpTimeLogged && isTimeValid()) {
    httpTimeLogged = true;
    trackLog("TIME_FROM_HTTP", "Clock set from HTTP Date header (NTP not reachable yet)");
  }

#if DEBUG
  // NTP diagnostic on USB serial (every 5s): sync flag, SNTP status, epoch, time.
  // Only in DEBUG builds: sntp_get_sync_status() consumes the COMPLETED state.
  static unsigned long previousMillisNtpDbg = 0;
  if (currentMillis - previousMillisNtpDbg >= 5000) {
    previousMillisNtpDbg = currentMillis;
    sntp_sync_status_t st = sntp_get_sync_status();
    const char* stStr = (st == SNTP_SYNC_STATUS_COMPLETED)   ? "COMPLETED"
                      : (st == SNTP_SYNC_STATUS_IN_PROGRESS) ? "IN_PROGRESS"
                                                             : "RESET";
    time_t nowEpoch = time(nullptr);
    struct tm ti;
    char buf[20] = "----";
    if (getLocalTime(&ti, 10)) strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
    DBG_PRINT("[NTP] synced=");  DBG_PRINT(isNtpSynced ? "1" : "0");
    DBG_PRINT(" status=");       DBG_PRINT(stStr);
    DBG_PRINT(" epoch=");        DBG_PRINT((long)nowEpoch);
    DBG_PRINT(" time=");         DBG_PRINTLN(buf);
    if (!isNtpSynced && nowEpoch < 1600000000) {
      DBG_PRINTLN("[NTP] ERROR: clock not set - NTP not reached (check server/DNS/UDP123)");
    }
  }
#endif

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

  // Drain the STM32 UART every loop so the RX buffer never backs up (works at
  // any STM32 send cadence). Bytes accumulate across iterations until a
  // terminator ('*', NUL, newline); the partial tail survives between loops, so
  // a read landing mid-transmission never loses a frame's head. Each completed
  // frame is validated (10 or 12 '#'-separated fields) and the newest valid one
  // is kept for the next publish tick. No flush needed — the drain IS the flush.
  static String latestStmFrame = "";   // most recent valid frame, awaiting publish
  {
    static String rxAccum = "";
    static bool   skipToTerminator = false;   // after an over-long line
    static bool   frameHasBadChar = false;
    while (testSerial.available()) {
      char c = (char)testSerial.read();
      if (c == '*' || c == '\0' || c == '\n' || c == '\r') {
        // A HW receive error (bit error / overflow) happened while this frame
        // was on the wire: its content can't be trusted.
        bool hwError = s_uartErrorInFrame;
        s_uartErrorInFrame = false;
        rxAccum.trim();
        if (skipToTerminator) {
          skipToTerminator = false;         // end of the over-long garbage
        } else if (!rxAccum.isEmpty()) {
          // Split into '#'-separated fields and validate each one.
          int startIndex = 0;
          int tokenCount = 0;
          bool allNumeric = true;
          while (startIndex < (int)rxAccum.length()) {
            int endIndex = rxAccum.indexOf('#', startIndex);
            if (endIndex == -1) endIndex = rxAccum.length();
            if (allNumeric &&
                !isNumericField(rxAccum.substring(startIndex, endIndex), tokenCount == 0)) {
              allNumeric = false;
            }
            tokenCount++;
            startIndex = endIndex + 1;
          }
          // Raw dump: exact frame content, field count, len, and the terminator
          // byte (hex) that ended it — use this to spot stray '#'/garbage fields.
          DBG_PRINT("[STM32] frame=[");  DBG_PRINT(rxAccum);
          DBG_PRINT("] tokens=");        DBG_PRINT(tokenCount);
          DBG_PRINT(" len=");            DBG_PRINT(rxAccum.length());
          DBG_PRINT(" term=0x");         DBG_PRINTLN((int)(uint8_t)c, HEX);

          if (hwError) {
            rejectFrame(UC_DROP_HWERR, rxAccum);
          } else if (frameHasBadChar) {
            rejectFrame(UC_BAD_CHAR, rxAccum);
          } else if (tokenCount != 10 && tokenCount != 12) {
            rejectFrame(UC_BAD_FIELDS, rxAccum);
          } else {
            // Non-numeric fields are only COUNTED for now (diagnostics); the
            // frame is still forwarded as before.
            if (!allNumeric) rejectFrame(UC_BAD_NUMBER, rxAccum);
            else uartCount(UC_OK);
            latestStmFrame = rxAccum;   // keep as newest
          }
        }
        rxAccum = "";
        frameHasBadChar = false;
      } else if (!skipToTerminator) {
        uint8_t b = (uint8_t)c;
        if (b < 0x20 || b > 0x7E) frameHasBadChar = true;
        rxAccum += c;
        if (rxAccum.length() > 256) {
          // Terminator never arrived: drop this line AND everything up to the
          // next terminator (its tail would otherwise look like a new frame).
          rejectFrame(UC_TOO_LONG, rxAccum);
          rxAccum = "";
          frameHasBadChar = false;
          skipToTerminator = true;
        }
      }
    }
  }
  reportUartStats(currentMillis);

  // Publish the newest valid STM32 frame over MQTT (rate-limited to every 3s).
  if (currentMillis - previousMillis >= interval && isMqttConnected) {
    previousMillis = currentMillis;
    if (!latestStmFrame.isEmpty()) {
      String jsonString = "{\"value\":\"" + latestStmFrame + "\"}";
      if (!MQTT_TOPIC_DATA.isEmpty()) {
        String signedJsonString = createSignedMessage(jsonString);
        bool dataPublished = mqttClient.publish(MQTT_TOPIC_DATA.c_str(), signedJsonString.c_str());
        DBG_PRINT("[STM32] publish ");
        DBG_PRINT(jsonString);
        DBG_PRINT(" -> ");
        DBG_PRINTLN(dataPublished ? "SUCCESS" : "FAILED");
      }
      latestStmFrame = "";   // clear so only fresh frames get published
    }
  }

  // WiFi connection monitoring and reconnection.
  if ((WiFi.status() != WL_CONNECTED) && (currentMillis - previousMillisWifi >= intervalWifi)) {
    WiFi.reconnect();
    previousMillisWifi = currentMillis;
    isMqttConnected = false;
    connectMqtt = 0;
  }

  // Periodic online status publish (heartbeat, every intervalMqtt).
  // ALWAYS published, even before the clock is set: apps use this message as
  // the online signal, so gating it on NTP made devices look offline whenever
  // NTP was unreachable. `updatedAt` is only included once the clock is valid;
  // `uptime` (seconds since boot) is always there.
  if (currentMillis - previousMillisMqtt >= intervalMqtt) {
    previousMillisMqtt = currentMillis;
    if (isMqttConnected && mqttClient.connected() && !MQTT_TOPIC_STATUS.isEmpty()) {
      String statusMsg = "{";
      if (isTimeValid()) {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 10)) {   // short timeout: never blocks the loop
          char timeStringBuff[32];
          strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
          statusMsg += "\"updatedAt\":\"" + String(timeStringBuff) + "\",";
        }
      }
      statusMsg += "\"status\":\"online\",\"uptime\":" + String(currentMillis / 1000) + "}";
      String signedStatusMsg = createSignedMessage(statusMsg);
      mqttClient.publish(MQTT_TOPIC_STATUS.c_str(), signedStatusMsg.c_str());
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
