#include "storage.h"
#include "shared_state.h"
#include "config.h"
#include <EEPROM.h>

String readStringFromEEPROM(int addr) {
  char data[150]; // Increased size for UTF-8 Vietnamese strings
  int len = 0;
  unsigned char k;
  k = EEPROM.read(addr);
  // Allow UTF-8 multibyte characters (Vietnamese) — no ASCII filter.
  while (k != '\0' && len < (int)sizeof(data) - 1) {
    data[len] = k;
    len++;
    k = EEPROM.read(addr + len);
  }
  data[len] = '\0';
  return String(data);
}

void writeStringToEEPROM(int addr, const String& str) {
  int len = str.length();
  for (int i = 0; i < len; i++) {
    EEPROM.write(addr + i, str[i]);
  }
  EEPROM.write(addr + len, '\0'); // Null-terminate the string
  EEPROM.commit();
}

void clearEEPROM() {
  for (int i = 0; i < 512; i++) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
}

void writeInfo(String ssid, String password, String userid) {
  String content = ssid + KEY_SPLIT;
  content = content + password;
  content = content + KEY_SPLIT;
  content = content + userid;
  writeStringToEEPROM(0, content);
}

void readWifi() {
  String strText = readStringFromEEPROM(0);
  if (strText.length() == 0 || strText.isEmpty()) {
    return;
  }

  String key_split = KEY_SPLIT;
  int index_split = strText.indexOf(key_split);
  param_ssid = strText.substring(0, index_split);
  strText.replace(param_ssid + key_split, "");
  index_split = strText.indexOf(key_split);
  param_password = strText.substring(0, index_split);
  strText.replace(param_password + key_split, "");
  uid = strText;
  uid.trim();
}

String getUid() {
  if (uid.isEmpty() == false) {
    uid.trim();  // normalise stray whitespace (used in MQTT topics + URLs)
    return uid;
  }
  String strText = readStringFromEEPROM(0);
  if (strText.length() == 0 || strText.isEmpty()) {
    return "";
  }

  String key_split = KEY_SPLIT;
  int index_split = strText.indexOf(key_split);
  param_ssid = strText.substring(0, index_split);
  strText.replace(param_ssid + key_split, "");
  index_split = strText.indexOf(key_split);
  param_password = strText.substring(0, index_split);
  strText.replace(param_password + key_split, "");
  uid = strText;
  uid.trim();
  return uid;
}

void saveSettingToStorage(const String& value) {
  if (value.isEmpty()) return;
  preferences.begin("device_setting", false);
  preferences.putString("setup_value", value);
  preferences.end();
}

String loadSettingFromStorage() {
  preferences.begin("device_setting", true);
  String value = preferences.getString("setup_value", "");
  preferences.end();
  return value;
}

// Last schedule written to / read from NVS, to skip identical writes: the
// schedule is re-fetched every 60 s but rarely changes.
static String s_storedSchedule;
static bool   s_storedScheduleKnown = false;
static const unsigned int kMaxStoredScheduleLen = 1900;  // NVS string limit is ~4000

void saveScheduleToStorage(const String& schedule) {
  String value = schedule;
  if (value == "null") value = "";          // server has no schedule for us
  if (value.length() > kMaxStoredScheduleLen) return;
  if (s_storedScheduleKnown && value == s_storedSchedule) return;

  preferences.begin("device_setting", false);
  preferences.putString("schedule", value);
  preferences.end();
  s_storedSchedule = value;
  s_storedScheduleKnown = true;
}

String loadScheduleFromStorage() {
  preferences.begin("device_setting", true);
  String value = preferences.getString("schedule", "");
  preferences.end();
  s_storedSchedule = value;
  s_storedScheduleKnown = true;
  return value;
}

void saveWifiBroadcastSSID(const String& ssid) {
  preferences.begin("wifi_config", false);
  preferences.putString("broadcast_ssid", ssid);
  preferences.end();
}

String loadWifiBroadcastSSID() {
  preferences.begin("wifi_config", true);
  String ssid = preferences.getString("broadcast_ssid", WIFI_BROADCAST_SSID);
  preferences.end();
  return ssid;
}
