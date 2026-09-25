#include "net_mqtt.h"
#include "shared_state.h"
#include "config.h"
#include "storage.h"
#include "worker.h"
#include "logic.h"
#include "stm_fota.h"
#include <ArduinoJson.h>
#include "time.h"

bool connectToMqtt() {
  lastMqttReconnectAttempt = millis();

  String clientId = "esp32-" + WiFi.macAddress();

  if (mqttClient.connect(clientId.c_str(), MQTT_USERNAME, MQTT_PASSWORD)) {
    isMqttConnected = true;
    connectMqtt = 1;
    mqttFailCount = 0;

    String currentUid = getUid();
    if (!currentUid.isEmpty()) {
      MQTT_TOPIC_SETUP        = "inverter/" + currentUid + "/" + wifiBroadcastSSID + "/setup/value";
      MQTT_TOPIC_SCHEDULE     = "inverter/" + currentUid + "/" + wifiBroadcastSSID + "/schedule/value";
      MQTT_TOPIC_DATA         = "inverter/" + currentUid + "/" + wifiBroadcastSSID + "/data";
      MQTT_TOPIC_STATUS       = "inverter/" + currentUid + "/" + wifiBroadcastSSID + "/status";
      MQTT_TOPIC_FIRMWARE     = "inverter/" + currentUid + "/" + wifiBroadcastSSID + "/firmware/update";
      MQTT_TOPIC_OTA_STATUS   = "inverter/" + currentUid + "/" + wifiBroadcastSSID + "/ota/status";
      MQTT_TOPIC_CMD_SETTINGS = "inverter/" + currentUid + "/" + wifiBroadcastSSID + "/cmd/settings";
      MQTT_TOPIC_CMD_SCHEDULE = "inverter/" + currentUid + "/" + wifiBroadcastSSID + "/cmd/schedule";
      MQTT_TOPIC_SHARE        = "inverter/" + currentUid + "/" + wifiBroadcastSSID + "/share";
      MQTT_TOPIC_BLACKLIST    = "inverter/" + currentUid + "/" + wifiBroadcastSSID + "/blacklist";
      MQTT_TOPIC_CMD_RESTART  = "inverter/" + currentUid + "/" + wifiBroadcastSSID + "/cmd/restart";
      MQTT_TOPIC_STM_UPDATE     = "inverter/" + currentUid + "/" + wifiBroadcastSSID + "/stm/update";
      MQTT_TOPIC_STM_OTA_STATUS = "inverter/" + currentUid + "/" + wifiBroadcastSSID + "/stm/ota/status";

      // Only subscribe to server->device control topics. Do NOT subscribe to
      // STATUS / DATA: the device publishes those itself, so subscribing echoes
      // every message straight back into the callback (self-flood) and the slow
      // 9600-baud debug prints starve the SoftwareSerial STM32 link.
      mqttClient.subscribe(MQTT_TOPIC_SETUP.c_str());
      mqttClient.subscribe(MQTT_TOPIC_SCHEDULE.c_str());
      mqttClient.subscribe(MQTT_TOPIC_FIRMWARE.c_str());
      mqttClient.subscribe(MQTT_TOPIC_CMD_SETTINGS.c_str(), 1);  // QoS 1
      mqttClient.subscribe(MQTT_TOPIC_CMD_SCHEDULE.c_str(), 1);  // QoS 1
      mqttClient.subscribe(MQTT_TOPIC_SHARE.c_str(), 1);         // QoS 1
      mqttClient.subscribe(MQTT_TOPIC_BLACKLIST.c_str(), 1);     // QoS 1: lock must not be missed
      mqttClient.subscribe(MQTT_TOPIC_CMD_RESTART.c_str(), 1);   // QoS 1, never retained
      mqttClient.subscribe(MQTT_TOPIC_STM_UPDATE.c_str());       // QoS 0, never retained

      DBG_PRINT("Subscribed cmd/settings: [");
      DBG_PRINT(MQTT_TOPIC_CMD_SETTINGS);
      DBG_PRINTLN("]");
      DBG_PRINT("Subscribed cmd/schedule: [");
      DBG_PRINT(MQTT_TOPIC_CMD_SCHEDULE);
      DBG_PRINTLN("]");
    }
    return true;
  }

  isMqttConnected = false;
  connectMqtt = 0;
  mqttFailCount++;
  DBG_PRINT("MQTT connection failed, fail count: ");
  DBG_PRINTLN(mqttFailCount);
  return false;
}

void setupMqttCallback() {
  mqttClient.setCallback([](char* topic, byte* payload, unsigned int length) {
    String message = "";
    for (unsigned int i = 0; i < length; i++) {
      message += (char)payload[i];
    }

    String topicStr = String(topic);
    DBG_PRINTLN("=== MQTT MESSAGE RECEIVED ===");
    DBG_PRINT("Topic: ");
    DBG_PRINTLN(topicStr);
    DBG_PRINT("Message: ");
    DBG_PRINTLN(message);

    // Firmware update: hand off to Core 0 so the callback returns immediately.
    if (topicStr == MQTT_TOPIC_FIRMWARE) {
      otaPending = true;
    }
    // STM32 firmware update: parse here, enqueue from loop (stmFotaLoopTick).
    else if (topicStr == MQTT_TOPIC_STM_UPDATE) {
      stmFotaOnTrigger(message);
    }
    // Command topics: payload is just "{}" - react to the topic name only.
    else if (topicStr.endsWith("/cmd/settings")) {
      cmdSettingsPending = true;
      cmdSettingsAt = millis();
    }
    else if (topicStr.endsWith("/cmd/schedule")) {
      cmdSchedulePending = true;
      cmdScheduleAt = millis();
    }
    // Remote restart: payload {"requestId":"...","source":"app","ts":<epoch ms>}.
    // Guards against reboot loops if a restart message is ever redelivered or
    // retained by mistake:
    //  - ignored during the first 20s after boot,
    //  - ignored when older than 2 minutes (only checkable once the clock is set).
    // The reboot itself happens in loop() after a short delay (see main.cpp).
    else if (topicStr.endsWith("/cmd/restart")) {
      bool accept = millis() > 20000UL;
      if (accept && isTimeValid()) {
        JsonDocument doc;
        if (deserializeJson(doc, message) == DeserializationError::Ok &&
            doc["ts"].is<double>()) {
          double nowMs = (double)time(nullptr) * 1000.0;
          double ageMs = nowMs - doc["ts"].as<double>();
          if (ageMs > 120000.0) accept = false;
        }
      }
      if (accept && !restartPending) {
        restartPending = true;
        restartAt = millis();
      }
      DBG_PRINT("[RESTART] command ");
      DBG_PRINTLN(accept ? "accepted" : "ignored (stale / just booted)");
    }
    // Share topic: payload is {"value":N}. Parse now, apply from loop().
    else if (topicStr.endsWith("/share")) {
      JsonDocument doc;
      if (deserializeJson(doc, message) == DeserializationError::Ok) {
        shareValue = doc["value"].as<int>();
        sharePending = true;
        shareAt = millis();
      }
    }
    // Blacklist topic: payload is {"lock":true|false}. Server kill switch.
    // Both this callback and applyCurrentValue() run on Core 1, so flipping the
    // volatile flags here is safe without a mutex. The actual STM32 write stays
    // in applyCurrentValue() (single-writer) — see logic.cpp.
    else if (topicStr.endsWith("/blacklist")) {
      JsonDocument doc;
      if (deserializeJson(doc, message) == DeserializationError::Ok) {
        bool lock = doc["lock"].as<bool>();
        if (lock) {
          deviceLocked = true;
          unlockPending = false;      // a fresh lock cancels any queued unlock pulse
        } else {
          if (deviceLocked) unlockPending = true;  // emit one *UNLOCK54321# pulse
          deviceLocked = false;
        }
        DBG_PRINT("[BLACKLIST] lock=");
        DBG_PRINTLN(lock ? "true" : "false");
      }
    }
  });
}

void drainOtaStatus() {
  if (!otaStatusQueue) return;
  OtaStatusMsg msg;
  while (xQueueReceive(otaStatusQueue, &msg, 0) == pdTRUE) {
    const String& topic = (msg.target == OTA_TARGET_STM) ? MQTT_TOPIC_STM_OTA_STATUS
                                                         : MQTT_TOPIC_OTA_STATUS;
    if (mqttClient.connected() && !topic.isEmpty()) {
      bool published = mqttClient.publish(topic.c_str(), msg.json);
      DBG_PRINT("MQTT OTA status published: ");
      DBG_PRINTLN(published ? "Success" : "Failed");
    }
  }
}

void startWiFiReset() {
  DBG_PRINTLN("=== Resetting WiFi to clear DNS cache (non-blocking) ===");
  mqttClient.disconnect();
  WiFi.disconnect(true);       // true = erase credentials from memory
  WiFi.mode(WIFI_AP_STA);
  wifiResetPending = true;
  wifiResetAt = millis();
}
