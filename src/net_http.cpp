#include "net_http.h"
#include "shared_state.h"
#include "config.h"
#include "storage.h"
#include "logic.h"
#include <esp_ota.h>         // esp32-common
#include <nvs_store.h>       // esp32-common
#include <ArduinoJson.h>

// All calls go through device_api (esp32-common): TLS verified against the
// pinned root CAs, no device auth (inverter endpoints), and the clock is set
// from the HTTP Date header while NTP (UDP 123) is blocked / not answered yet.

static String apiUid()      { return getUid(); }
static String apiDeviceId() { return wifiBroadcastSSID; }

void netHttpInit() {
  DeviceApiConfig cfg = {};
  cfg.apiBase = API_BASE;
  cfg.userId = apiUid;
  cfg.deviceId = apiDeviceId;
  cfg.deviceAuth = false;
  cfg.syncClockFromDate = true;
#if DEBUG
  cfg.debug = &Serial;
#else
  cfg.debug = nullptr;
#endif
  deviceApiInit(cfg);
}

void publishOTAStatus(const String& status, const String& message, int progress) {
  DBG_PRINT("OTA Status: ");
  DBG_PRINT(status);
  if (!message.isEmpty()) { DBG_PRINT(" - "); DBG_PRINT(message); }
  if (progress >= 0) { DBG_PRINT(" ("); DBG_PRINT(progress); DBG_PRINT("%)"); }
  DBG_PRINTLN();

  // Hand to Core 1 for the actual mqttClient.publish (keeps MQTT single-threaded).
  if (otaStatusQueue) {
    const String json = espOtaStatusJson(status.c_str(), message, progress);
    OtaStatusMsg msg = {};
    msg.target = OTA_TARGET_ESP;
    strncpy(msg.json, json.c_str(), sizeof(msg.json) - 1);
    xQueueSend(otaStatusQueue, &msg, 0);
  }
}

static void otaStatusCb(const char* status, const String& message, int progress) {
  publishOTAStatus(status, message, progress);
}

void handleFirmwareUpdate() {
  DBG_PRINTLN("=== FIRMWARE UPDATE STARTED ===");
  if (getUid().isEmpty()) {   // as before: no owner, no firmware lookup
    publishOTAStatus("failed", "Failed to get firmware URL from server");
    trackLogRL("FOTA_URL_FAILED", "Failed to get firmware URL from server (no uid)");
    return;
  }
  espOtaRun("/api/firmware?deviceId=" + wifiBroadcastSSID, otaStatusCb);
  DBG_PRINTLN("=== FIRMWARE UPDATE FAILED ===");   // espOtaRun reboots on success
}

bool updateFirmwareVersion(const String& firmwareVersion) {
  const String currentUid = getUid();
  if (currentUid.isEmpty()) return false;
  const String path = "/api/inverter-device/data/" + currentUid + "/" + wifiBroadcastSSID + "/firmware";
  const int code = deviceApiRequest("PATCH", path, "{\"firmwareVersion\":\"" + firmwareVersion + "\"}");
  return code == 200 || code == 204;
}

// Offline fallback: the last cloud setting from NVS (applyCurrentValue() uses
// it). Core 0 is the sole writer of lastSetupValue, so reading emptiness is
// safe; guard only the assignment against Core 1 readers.
static void loadSettingIfEmpty() {
  if (!lastSetupValue.isEmpty()) return;
  String v = nvsLoadSetting();
  if (v.isEmpty()) return;
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  lastSetupValue = v;
  xSemaphoreGive(stateMutex);
}

String getDeviceSettings(const String& deviceUid, const String& deviceSSID) {
  if (WiFi.status() != WL_CONNECTED) {
    loadSettingIfEmpty();
    return "";
  }

  String response;
  const int code = deviceApiRequest(
      "GET", "/api/inverter-setting/data/" + deviceUid + "/" + deviceSSID + "?source=hardware",
      "", &response);

  DBG_PRINT("[SETTING] HTTP code: ");
  DBG_PRINTLN(code);

  if (code <= 0) {
    loadSettingIfEmpty();
    return "";
  }
  DBG_PRINT("[SETTING] raw response: ");
  DBG_PRINTLN(response);
  if (code != 200) return response;

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);
  if (!error && doc["gridTieOff"] == true) {
    // While grid-tie is OFF the server answers with the OFF command
    // (99001001) instead of the real setting. Never cache it as the
    // setting (it would keep running after grid-tie is turned back ON):
    // take it as the OFF state instead (fallback if cmd/grid-tie was
    // missed; ON always comes from the retained cmd/grid-tie).
    if (!gridTieOff) {
      gridTieOff = true;
      saveGridTieOff(true);
      DBG_PRINTLN("[SETTING] grid-tie OFF (from API)");
    }
  } else if (!error) {
    String value = doc["value"].as<String>();
    String newSetupValue = convertSetupValue(value);

    DBG_PRINT("[SETTING] value from API: '");
    DBG_PRINT(value);
    DBG_PRINT("' -> converted: '");
    DBG_PRINT(newSetupValue);
    DBG_PRINTLN("'");

    if (!newSetupValue.isEmpty() && newSetupValue != lastSetupValue) {
      nvsSaveSetting(newSetupValue);          // NVS write, outside lock
      xSemaphoreTake(stateMutex, portMAX_DELAY);
      lastSetupValue = newSetupValue;
      xSemaphoreGive(stateMutex);
      DBG_PRINTLN("[SETTING] cached (changed) -> applyCurrentValue will push it");
    } else if (newSetupValue.isEmpty()) {
      DBG_PRINTLN("[SETTING] REJECTED: value is not 8 digits (HHHHLLLL)");
    }
  } else {
    DBG_PRINT("[SETTING] JSON parse error: ");
    DBG_PRINTLN(error.c_str());
  }
  return response;
}

String getScheduleSettings(const String& deviceUid, const String& deviceSSID) {
  if (WiFi.status() != WL_CONNECTED) return "";

  String response;
  const int code = deviceApiRequest(
      "GET", "/api/inverter-schedule/data/" + deviceUid + "/" + deviceSSID + "?source=hardware",
      "", &response);

  if (code <= 0) {
    trackLogRL("SCHEDULE_CONN_ERR", "No response, code=" + String(code));
    return "";
  }
  if (code != 200) {
    trackLogRL("SCHEDULE_HTTP_ERR", "HTTP " + String(code));
    return response;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);
  if (!error && doc["gridTieOff"] == true) {
    // OFF slots served while grid-tie is OFF: keep the real schedule.
    if (!gridTieOff) {
      gridTieOff = true;
      saveGridTieOff(true);
    }
  } else if (!error) {
    String value = doc["schedule"].as<String>();
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    parseScheduleData(value);
    int count = scheduleCount;
    xSemaphoreGive(stateMutex);
    // Persist (outside the lock, NVS write) so a reboot while the server
    // is unreachable still runs the last known schedule.
    saveScheduleToStorage(value);
    DBG_PRINT("[SCHEDULE] loaded, count="); DBG_PRINTLN(count);
  } else {
    trackLogRL("SCHEDULE_JSON_ERR", "JSON parse failed: " + String(error.c_str()));
  }
  return response;
}

bool registerDevice(const String& deviceId, const String& deviceName, const String& userId) {
  String body = "{";
  body += "\"deviceId\":\"" + deviceId + "\",";
  body += "\"deviceName\":\"" + deviceName + "\",";
  body += "\"userId\":\"" + userId + "\",";
  body += "\"firmwareVersion\":\"" + currentFirmwareVersion + "\"";
  body += "}";
  const int code = deviceApiRequest("POST", "/api/inverter-device/data", body);
  return code == 200 || code == 201;
}
