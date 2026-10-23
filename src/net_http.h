#pragma once

#include <Arduino.h>
#include <device_api.h>      // esp32-common: deviceApiRequest, trackLogError, trackLogRL

// ---------------------------------------------------------------------------
// Blocking HTTP / OTA. EVERY function here runs on the Core 0 worker task only
// (device_api.h shares one HTTPClient). Never call these from loop()/Core 1 —
// enqueue a Job via worker.h instead.
// ---------------------------------------------------------------------------

// Configure device_api (call once from setup, before the worker starts). The
// clock is also set from the HTTP Date header while NTP has not answered.
void netHttpInit();

// Backend fetches. Update shared state (lastSetupValue / schedules[]) under stateMutex.
String getDeviceSettings(const String& deviceUid, const String& deviceSSID);
String getScheduleSettings(const String& deviceUid, const String& deviceSSID);
bool   registerDevice(const String& deviceId, const String& deviceName, const String& userId);
bool   updateFirmwareVersion(const String& firmwareVersion);

// ESP32 OTA (esp_ota.h): firmware URL from /api/firmware, then install.
void   handleFirmwareUpdate();

// Enqueue an OTA/status JSON line for Core 1 to publish over MQTT. Safe from any
// core (does not touch mqttClient directly).
void   publishOTAStatus(const String& status, const String& message = "", int progress = -1);
