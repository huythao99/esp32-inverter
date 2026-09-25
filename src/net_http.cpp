#include "net_http.h"
#include "shared_state.h"
#include "config.h"
#include "storage.h"
#include "logic.h"
#include "ca_certs.h"
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "time.h"
#include <sys/time.h>

// Shared HTTPClient — Core 0 only. All API calls go over TLS through one
// WiFiClientSecure that verifies the server certificate against the pinned
// root CAs (ca_certs.h). A plain http.begin(url) would create an unverified
// TLS client, so a fake server (DNS spoofing, rogue router) could answer.
static HTTPClient http;
static WiFiClientSecure s_tls;

// Response headers we want HTTPClient to keep. `Date` is the fallback clock
// source when NTP (UDP 123) is blocked by the router/ISP.
static const char* kCollectHeaders[] = {"Date"};

// http.begin() + ask HTTPClient to keep the Date header of the response.
static void beginJson(const String& url) {
  http.begin(s_tls, url);
  http.collectHeaders(kCollectHeaders, 1);
  http.addHeader("Content-Type", "application/json");
}

// If the clock is still unset (NTP not reached yet), set it from the server's
// HTTP Date header. Second-level accuracy is plenty for minute-based schedules;
// SNTP overwrites it with the precise time as soon as it gets through.
static void syncClockFromHttpDate() {
  if (isTimeValid()) return;
  time_t t = parseHttpDate(http.header("Date"));
  if (t <= 0) return;
  struct timeval tv = { t, 0 };
  settimeofday(&tv, nullptr);
  DBG_PRINT("[TIME] clock set from HTTP Date: ");
  DBG_PRINTLN((long)t);
}

void netHttpInit() {
  // Don't reuse HTTPS connections: polls are ~60s apart, so the server closes the
  // idle keep-alive connection and reuse then fails with a TLS reset (ssl_client
  // -76). A fresh connection per request avoids that.
  http.setReuse(false);
  s_tls.setCACert(kRootCA);
  s_tls.setHandshakeTimeout(15);   // seconds
  http.setTimeout(3000);  // 3s timeout to bound blocking on Core 0
}

bool trackLogError(const String& errorCode, const String& errorMessage) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  String currentUid = getUid();
  String url = String(API_BASE) + "/api/track-log-error";

  beginJson(url);

  String jsonPayload = "{";
  jsonPayload += "\"userId\":\"" + jsonEscape(currentUid) + "\",";
  jsonPayload += "\"deviceId\":\"" + jsonEscape(wifiBroadcastSSID) + "\",";
  jsonPayload += "\"errorCode\":\"" + jsonEscape(errorCode) + "\",";
  jsonPayload += "\"errorMessage\":\"" + jsonEscape(errorMessage) + "\"";
  jsonPayload += "}";

  int httpResponseCode = http.POST(jsonPayload);
  syncClockFromHttpDate();
  bool success = (httpResponseCode == 200 || httpResponseCode == 201);

  DBG_PRINT("Track error log [");
  DBG_PRINT(errorCode);
  DBG_PRINT("] -> HTTP ");
  DBG_PRINTLN(httpResponseCode);

  http.end();
  return success;
}

bool trackLogRL(const String& code, const String& message, unsigned long cooldownMs) {
  struct Entry { String code; unsigned long sentAt; };
  static Entry cache[8];
  static int cacheLen = 0;
  unsigned long now = millis();
  for (int i = 0; i < cacheLen; i++) {
    if (cache[i].code == code) {
      if (now - cache[i].sentAt < cooldownMs) return false;
      cache[i].sentAt = now;
      return trackLogError(code, message);
    }
  }
  int slot = (cacheLen < 8) ? cacheLen++ : 0;
  cache[slot] = {code, now};
  return trackLogError(code, message);
}

void publishOTAStatus(const String& status, const String& message, int progress) {
  // Console echo (any core).
  DBG_PRINT("OTA Status: ");
  DBG_PRINT(status);
  if (!message.isEmpty()) { DBG_PRINT(" - "); DBG_PRINT(message); }
  if (progress >= 0) { DBG_PRINT(" ("); DBG_PRINT(progress); DBG_PRINT("%)"); }
  DBG_PRINTLN();

  String jsonStatus = "{\"status\":\"" + status + "\"";
  if (!message.isEmpty()) {
    jsonStatus += ",\"message\":\"" + jsonEscape(message) + "\"";
  }
  if (progress >= 0) {
    jsonStatus += ",\"progress\":" + String(progress);
  }
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10)) {
    char timeStr[30];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
    jsonStatus += ",\"timestamp\":\"" + String(timeStr) + "\"";
  }
  jsonStatus += "}";

  // Hand to Core 1 for the actual mqttClient.publish (keeps MQTT single-threaded).
  if (otaStatusQueue) {
    OtaStatusMsg msg;
    msg.target = OTA_TARGET_ESP;
    strncpy(msg.json, jsonStatus.c_str(), sizeof(msg.json) - 1);
    msg.json[sizeof(msg.json) - 1] = '\0';
    xQueueSend(otaStatusQueue, &msg, 0);
  }
}

bool updateFirmwareVersion(const String& firmwareVersion) {
  if (WiFi.status() != WL_CONNECTED) return false;

  String currentUid = getUid();
  if (currentUid.isEmpty()) return false;

  String url = String(API_BASE) + "/api/inverter-device/data/" + currentUid + "/" + wifiBroadcastSSID + "/firmware";

  beginJson(url);

  String jsonPayload = "{\"firmwareVersion\":\"" + firmwareVersion + "\"}";
  int httpResponseCode = http.sendRequest("PATCH", jsonPayload);
  syncClockFromHttpDate();
  bool success = false;
  if (httpResponseCode > 0) {
    http.getString();
    if (httpResponseCode == 200 || httpResponseCode == 204) success = true;
  }
  http.end();
  return success;
}

String getFirmwareS3URL(const String& version) {
  if (WiFi.status() != WL_CONNECTED) return "";

  String currentUid = getUid();
  if (currentUid.isEmpty()) return "";

  String url = String(API_BASE) + "/api/firmware";
  url += "?deviceId=" + wifiBroadcastSSID;

  beginJson(url);

  int httpResponseCode = http.GET();
  syncClockFromHttpDate();
  String s3URL = "";
  if (httpResponseCode > 0) {
    String response = http.getString();
    if (httpResponseCode == 200) {
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, response);
      if (!error) {
        if (doc.containsKey("url")) {
          s3URL = doc["url"].as<String>();
        } else if (doc.containsKey("downloadUrl")) {
          s3URL = doc["downloadUrl"].as<String>();
        }
      }
    }
  }
  http.end();
  return s3URL;
}

// Last download % published (reset per update() so retries report from 0).
static int s_otaLastPct = -1;

bool performFOTAUpdate(const String& firmwareURL) {
  if (WiFi.status() != WL_CONNECTED) return false;

  s_otaLastPct = -1;   // start each attempt fresh

  // 30s HTTP timeout (default is 8s): TLS handshake + first byte from S3/CDN can
  // exceed 8s on a slow link, which surfaces as "HTTP error: read Timeout".
  HTTPUpdate httpUpdate(30000);
  // Presigned S3 / CloudFront URLs often 30x-redirect; the default does NOT follow
  // redirects, so the GET returns a redirect body and the update fails.
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  // Publish success/reboot ourselves so Core 1 has time to flush the status.
  httpUpdate.rebootOnUpdate(false);
  httpUpdate.onStart([]() {
    publishOTAStatus("installing", "Firmware download completed, installing...", 0);
  });
  httpUpdate.onEnd([]() {
    publishOTAStatus("installing", "Firmware installation completed", 100);
  });
  httpUpdate.onProgress([](int cur, int total) {
    int progress = (total > 0) ? (cur * 100) / total : 0;
    // Publish every 10% (and the final 100%). s_otaLastPct is reset before each
    // update() so repeated attempts in one boot session still report from 0.
    if (progress >= s_otaLastPct + 10 || progress >= 100) {
      publishOTAStatus("downloading", "Downloading firmware", progress);
      s_otaLastPct = progress;
    }
  });
  httpUpdate.onError([](int err) {
    publishOTAStatus("failed", "Firmware update error: " + String(err));
  });

  // Verify the firmware server's certificate: an unverified download would
  // let anyone who can redirect the traffic install their own firmware.
  WiFiClientSecure client;
  client.setCACert(kRootCA);
  client.setHandshakeTimeout(30);   // seconds — allow a slow TLS handshake

  t_httpUpdate_return ret = httpUpdate.update(client, firmwareURL);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      publishOTAStatus("failed", "Update failed - connection lost");
      return false;
    case HTTP_UPDATE_NO_UPDATES:
      publishOTAStatus("failed", "No updates available");
      return false;
    case HTTP_UPDATE_OK:
      publishOTAStatus("success", "Update completed, rebooting...");
      delay(1500);        // let Core 1 drain otaStatusQueue and publish "success"
      ESP.restart();      // manual reboot (rebootOnUpdate is off)
      return true;
    default:
      publishOTAStatus("failed", "Unknown update error");
      return false;
  }
}

void handleFirmwareUpdate() {
  DBG_PRINTLN("=== FIRMWARE UPDATE STARTED ===");
  publishOTAStatus("starting", "Requesting firmware URL from server");

  String s3URL = getFirmwareS3URL();
  DBG_PRINT("S3 URL received: ");
  DBG_PRINTLN(s3URL.isEmpty() ? "EMPTY" : s3URL);

  if (s3URL.isEmpty()) {
    publishOTAStatus("failed", "Failed to get firmware URL from server");
    trackLogRL("FOTA_URL_FAILED", "Failed to get firmware URL from server");
    return;
  }

  publishOTAStatus("downloading", "Starting firmware download");
  bool updateResult = performFOTAUpdate(s3URL);
  DBG_PRINT("Update result: ");
  DBG_PRINTLN(updateResult ? "SUCCESS" : "FAILED");

  if (updateResult) {
    DBG_PRINTLN("=== FIRMWARE UPDATE SUCCESSFUL ===");
    publishOTAStatus("success", "Firmware update completed successfully, rebooting...");
  } else {
    DBG_PRINTLN("=== FIRMWARE UPDATE FAILED ===");
    publishOTAStatus("failed", "Firmware download or installation failed");
    trackLogRL("FOTA_FAILED", "Firmware download or installation failed");
  }
}

String getDeviceSettings(const String& deviceUid, const String& deviceSSID) {
  if (WiFi.status() != WL_CONNECTED) {
    // Load from storage if WiFi not connected (applyCurrentValue() uses it).
    // Core 0 is the sole writer of lastSetupValue, so reading emptiness is safe;
    // guard only the assignment against Core 1 readers.
    if (lastSetupValue.isEmpty()) {
      String v = loadSettingFromStorage();
      if (!v.isEmpty()) {
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        lastSetupValue = v;
        xSemaphoreGive(stateMutex);
      }
    }
    return "";
  }

  String url = String(API_BASE) + "/api/inverter-setting/data/" + deviceUid + "/" + deviceSSID + "?source=hardware";

  beginJson(url);

  int httpResponseCode = http.GET();
  syncClockFromHttpDate();
  String response = "";

  DBG_PRINT("[SETTING] HTTP code: ");
  DBG_PRINTLN(httpResponseCode);

  if (httpResponseCode > 0) {
    response = http.getString();
    DBG_PRINT("[SETTING] raw response: ");
    DBG_PRINTLN(response);

    if (httpResponseCode == 200) {
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, response);
      if (!error) {
        String value = doc["value"].as<String>();
        String newSetupValue = convertSetupValue(value);

        DBG_PRINT("[SETTING] value from API: '");
        DBG_PRINT(value);
        DBG_PRINT("' -> converted: '");
        DBG_PRINT(newSetupValue);
        DBG_PRINTLN("'");

        if (!newSetupValue.isEmpty() && newSetupValue != lastSetupValue) {
          saveSettingToStorage(newSetupValue);          // NVS write, outside lock
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
    }
  } else {
    if (lastSetupValue.isEmpty()) {
      String v = loadSettingFromStorage();
      if (!v.isEmpty()) {
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        lastSetupValue = v;
        xSemaphoreGive(stateMutex);
      }
    }
  }

  http.end();
  return response;
}

String getScheduleSettings(const String& deviceUid, const String& deviceSSID) {
  if (WiFi.status() != WL_CONNECTED) return "";

  String url = String(API_BASE) + "/api/inverter-schedule/data/" + deviceUid + "/" + deviceSSID + "?source=hardware";

  beginJson(url);

  int httpResponseCode = http.GET();
  syncClockFromHttpDate();
  String response = "";

  if (httpResponseCode > 0) {
    response = http.getString();
    if (httpResponseCode == 200) {
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, response);
      if (!error) {
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
    } else {
      trackLogRL("SCHEDULE_HTTP_ERR", "HTTP " + String(httpResponseCode));
    }
  } else {
    trackLogRL("SCHEDULE_CONN_ERR", "No response, code=" + String(httpResponseCode));
  }

  http.end();
  return response;
}

bool registerDevice(const String& deviceId, const String& deviceName, const String& userId) {
  if (WiFi.status() != WL_CONNECTED) return false;

  String url = String(API_BASE) + "/api/inverter-device/data";

  beginJson(url);

  String jsonPayload = "{";
  jsonPayload += "\"deviceId\":\"" + deviceId + "\",";
  jsonPayload += "\"deviceName\":\"" + deviceName + "\",";
  jsonPayload += "\"userId\":\"" + userId + "\",";
  jsonPayload += "\"firmwareVersion\":\"" + currentFirmwareVersion + "\"";
  jsonPayload += "}";

  int httpResponseCode = http.POST(jsonPayload);
  syncClockFromHttpDate();
  bool success = false;
  if (httpResponseCode > 0) {
    http.getString();
    if (httpResponseCode == 200 || httpResponseCode == 201) success = true;
  }
  http.end();
  return success;
}
