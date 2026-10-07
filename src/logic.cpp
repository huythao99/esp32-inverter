#include "logic.h"
#include "stm_fota.h"
#include "shared_state.h"
#include "config.h"
#include "worker.h"   // trackLog() (enqueues; non-blocking)
#include "time.h"
#include "storage.h"   // STM legacy hint (NVS)
#include <math.h>

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

// ---------------------------------------------------------------------------
// LEGACY STM32 link (boards of the esp-32 firmware before 06/2025).
// Same handshake as that firmware: a new value raises STM_START; whenever the
// STM32 holds STM_READY high, STM_START goes low and the raw 8-digit value is
// written (at most once a second, as before).
// ---------------------------------------------------------------------------
static String        s_legacyValue = "";
static unsigned long s_legacyWriteAt = 0;
static const unsigned long kLegacyWriteMinMs = 1000;

String toLegacyValue(const String& out) {
  if (out.startsWith("*LOCK")) return LEGACY_LOCK_VALUE;
  if (out.startsWith("*UNLOCK")) return "";
  const int at = out.indexOf('@');
  const int hash = out.indexOf('#');
  if (out.length() < 4 || out[0] != '*' || at < 2 || hash <= at + 1) return "";
  const long v = out.substring(1, at).toInt();
  const long p = out.substring(at + 1, hash).toInt();
  if (v < 0 || v > 9999 || p < 0 || p > 9999) return "";
  char buf[12];
  snprintf(buf, sizeof(buf), "%04ld%04ld", v, p);
  return String(buf);
}

static void noteWritten(const String& out);   // echo check (below)

static String s_legacyOut = "";            // command s_legacyValue was made from

static void legacyWrite(const String& out) {
  // Convert only when the command changes (no String churn every second).
  if (out != s_legacyOut) {
    s_legacyOut = out;
    const String raw = toLegacyValue(out);
    if (raw != s_legacyValue) {
      s_legacyValue = raw;
      if (!raw.isEmpty()) digitalWrite(STM_START, HIGH);   // "new value"
    }
  }
  const String& raw = s_legacyValue;
  if (raw.isEmpty()) return;
  const unsigned long now = millis();
  if (now - s_legacyWriteAt < kLegacyWriteMinMs) return;
  if (digitalRead(STM_READY) != HIGH) return;   // STM32 not ready yet
  digitalWrite(STM_START, LOW);
  testSerial.write(raw.c_str());
  s_legacyWriteAt = now;
  noteWritten(out);
  DBG_PRINT("[APPLY legacy] "); DBG_PRINTLN(raw);
}

// ---------------------------------------------------------------------------
// AUTO detection, confirmed by the STM32's own echo.
//
// The STM32 reports the cut-off / limit it is running in frame fields 7 and 8
// ("...#48.50#1600.00"). Seeing the current value there proves nothing (the
// STM32 may still hold it from before, e.g. written over the other protocol),
// so while detecting, the limit sent is offset by a few watts (s_probeDelta,
// random 2..4 W) and only that probe value coming back counts:
//   1. TRY_NEW    : always start with "*VVVV@PPPP#". 3 frames echoing the
//                   command -> CONFIRMED new.
//   2. If the echo stays wrong >= kTryNewMs after the command was written AND
//      STM_READY is high (a legacy board waits for data) -> TRY_LEGACY.
//   3. TRY_LEGACY : 3 echoing frames -> CONFIRMED legacy (hint kept in NVS so
//                   the next boot tries legacy first). No echo within
//                   kTryLegacyMs -> back to TRY_NEW, next legacy try only
//                   after kRetryLegacyMs.
// Forced modes (CMS) skip all of this.
// ---------------------------------------------------------------------------
enum AutoPhase : uint8_t { AP_TRY_NEW, AP_TRY_LEGACY, AP_CONFIRMED };
static AutoPhase     s_autoPhase = AP_TRY_NEW;
static unsigned long s_phaseAt = 0;           // phase start
static unsigned long s_nextLegacyTryAt = 0;   // backoff after a failed legacy try
static int           s_echoOk = 0;
// Last *VVVV@PPPP# written to the STM32, kept as numbers (no heap use):
// cut-off in 1/100 V, limit in W. s_echoHave = false: nothing to compare
// (nothing written yet, or a lock/unlock command).
static bool          s_echoHave = false;
static long          s_echoCut = 0;
static long          s_echoLim = 0;
static unsigned long s_echoCmdAt = 0;         // when it was first written
static const unsigned long kEchoSettleMs   = 4000;
static const unsigned long kTryNewMs       = 20000;
static const unsigned long kTryLegacyMs    = 40000;
static const unsigned long kRetryLegacyMs  = 600000;
static const int           kEchoConfirm    = 3;
static int                 s_probeDelta    = 3;   // W, set at boot

static bool        s_protoLogPending = true;
static const char* s_protoSource = "auto";

// Called whenever a command actually goes out on the UART (both modes).
static void noteWritten(const String& out) {
  long cut = 0, lim = 0;
  bool have = false;
  const char* c = out.c_str();
  if (c[0] == '*' && c[1] >= '0' && c[1] <= '9') {   // not *LOCK / *UNLOCK
    char* end = nullptr;
    cut = strtol(c + 1, &end, 10);
    if (end && *end == '@') {
      lim = strtol(end + 1, &end, 10) - 1000;
      have = end && *end == '#';
    }
  }
  if (have != s_echoHave || cut != s_echoCut || lim != s_echoLim) {
    s_echoHave = have;
    s_echoCut = cut;
    s_echoLim = lim;
    s_echoCmdAt = millis();
    s_echoOk = 0;
  }
}

static void setLegacy(bool legacy) {
  if (legacy == stmLegacy) return;
  stmLegacy = legacy;
  s_lastWrittenValue = "";             // re-send in the new format at once
  s_legacyValue = "";
  s_legacyOut = "";
  s_legacyWriteAt = 0;
  s_echoHave = false;
  s_echoOk = 0;
  digitalWrite(STM_START, LOW);
}

bool detectLegacyStm() {
  // Pin setup only; the decision is made from the STM32's echo (above).
  // Returns the hint of the previous boot (NVS): try legacy first.
  s_probeDelta = 2 + (int)(esp_random() % 3);
  // Plain INPUT, exactly like the esp-32 (Firebase) firmware: a first-gen
  // STM32 that drives READY weakly is not pulled low. A floating pin reading
  // HIGH by noise only costs a 40 s legacy try (the echo decides).
  pinMode(STM_READY, INPUT);
  pinMode(STM_START, OUTPUT);
  digitalWrite(STM_START, LOW);
  return loadStmLegacyHint();
}

void updateStmProtocol(const char* source) {
  if (stmProtoSetting == STM_PROTO_LEGACY) {
    setLegacy(true);
  } else if (stmProtoSetting == STM_PROTO_NEW) {
    setLegacy(false);
  } else {
    // AUTO: (re)start detection; a legacy hint from the last boot goes first.
    s_autoPhase = stmLegacyDetected ? AP_TRY_LEGACY : AP_TRY_NEW;
    s_phaseAt = millis();
    setLegacy(stmLegacyDetected);
  }
  s_protoSource = source;
  s_protoLogPending = true;
}

// While AUTO is still detecting: the command with its limit moved by
// s_probeDelta W (down, or up for a limit <= 10 W such as grid-tie OFF), so
// its echo cannot be a value the STM32 already had. Lock/unlock untouched.
static String probeValue(const String& out) {
  if (stmProtoSetting != STM_PROTO_AUTO || s_autoPhase == AP_CONFIRMED) return out;
  if (out.length() < 4 || out[0] != '*' ||
      out.startsWith("*LOCK") || out.startsWith("*UNLOCK")) {
    return out;
  }
  const int at = out.indexOf('@');
  const int hash = out.indexOf('#');
  if (at < 2 || hash <= at + 1) return out;
  const long w = out.substring(at + 1, hash).toInt() - 1000;
  const long pw = w > 10 ? w - s_probeDelta : w + s_probeDelta;
  return out.substring(0, at + 1) + String(pw + 1000) + "#";
}

// "a#b#c#d#e#f#CUTOFF#LIMIT[#...]" -> did the STM32 take the last command?
// Parsed in place (strtod on the frame buffer): no heap allocation per frame.
static int echoMatches(const String& frame) {
  if (!s_echoHave) return -1;           // nothing to compare against
  const char* p = frame.c_str();
  if (*p == '$') p++;
  for (int idx = 0; idx < 6; idx++) {   // skip fields 1..6
    p = strchr(p, '#');
    if (!p) return -1;
    p++;
  }
  char* end = nullptr;
  const double cut = strtod(p, &end);
  if (end == p || *end != '#') return -1;
  p = end + 1;
  const double lim = strtod(p, &end);
  if (end == p) return -1;
  // Exact watt: the probe differs from the real value by only 2..4 W.
  return (fabs(cut - s_echoCut / 100.0) < 0.06 && fabs(lim - s_echoLim) < 0.6)
             ? 1 : 0;
}

void stmProtoOnFrame(const String& frame) {
  if (stmProtoSetting != STM_PROTO_AUTO || s_autoPhase == AP_CONFIRMED) return;
  if (!s_echoHave || millis() - s_echoCmdAt < kEchoSettleMs) return;
  const int m = echoMatches(frame);
  if (m < 0) return;
  if (m == 0) { s_echoOk = 0; return; }
  if (++s_echoOk < kEchoConfirm) return;

  const bool legacy = s_autoPhase == AP_TRY_LEGACY;
  s_autoPhase = AP_CONFIRMED;
  if (legacy != stmLegacyDetected) {
    stmLegacyDetected = legacy;
    saveStmLegacyHint(legacy);
  }
  s_protoSource = legacy ? "auto-legacy" : "auto-new";
  s_protoLogPending = true;
}

void stmProtocolTick(unsigned long now) {
  if (stmProtoSetting == STM_PROTO_AUTO && s_autoPhase != AP_CONFIRMED) {
    if (s_autoPhase == AP_TRY_NEW) {
      // Echo still wrong long after the command went out + a legacy board's
      // READY line is up -> try the handshake.
      if (s_echoHave && s_echoOk == 0 &&
          now - s_echoCmdAt >= kTryNewMs && now - s_phaseAt >= kTryNewMs &&
          (long)(now - s_nextLegacyTryAt) >= 0 &&
          digitalRead(STM_READY) == HIGH) {
        s_autoPhase = AP_TRY_LEGACY;
        s_phaseAt = now;
        setLegacy(true);
        s_protoSource = "auto-try-legacy";
        s_protoLogPending = true;
      }
    } else if (now - s_phaseAt >= kTryLegacyMs) {
      // AP_TRY_LEGACY without an echo: not a legacy board after all.
      s_autoPhase = AP_TRY_NEW;
      s_phaseAt = now;
      s_nextLegacyTryAt = now + kRetryLegacyMs;
      setLegacy(false);
      if (stmLegacyDetected) {
        stmLegacyDetected = false;
        saveStmLegacyHint(false);
      }
          s_protoSource = "auto-revert-new";
      s_protoLogPending = true;
    }
  }
  if (s_protoLogPending && isMqttConnected) {
    s_protoLogPending = false;
    const char* detected =
        s_autoPhase == AP_CONFIRMED ? (stmLegacyDetected ? "legacy" : "new")
                                    : "unknown";
    trackLog("STM_PROTOCOL",
             String("mode=") + (stmLegacy ? "legacy" : "new") +
                 " src=" + s_protoSource +
                 " detected=" + detected +
                 " setting=" + String((int)stmProtoSetting),
             0);
  }
}

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
  } else if (gridTieOff) {
    // 0b. Grid-tie OFF (cmd/grid-tie): above share/schedule/setting, so
    //     neither a share group nor a schedule window can feed the grid.
    out = GRID_TIE_OFF_OUT;
    source = "gridtie";
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

  out = probeValue(out);   // AUTO still detecting: limit offset by a few W

  if (stmLegacy) {
    legacyWrite(out);
    return;
  }

  unsigned long now = millis();
  if (out != s_lastWrittenValue || now - s_lastWriteAt >= kApplyKeepaliveMs) {
    testSerial.write(out.c_str());
    noteWritten(out);
    s_lastWrittenValue = out;
    s_lastWriteAt = now;
    DBG_PRINT("[APPLY] "); DBG_PRINTLN(out);
  }
}
