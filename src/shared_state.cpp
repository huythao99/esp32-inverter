#include "shared_state.h"
#include "config.h"

// ---- RTOS primitives ------------------------------------------------------
SemaphoreHandle_t stateMutex     = nullptr;
QueueHandle_t     jobQueue       = nullptr;
QueueHandle_t     otaStatusQueue = nullptr;

// ---- Peripherals / clients ------------------------------------------------
HardwareSerial testSerial(2);   // UART2 for the STM32 link
Preferences             preferences;
WiFiClient              mqttWifiClient;
PubSubClient            mqttClient(mqttWifiClient);

// ---- MQTT topics ----------------------------------------------------------
String MQTT_TOPIC_DATA;
String MQTT_TOPIC_SETUP;
String MQTT_TOPIC_SCHEDULE;
String MQTT_TOPIC_STATUS;
String MQTT_TOPIC_FIRMWARE;
String MQTT_TOPIC_OTA_STATUS;
String MQTT_TOPIC_CMD_SETTINGS;
String MQTT_TOPIC_CMD_SCHEDULE;
String MQTT_TOPIC_SHARE;
String MQTT_TOPIC_BLACKLIST;

// ---- Command sync flags ---------------------------------------------------
volatile bool cmdSettingsPending = false;
volatile bool cmdSchedulePending = false;
unsigned long cmdSettingsAt = 0;
unsigned long cmdScheduleAt = 0;
const long    cmdDebounce = 500;

// ---- Share value ----------------------------------------------------------
volatile bool sharePending = false;
volatile int  shareValue = 0;
unsigned long shareAt = 0;
int           activeShareValue = -1;
unsigned long activeShareAt = 0;
const long    shareValidMs = 30000;
// Share debounce: coalesces a reconnect burst but stays well below the push
// interval so 1s pushes apply promptly (set 0 for immediate apply).
const long    shareDebounceMs = 100;

// ---- OTA trigger ----------------------------------------------------------
volatile bool otaPending = false;

// ---- Blacklist / device lock ----------------------------------------------
volatile bool deviceLocked  = false;
volatile bool unlockPending  = false;

// ---- Identity / config ----------------------------------------------------
String uid;
String wifiBroadcastSSID;
String currentFirmwareVersion = "1.0.12";
String param_ssid;
String param_password;

// ---- Connection state -----------------------------------------------------
int           connectMqtt = -1;
bool          isMqttConnected = false;
unsigned long lastMqttReconnectAttempt = 0;
int           mqttFailCount = 0;
const int     MQTT_MAX_FAIL_BEFORE_RESET = 6;  // reset WiFi after 6 failures (~60s)

// ---- Non-blocking WiFi reset state ----------------------------------------
bool          wifiResetPending = false;
unsigned long wifiResetAt = 0;

// ---- Firmware-version report flag -----------------------------------------
volatile bool isUpdateVersion = false;

// ---- NTP ------------------------------------------------------------------
volatile bool isNtpSynced = false;

// ---- Driver state ---------------------------------------------------------
ScheduleItem schedules[10];
int          scheduleCount = 0;
String       lastSetupValue = "";
bool         scheduleActive = false;

// ---- NTP / time config ----------------------------------------------------
const char* ntpServer  = "time.google.com";
const char* ntpServer2 = "pool.ntp.org";
const char* ntpServer3 = "216.239.35.0";   // time.google.com IP: fallback if DNS is blocked
const long  gmtOffset_sec = 0;
const int   daylightOffset_sec = 0;
