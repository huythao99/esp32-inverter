#pragma once

#include <Arduino.h>

// EEPROM helpers (WiFi credentials + UID blob at address 0).
String readStringFromEEPROM(int addr);
void   writeStringToEEPROM(int addr, const String& str);
void   clearEEPROM();
void   writeInfo(String ssid, String password, String userid);

// Parse the EEPROM blob into param_ssid / param_password / uid.
void   readWifi();

// Cached UID accessor (lazily parses EEPROM on first call).
String getUid();

// Base setting value persistence (NVS).
void   saveSettingToStorage(const String& value);
String loadSettingFromStorage();

// Schedule persistence (NVS): the raw schedule string from the server, so a
// rebooted device keeps running its schedule even if the server/API is not
// reachable yet. Only writes when the value actually changed (flash wear).
// Core 0 (worker) only after setup().
void   saveScheduleToStorage(const String& schedule);
String loadScheduleFromStorage();

// Grid-tie OFF flag (NVS, own namespace + own Preferences object: written from
// the MQTT callback on Core 1, while the worker on Core 0 uses `preferences`).
// Writes only when the value changes. Ignored after re-provisioning to
// another uid.
void   saveGridTieOff(bool off);
bool   loadGridTieOff();

// Broadcast SSID persistence (NVS) — set once, survives firmware uploads.
void   saveWifiBroadcastSSID(const String& ssid);
String loadWifiBroadcastSSID();
