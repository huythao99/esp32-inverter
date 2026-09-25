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
#include "stm_fota.h"
#include "ca_certs.h"

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

const long interval = 1000;  // read STM32 frame + publish every 1s
const long intervalWifi = 60000;
const long intervalMqtt = 1000;
const long intervalMqttReconnect = 10000;
const long invertalSetting = 60000;   // slow backstop poll; real-time via cmd/settings MQTT
const long intervalSchedule = 60000;  // slow backstop poll; real-time via cmd/schedule MQTT
const long intervalApply = 1000;      // re-evaluate the effective value every 1s
const long intervalUpdateVersion = 30000;  // retry firmware-version report every 30s

// STM32 hardware-serial framing. The UART is drained byte-by-byte every loop()
// iteration so no message is ever missed; '*' terminates a frame. stmMsgBuffer
// holds the in-progress frame, stmLatestFrame the most recent complete one that
// the 1s publish tick will consume.
static String stmMsgBuffer;
static String stmLatestFrame;
static const int STM_FRAME_MAX = 512;  // overflow guard for a runaway/garbled stream

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
// Why the last STA attempt dropped (wifi_err_reason_t, 0 = none), shown by the
// setup page: 15/202/204/2 wrong password, 201 network not found, 200/203
// weak signal. 8 (ASSOC_LEAVE) is our own WiFi.disconnect() and is ignored.
static volatile uint8_t staDisconnectReason = 0;
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
//  - frames rejected by acceptFrame() (wrong field count, non-numeric field,
//    non-printable byte, over-long line). Rejected frames are NOT published.
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
  UC_DROP_HWERR,    // frames dropped: HW error / RX overflow hit them
  UC_BAD_FIELDS,    // field count not 10/12/13
  UC_BAD_NUMBER,    // a field is not a plain number (13th: not x.y.z)
  UC_BAD_CHAR,      // frame contained a non-printable byte
  UC_TOO_LONG,      // no terminator within 256 bytes
  UC_COUNT
};
static volatile uint32_t s_uartCounters[UC_COUNT] = {0};
static portMUX_TYPE     s_uartMux = portMUX_INITIALIZER_UNLOCKED;
// Set by the UART error callback: the frame being assembled is corrupt.
static volatile bool    s_uartErrorInFrame = false;
// Set on RX overflow (HW FIFO / ring buffer full): bytes were lost somewhere
// in what is buffered right now, so the whole backlog can't be trusted.
static volatile bool    s_uartOverflow = false;
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
    case UART_FIFO_OVF_ERROR:    uartCount(UC_HW_FIFO_OVF); s_uartOverflow = true; break;
    case UART_BUFFER_FULL_ERROR: uartCount(UC_HW_BUF_FULL); s_uartOverflow = true; break;
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

// Field count this STM32 sends (10 or 12), learned from the first valid frame
// after boot. A frame whose head was lost can still split into exactly 10
// fields (a 12-field frame minus its first two), so once the count is known a
// different count is rejected — unless it repeats kFieldRelearnRun times in a
// row (the STM32 firmware really changed).
// Frame layouts: 10 fields, 12 fields (odometers), or 12 + 13th field = STM32
// firmware version "x.y.z" (STM32 firmware >= 2.0.0). 12 and 13 share one
// layout for the learned count below, so an STM32 FOTA that adds the version
// field doesn't trigger a relearn.
static bool isVersionField(const String& f) {
  int dots = 0;
  bool digit = false;
  for (unsigned int i = 0; i < f.length(); i++) {
    char c = f[i];
    if (c >= '0' && c <= '9') { digit = true; continue; }
    if (c != '.' || !digit) return false;   // no leading / double dots
    dots++;
    digit = false;
  }
  return dots == 2 && digit;               // exactly x.y.z, no trailing dot
}

static int s_expectedFields = 0;       // 0 = not learned yet
static int s_otherFields = 0;
static int s_otherFieldsRun = 0;
static const int kFieldRelearnRun = 5;

// Validate a completed (trimmed) frame. Returns true when it may be published.
// Rejected frames are counted for UART_STATS and one escaped sample is kept
// for STM32_BAD_FRAME. A frame that passes here contains only digits, '.',
// '+', '-', '#' and an optional leading '$', so it is also JSON-safe.
// Accepted: 10 or 12 numeric fields, or 12 numeric + a 13th "x.y.z" version.
static bool acceptFrame(const String& frame, bool hwError) {
  if (hwError) { rejectFrame(UC_DROP_HWERR, frame); return false; }

  int fields = 0;
  bool allNumeric = true;
  bool badChar = false;
  const int len = frame.length();
  for (int i = 0; i < len; i++) {
    uint8_t b = (uint8_t)frame[i];
    if (b < 0x20 || b > 0x7E) { badChar = true; break; }
  }
  // Same field split the backend uses; a trailing '#' adds no empty field.
  // Fields 1..12 must be plain numbers; a 13th field must be a version x.y.z.
  int start = 0;
  bool versionOk = true;
  while (start < len) {
    int end = frame.indexOf('#', start);
    if (end == -1) end = len;
    String f = frame.substring(start, end);
    if (fields < 12) {
      if (allNumeric && !isNumericField(f, fields == 0)) allNumeric = false;
    } else if (fields == 12) {
      versionOk = isVersionField(f);
    }
    fields++;
    start = end + 1;
  }

  if (badChar)                                  { rejectFrame(UC_BAD_CHAR, frame);   return false; }
  if (fields != 10 && fields != 12 && fields != 13) { rejectFrame(UC_BAD_FIELDS, frame); return false; }
  if (!allNumeric || !versionOk)                { rejectFrame(UC_BAD_NUMBER, frame); return false; }

  // 12 and 13 fields are the same layout (13 = 12 + version).
  const int layout = (fields == 10) ? 10 : 12;
  if (s_expectedFields == 0 || layout == s_expectedFields) {
    s_expectedFields = layout;
    s_otherFieldsRun = 0;
  } else {
    s_otherFieldsRun = (layout == s_otherFields) ? s_otherFieldsRun + 1 : 1;
    s_otherFields = layout;
    if (s_otherFieldsRun < kFieldRelearnRun) {
      rejectFrame(UC_BAD_FIELDS, frame);
      return false;
    }
    s_expectedFields = layout;           // consistent new format: adopt it
    s_otherFieldsRun = 0;
  }
  uartCount(UC_OK);
  return true;
}

// Remember the STM32 firmware version from a 13-field frame (last field x.y.z).
static void recordStmVersion(const String& frame) {
  int end = frame.length();
  if (end > 0 && frame[end - 1] == '#') end--;          // trailing '#'
  const int start = frame.lastIndexOf('#', end - 1) + 1;
  if (start <= 0) return;
  const String last = frame.substring(start, end);
  if (isVersionField(last)) stmSetReportedVersion(last);
}

// ---------------------------------------------------------------------------
// Memory diagnostics: heap + task stack high-water marks, sent every
// kMemReportMs as "STACK_STATS" (and once ~2 min after boot). Used to size the
// task stacks safely (async_tcp via CONFIG_ASYNC_TCP_STACK_SIZE, http_worker in
// workerInit()). Values: bytes; stack = minimum free stack ever seen (-1 if the
// task doesn't exist yet).
// ---------------------------------------------------------------------------
static unsigned long s_lastMemReport = 0;
static bool          s_firstMemReportDone = false;
static const unsigned long kMemReportMs = 1800000;     // 30 min
static const unsigned long kFirstMemReportMs = 120000; // 2 min after boot

static long stackFreeBytes(const char* taskName) {
  TaskHandle_t h = xTaskGetHandle(taskName);
  if (h == nullptr) return -1;
  return (long)uxTaskGetStackHighWaterMark(h);   // ESP-IDF: in bytes
}

static void reportMemStats(unsigned long now) {
  if (!s_firstMemReportDone) {
    if (now < kFirstMemReportMs) return;
    s_firstMemReportDone = true;
  } else if (now - s_lastMemReport < kMemReportMs) {
    return;
  }
  s_lastMemReport = now;

  char msg[100];
  snprintf(msg, sizeof(msg),
           "heap=%u min=%u blk=%u loop=%ld worker=%ld async=%ld",
           (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
           (unsigned)ESP.getMaxAllocHeap(),
           (long)uxTaskGetStackHighWaterMark(nullptr),   // this (loop) task
           stackFreeBytes("http_worker"), stackFreeBytes("async_tcp"));
  trackLog("STACK_STATS", String(msg), kMemReportMs - 1000);
  DBG_PRINT("[MEM] "); DBG_PRINTLN(msg);
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

  // Hardware UART2 for the STM32 link. Enlarge the RX FIFO/ring buffer so bytes
  // are never dropped even if loop() is briefly busy between drains.
  // Must precede begin(). At 1 frame/s (~55 B) this holds ~9s of frames
  // (~0.5s if the line were saturated at 960 B/s). Only a hanging MQTT connect
  // (up to ~11s, while nothing can be published anyway) can overflow it, and
  // the overflow handling in loop() then drops the backlog and resyncs, so no
  // glued/corrupt frame is ever published. Kept small to save RAM.
  testSerial.setRxBufferSize(512);
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

  // Last known schedule from NVS: runs until the first successful fetch, so a
  // reboot while the server is unreachable doesn't drop the schedule.
  // (Before workerInit(): no other task touches schedules[] yet.)
  {
    String storedSchedule = loadScheduleFromStorage();
    if (!storedSchedule.isEmpty()) {
      parseScheduleData(storedSchedule);
      DBG_PRINT("Loaded schedule from storage, count=");
      DBG_PRINTLN(scheduleCount);
    }
  }

  netHttpInit();

  // MQTT init
  // Server/port + transport (TLS 8883 or plain 1883) are chosen on every
  // connect attempt in connectToMqtt().
  mqttTlsClient.setCACert(kRootCA);
  mqttTlsClient.setHandshakeTimeout(8);   // s; keeps connect well under the WDT
  mqttClient.setKeepAlive(60);
  mqttClient.setSocketTimeout(8);  // bound connectToMqtt() well under the WDT
  // PubSubClient drops any packet (topic + payload) larger than its buffer
  // WITHOUT calling the callback. The default 256 B was too small for the
  // old firmware/update payload (uid + deviceId + timestamps ~258 B), so OTA
  // commands were silently lost. 512 B leaves room for every topic we use.
  mqttClient.setBufferSize(512);
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

  // Remember why a setup connect attempt failed (reported by /connect-status).
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    uint8_t r = info.wifi_sta_disconnected.reason;
    if (r != WIFI_REASON_ASSOC_LEAVE) staDisconnectReason = r;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  // ---- Web routes ----
  server.on("/connect", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", index_html);   // PROGMEM page, sent from flash
  });

  server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request){
    lastScanRequest = millis();
    if (request->hasParam("refresh")) {
      scanRequested = true;
    }
    request->send(200, "application/json", scannedNetworksJson);
  });

  server.on("/wifi", HTTP_POST, [](AsyncWebServerRequest *request){
    // All three are required: getParam() returns nullptr for a missing one and
    // dereferencing it crashed the device.
    if (request->hasParam(PARAM_INPUT_1) && request->hasParam(PARAM_INPUT_2) &&
        request->hasParam(PARAM_INPUT_3)) {
      param_ssid = request->getParam(PARAM_INPUT_1)->value();
      param_password = request->getParam(PARAM_INPUT_2)->value();
      uid = request->getParam(PARAM_INPUT_3)->value();
      isStartConnect = true;
      // Reset the reported state NOW (not in loop()): the page polls
      // /connect-status right after this reply and must not see the previous
      // attempt's result.
      wifiConnectResult = -1;
      connectMqtt = -1;
      staDisconnectReason = 0;
      lastScanRequest = 0;
      WiFi.disconnect();
      request->send(200, "text/plain", connectSuccess());
    } else {
      request->send(200, "text/plain", connectError());
    }
  });

  server.on("/wifi-status", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
    }
    request->send(200, "text/plain", String(statusWifi()));
  });

  server.on("/mqtt-status", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", String(mqttStatus()));
  });

  server.on("/connect-status", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{\"wifi\":" + String((int)WiFi.status()) +
                  ",\"mqtt\":" + String(connectMqtt) +
                  ",\"result\":" + String(wifiConnectResult) +
                  // busy: a connect attempt is queued / running.
                  ",\"busy\":" + String(((isStartConnect && !param_ssid.isEmpty()) ||
                                          connectAttemptStart != 0) ? 1 : 0) +
                  ",\"reason\":" + String((int)staDisconnectReason) +
                  ",\"ssid\":\"" + jsonEscape(param_ssid) + "\"" + "}";
    request->send(200, "application/json", json);
  });

  server.on("/change-mode-wifi", HTTP_GET, [](AsyncWebServerRequest *request) {
    isStartChangeModeWifi = true;
    request->send(200, "text/plain", connectSuccess());
  });

  server.begin();
}

void loop() {
  unsigned long currentMillis = millis();

  esp_task_wdt_reset();  // feed the watchdog each iteration

  // Drain the STM32 UART every iteration, char by char. A frame ends at '*',
  // a null, or a newline ('\n'/'\r'); only VALID frames (acceptFrame) are
  // stashed in stmLatestFrame for the publish tick below, so two glued or
  // truncated frames are never published. (Kept print-free — hot path.)
  //
  // stmSkipToTerminator drops everything up to the next terminator. It starts
  // true because the first bytes after boot are usually the middle of a frame.
  static bool stmSkipToTerminator = true;

  // RX overflow: bytes were lost somewhere inside the current backlog and we
  // can't tell where, so a later "frame" may be the tail of one frame glued to
  // the head of another. Drop the whole backlog and resync on a terminator.
  // (Loses a few seconds of frames; overflow only happens while loop() was
  // blocked, e.g. in a failing MQTT reconnect when nothing is published anyway.)
  // STM32 FOTA: the Core 0 worker asked for the UART. Stop touching it
  // (no reads here, applyCurrentValue() writes nothing) and confirm. When it is
  // handed back, drop whatever is buffered and resync on the next terminator.
  const bool stmUartBusy = stmUartRequest;
  if (stmUartBusy) {
    if (!stmUartReleased) {
      stmMsgBuffer = "";
      stmLatestFrame = "";
      stmUartReleased = true;
    }
  } else if (stmUartReleased) {
    stmUartReleased = false;
    s_uartOverflow = true;          // reuse the resync path below
  }

  if (!stmUartBusy && s_uartOverflow) {
    s_uartOverflow = false;
    while (testSerial.available()) testSerial.read();
    if (stmMsgBuffer.length() > 0) rejectFrame(UC_DROP_HWERR, stmMsgBuffer);
    stmMsgBuffer = "";
    s_uartErrorInFrame = false;
    stmSkipToTerminator = true;
  }

  while (!stmUartBusy && testSerial.available()) {
    char c = (char)testSerial.read();
    if (c == '*' || c == '\0' || c == '\n' || c == '\r') {
      // A HW receive error (bit error / break) happened while this frame was
      // on the wire. Consumed per frame so it never leaks into the next.
      bool hwError = s_uartErrorInFrame;
      s_uartErrorInFrame = false;
      if (stmSkipToTerminator) {
        stmSkipToTerminator = false;     // end of a partial/garbage line: drop it
      } else if (stmMsgBuffer.length() > 0) {
        String frame = stmMsgBuffer;
        frame.trim();
        if (acceptFrame(frame, hwError)) {
          stmLatestFrame = frame;        // keep only the most recent valid frame
          recordStmVersion(frame);       // field 13 (STM32 firmware >= 2.0.0)
          DBG_PRINT("[STM32] ");
          DBG_PRINTLN(frame);
        } else {
          DBG_PRINT("[STM32] rejected: ");
          DBG_PRINTLN(escapeFrame(frame));
        }
      }
      stmMsgBuffer = "";
    } else if (!stmSkipToTerminator) {
      stmMsgBuffer += c;
      if ((int)stmMsgBuffer.length() > STM_FRAME_MAX) {
        // Terminator never arrived: drop this line AND its tail up to the next
        // terminator (the tail would otherwise look like a new frame).
        rejectFrame(UC_TOO_LONG, stmMsgBuffer);
        stmMsgBuffer = "";
        stmSkipToTerminator = true;
      }
    }
  }
  reportUartStats(currentMillis);        // UART_STATS / STM32_BAD_FRAME every 5 min
  reportMemStats(currentMillis);         // STACK_STATS every 30 min

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
  stmFotaLoopTick();   // STM32 FOTA trigger (stm/update) -> Core 0

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
    staDisconnectReason = 0;
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

  // Publish the latest complete STM32 frame over MQTT (every 1s). Frames are
  // assembled char-by-char by the UART drain at the top of loop().
  if (currentMillis - previousMillis >= interval && isMqttConnected) {
    previousMillis = currentMillis;

    // Consume the most recent complete frame (empty if none arrived this second).
    String res = stmLatestFrame;
    stmLatestFrame = "";
    res.trim();

    DBG_PRINT("STM32 frame: [");
    DBG_PRINT(res);
    DBG_PRINTLN("]");

    if (res.isEmpty()) {
      DBG_PRINTLN("No data from STM32");
      DBG_PRINTLN("=======================");
    } else {
      String jsonString = "{\"value\":\"" + res + "\"}";

      DBG_PRINTLN("=== Publishing to MQTT ===");
      DBG_PRINT("Topic: ");
      DBG_PRINTLN(MQTT_TOPIC_DATA);
      DBG_PRINT("Payload: ");
      DBG_PRINTLN(jsonString);

      if (!MQTT_TOPIC_DATA.isEmpty()) {
        String signedJsonString = createSignedMessage(jsonString);
        bool dataPublished = mqttClient.publish(MQTT_TOPIC_DATA.c_str(), signedJsonString.c_str());
        DBG_PRINT("[STM32] publish ");
        DBG_PRINT(jsonString);
        DBG_PRINT(" -> ");
        DBG_PRINTLN(dataPublished ? "SUCCESS" : "FAILED");
      }
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
