#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <HardwareSerial.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// ---------------------------------------------------------------------------
// State shared between the two cores.
//
// Threading model:
//   Core 1 (Arduino loop): MQTT, STM32 serial, applyCurrentValue(), web server.
//   Core 0 (worker task) : ALL blocking HTTP (settings/schedule/register/OTA).
//
// Cross-core data is protected by:
//   stateMutex      guards lastSetupValue, schedules[], scheduleCount, scheduleActive.
//   jobQueue        Core 1 -> Core 0 requests (see Job below).
//   otaStatusQueue  Core 0 -> Core 1 OTA status lines to publish over MQTT.
//                   (keeps ALL mqttClient access on Core 1 — no MQTT mutex needed.)
// ---------------------------------------------------------------------------

// Schedule entry parsed from the backend.
struct ScheduleItem {
  String startTime;
  String endTime;
  String value;
  String outValue;
};

// ---- Worker job queue (Core 1 -> Core 0) ----------------------------------
enum JobType : uint8_t {
  JOB_FETCH_SETTINGS,
  JOB_FETCH_SCHEDULE,
  JOB_REGISTER,
  JOB_UPDATE_VERSION,
  JOB_OTA,
  JOB_LOG,
  JOB_STM_OTA,     // flash the STM32 (stm_fota.cpp)
};

// Fixed-size buffers only: Strings must not be copied across the queue boundary
// (heap ownership would be shared between cores).
struct Job {
  JobType       type;
  char          code[24];      // JOB_LOG: error code
  char          message[100];  // JOB_LOG: error message
  unsigned long cooldownMs;    // JOB_LOG: rate-limit window
  uint32_t      crc32;         // JOB_STM_OTA: expected CRC32 (0 = unknown); version in code[]
  bool          force;         // JOB_STM_OTA: reflash same / unknown version
};

// ---- OTA status queue (Core 0 -> Core 1) ----------------------------------
enum OtaTarget : uint8_t {
  OTA_TARGET_ESP = 0,   // -> MQTT_TOPIC_OTA_STATUS
  OTA_TARGET_STM = 1,   // -> MQTT_TOPIC_STM_OTA_STATUS
};
struct OtaStatusMsg {
  uint8_t target;  // OtaTarget
  char json[192];  // ready-to-publish JSON payload
};

// ---- RTOS primitives ------------------------------------------------------
extern SemaphoreHandle_t stateMutex;
extern QueueHandle_t     jobQueue;
extern QueueHandle_t     otaStatusQueue;

// ---- Peripherals / clients ------------------------------------------------
extern HardwareSerial          testSerial;   // UART2, STM32 link
extern Preferences             preferences;
extern WiFiClient              mqttWifiClient;
extern PubSubClient            mqttClient;

// ---- MQTT topics (built on connect, Core 1) -------------------------------
extern String MQTT_TOPIC_DATA;
extern String MQTT_TOPIC_SETUP;
extern String MQTT_TOPIC_SCHEDULE;
extern String MQTT_TOPIC_STATUS;
extern String MQTT_TOPIC_FIRMWARE;
extern String MQTT_TOPIC_OTA_STATUS;
extern String MQTT_TOPIC_CMD_SETTINGS;
extern String MQTT_TOPIC_CMD_SCHEDULE;
extern String MQTT_TOPIC_SHARE;
extern String MQTT_TOPIC_BLACKLIST;
extern String MQTT_TOPIC_CMD_RESTART;
extern String MQTT_TOPIC_STM_UPDATE;       // server -> device: flash the STM32
extern String MQTT_TOPIC_STM_OTA_STATUS;   // device -> server: STM32 flash progress

// ---- Command sync flags (set in MQTT callback, drained in loop) -----------
extern volatile bool  cmdSettingsPending;
extern volatile bool  cmdSchedulePending;
extern unsigned long  cmdSettingsAt;
extern unsigned long  cmdScheduleAt;
extern const long     cmdDebounce;

// ---- Share value ----------------------------------------------------------
extern volatile bool  sharePending;
extern volatile int   shareValue;
extern unsigned long  shareAt;
extern int            activeShareValue;
extern unsigned long  activeShareAt;
extern const long     shareValidMs;
extern const long     shareDebounceMs;  // own debounce (not cmdDebounce): small so
                                        // fast (e.g. 1s) share pushes aren't delayed/starved

// ---- OTA trigger (set in MQTT callback) -----------------------------------
extern volatile bool  otaPending;
extern volatile bool  otaInProgress;   // set by the Core 0 worker while an OTA job runs

// ---- Remote restart (set in MQTT callback, executed in loop) --------------
extern volatile bool  restartPending;
extern unsigned long  restartAt;

// ---- Blacklist / device lock (server kill switch) -------------------------
// Both written by the MQTT callback and read by applyCurrentValue(); both run
// on Core 1, so a plain volatile bool is enough (no mutex needed).
//   deviceLocked  : current lock intent from the server.
//   unlockPending : set when a lock:false arrives while locked, so the next
//                   applyCurrentValue() emits one *UNLOCK54321# pulse.
extern volatile bool  deviceLocked;
extern volatile bool  unlockPending;

// ---- Identity / config ----------------------------------------------------
extern String uid;
extern String wifiBroadcastSSID;
extern String currentFirmwareVersion;
extern String param_ssid;
extern String param_password;

// ---- Connection state -----------------------------------------------------
extern int           connectMqtt;      // -1 pending, 0 failed, 1 success
extern bool          isMqttConnected;
extern unsigned long lastMqttReconnectAttempt;
extern int           mqttFailCount;
extern const int     MQTT_MAX_FAIL_BEFORE_RESET;

// ---- Non-blocking WiFi reset state ----------------------------------------
extern bool          wifiResetPending;
extern unsigned long wifiResetAt;

// ---- Firmware-version report flag (written by worker on success) ----------
extern volatile bool isUpdateVersion;

// ---- NTP ------------------------------------------------------------------
extern volatile bool isNtpSynced;

// ---- Driver state (guarded by stateMutex where noted) ---------------------
extern ScheduleItem schedules[10];   // guarded by stateMutex
extern int          scheduleCount;   // guarded by stateMutex
extern String       lastSetupValue;  // guarded by stateMutex
extern bool         scheduleActive;  // guarded by stateMutex

// ---- NTP / time config ----------------------------------------------------
extern const char* ntpServer;
extern const char* ntpServer2;
extern const char* ntpServer3;
extern const long  gmtOffset_sec;
extern const int   daylightOffset_sec;
