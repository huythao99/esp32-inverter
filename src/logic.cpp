#include "logic.h"
#include "stm_fota.h"
#include "shared_state.h"
#include "config.h"
#include "worker.h"   // trackLog() (enqueues; non-blocking)
#include "time.h"

String convertSetupValue(const String& input) {
  if (input.length() != 8) {
    return "";
  }
  int pset = input.substring(0, 4).toInt();  // First 4 digits as Pset
  int vset = input.substring(4, 8).toInt();  // Last 4 digits as Vset
  char buffer[50];
  snprintf(buffer, sizeof(buffer), "*%d@%d#", pset, vset);
  return String(buffer);
}

bool isTimeInRange(const String& currentTime, const String& startTime, const String& endTime) {
  int currentMinutes = (currentTime.substring(0, 2).toInt() * 60) + currentTime.substring(3, 5).toInt();
  int startMinutes = (startTime.substring(0, 2).toInt() * 60) + startTime.substring(3, 5).toInt();

  int endMinutes;
  if (endTime.substring(0, 2).toInt() >= 24) {
    int adjustedHour = endTime.substring(0, 2).toInt() - 24;   // 27:00 -> 03:00
    endMinutes = (adjustedHour * 60) + endTime.substring(3, 5).toInt();
  } else {
    endMinutes = (endTime.substring(0, 2).toInt() * 60) + endTime.substring(3, 5).toInt();
  }

  if (endMinutes < startMinutes) {
    // Schedule spans midnight (e.g., 18:00 to 03:00)
    return currentMinutes >= startMinutes || currentMinutes <= endMinutes;
  }
  return currentMinutes >= startMinutes && currentMinutes <= endMinutes;
}

void parseScheduleData(const String& scheduleData) {
  for (int i = 0; i < 10; i++) {
    schedules[i].startTime = "";
    schedules[i].endTime = "";
    schedules[i].value = "";
    schedules[i].outValue = "";
  }
  scheduleCount = 0;

  if (scheduleData.isEmpty() || scheduleData == "null") return;

  int startIndex = 0;
  int endIndex = 0;

  while (startIndex < (int)scheduleData.length() && scheduleCount < 10) {
    endIndex = scheduleData.indexOf('#', startIndex);
    if (endIndex == -1) endIndex = scheduleData.length();

    String schedule = scheduleData.substring(startIndex, endIndex);

    int startPos = schedule.indexOf("start=");
    int endPos = schedule.indexOf("&", startPos);
    if (startPos != -1) {
      schedules[scheduleCount].startTime = schedule.substring(startPos + 6, endPos);
      schedules[scheduleCount].startTime.trim();
    }

    startPos = schedule.indexOf("end=");
    endPos = schedule.indexOf("&", startPos);
    if (startPos != -1) {
      schedules[scheduleCount].endTime = schedule.substring(startPos + 4, endPos);
      schedules[scheduleCount].endTime.trim();
    }

    startPos = schedule.indexOf("value=");
    if (startPos != -1) {
      schedules[scheduleCount].value = schedule.substring(startPos + 6);
      schedules[scheduleCount].value.trim();
    }

    scheduleCount++;
    startIndex = endIndex + 1;
  }
}

String currentScheduleValue() {
  if (scheduleCount <= 0) return "";
  // Check the clock itself instead of the SNTP status: sntp_get_sync_status()
  // reports COMPLETED only ONCE (then resets), and the clock may also have been
  // set by the HTTP Date fallback without SNTP ever succeeding.
  if (!isTimeValid()) {
    trackLog("NTP_NOT_SYNCED", "Clock not set yet (no NTP/HTTP time), schedule skipped", 120000);
    return "";
  }
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10)) return "";   // short timeout: never blocks
  char timeStr[6];
  strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
  String currentTime = String(timeStr);

  for (int i = 0; i < scheduleCount; i++) {
    if (schedules[i].startTime.isEmpty()) continue;
    if (isTimeInRange(currentTime, schedules[i].startTime, schedules[i].endTime)) {
      return convertSetupValue(schedules[i].value);
    }
  }
  return "";
}

String buildShareValue(int shareWatts) {
  int at = lastSetupValue.indexOf('@');
  if (lastSetupValue.length() < 2 || lastSetupValue[0] != '*' || at < 1) return "";

  int value = shareWatts;
  if (value > 9999) value = 9999;   // max 4 digits
  if (value < 0)    value = 0;

  String pset = lastSetupValue.substring(1, at);       // keep the cap part
  return "*" + pset + "@" + String(value) + "#";
}

String jsonEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (unsigned int i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else {
      out += c;
    }
  }
  return out;
}

String createSignedMessage(const String& payload) {
  return payload;
}

void onNtpSync(struct timeval* tv) {
  isNtpSynced = true;
  // Do NOT do HTTP here — this runs on the SNTP task, not loop().
}

bool isTimeValid() {
  // Any date after 2023-11-14 means the clock was set (it boots at 1970).
  return time(nullptr) > 1700000000;
}

// Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's algorithm).
static long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

time_t parseHttpDate(const String& s) {
  // "Wed, 23 Sep 2026 08:12:34 GMT"
  int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
  char mon[4] = {0};
  const char* p = strchr(s.c_str(), ',');
  if (!p) return 0;
  if (sscanf(p + 1, " %d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) != 6) return 0;
  static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int month = 0;
  for (int i = 0; i < 12; i++) {
    if (strncmp(mon, kMonths[i], 3) == 0) { month = i + 1; break; }
  }
  if (month == 0 || day < 1 || day > 31 || year < 2020 || year > 2100 ||
      hh > 23 || mm > 59 || ss > 60) {
    return 0;
  }
  return (time_t)(daysFromCivil(year, month, day) * 86400L + hh * 3600L + mm * 60L + ss);
}

// ---------------------------------------------------------------------------
// THE single writer to the STM32.
// ---------------------------------------------------------------------------
static String       s_lastWrittenValue = "";
static unsigned long s_lastWriteAt = 0;
static const long   kApplyKeepaliveMs = 3000;   // re-send same value at least this often
static const char*  s_lastLoggedSource = "";

void applyCurrentValue() {
  // The Core 0 worker owns the UART while it flashes the STM32 (stm_fota.h).
  // Nothing may be written then; the keepalive below re-sends the value within
  // kApplyKeepaliveMs once the UART is back.
  if (stmUartRequest) return;

  String out;
  bool sched = false;
  const char* source = "setting";

  // 0. Blacklist lock (absolute highest priority — server kill switch).
  //    Overrides share/schedule/setting. Re-sent on the keepalive below so a
  //    rebooted STM32 re-locks. deviceLocked/unlockPending are only touched on
  //    Core 1 (MQTT callback + here), so no mutex is needed for them.
  if (deviceLocked) {
    out = "*LOCK12345#";
    source = "lock";
  } else if (unlockPending) {
    // One-shot unlock pulse: emit *UNLOCK54321# once, then resume normal values
    // on the next cycle (out differs from this, so it writes immediately).
    unlockPending = false;
    out = "*UNLOCK54321#";
    source = "unlock";
  }

  if (out.isEmpty()) {
    // Read shared state under the mutex. Keep the critical section short: the
    // only potentially slow call inside is getLocalTime() (<=10ms).
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
      return;  // couldn't get the lock this cycle; retry in intervalApply
    }

    // 1. Share (highest) — only while fresh.
    if (activeShareValue >= 0 && (millis() - activeShareAt) < (unsigned long)shareValidMs) {
      out = buildShareValue(activeShareValue);
      source = "share";
    }
    // 2. Schedule
    if (out.isEmpty()) {
      out = currentScheduleValue();
      if (!out.isEmpty()) { sched = true; source = "schedule"; }
    }
    // 3. Base setting
    if (out.isEmpty()) {
      out = lastSetupValue;
    }
    scheduleActive = sched;

    xSemaphoreGive(stateMutex);
  }

  if (out.isEmpty()) return;   // nothing known yet

  // Source-change log — trackLog only enqueues, so this never blocks.
  if (s_lastLoggedSource != source) {
    s_lastLoggedSource = source;
    trackLog("SOURCE_CHANGE", "source=" + String(source) + " value=" + out, 300000);
  }

  unsigned long now = millis();
  if (out != s_lastWrittenValue || now - s_lastWriteAt >= kApplyKeepaliveMs) {
    testSerial.write(out.c_str());
    s_lastWrittenValue = out;
    s_lastWriteAt = now;
    DBG_PRINT("[APPLY] "); DBG_PRINTLN(out);
  }
}
