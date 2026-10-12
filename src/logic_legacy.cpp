// ---------------------------------------------------------------------------
// STM32 link protocol: LEGACY writer (first-gen boards) + AUTO detection.
// Split out of logic.cpp; the single writer (applyCurrentValue) stays there
// and calls in here through logic_legacy.h.
// ---------------------------------------------------------------------------
#include "logic_legacy.h"
#include "shared_state.h"
#include "config.h"
#include "worker.h"    // trackLog() (enqueues; non-blocking)
#include "storage.h"   // STM legacy hint (NVS)
#include <math.h>

// ---------------------------------------------------------------------------
// LEGACY STM32 link (boards of the esp-32 firmware before 06/2025).
// What that STM32 firmware does (main.c read_data(), USART1 9600 8N1):
//  - polls PB9 (= our STM_START) in its main loop; while it is HIGH it turns
//    the DC/DC OFF, stops its control timer, raises PB8 (= our STM_READY) and
//    blocks up to 500 ms in HAL_UART_Receive() for exactly 8 bytes;
//  - "VVVVPPPP": v_set = VV.VV V, limit = PPPP - 1000 W, clamped to 0..1200 W;
//  - after a valid value it answers at once with a telemetry frame (8 fields,
//    '*'-terminated, fields 7/8 = v_set / limit), otherwise one every ~10 s;
//  - READY is LOW at idle: it only goes HIGH as an answer to START.
//  - the DC/DC comes back only ~9 s later (counter_protect > 80000 at 10 kHz
//    + 1 s in on_DCDC()), so EVERY write stops discharging for ~9 s.
// Hence: raise START only for a new value, write when READY answers, drop
// START, and space out ordinary changes (share/schedule) by kLegacyChangeMinMs.
// Lock / grid-tie OFF (and leaving them) go out at once.
// ---------------------------------------------------------------------------
static String        s_legacyValue = "";      // value requested / last written
static String        s_legacyValueOut = "";   // command s_legacyValue came from
static String        s_legacyNext = "";       // latest wanted value
static bool          s_legacyRequested = false;   // START raised, not written yet
static unsigned long s_legacyWriteAt = 0;
static unsigned long s_legacyStartAt = 0;
static uint8_t       s_legacyNoAnswer = 0;   // STARTs that got no READY in time
static const unsigned long kLegacyChangeMinMs = 60000;
static const unsigned long kLegacyStartTimeoutMs = 5000;
static const long          kLegacyMaxLimitW = 1200;   // STM32 clamp
static const char* const   kLegacyGridTieOff = "99001001";

// Legacy STM32 clamps the limit to 1200 W: send what it will really run, so
// the echo (detection) compares with the value it reports back.
String clampLegacyLimit(const String& out) {
  if (out.length() < 4 || out[0] != '*' || out.startsWith("*LOCK") ||
      out.startsWith("*UNLOCK")) {
    return out;
  }
  const int at = out.indexOf('@');
  const int hash = out.indexOf('#');
  if (at < 2 || hash <= at + 1) return out;
  const long w = out.substring(at + 1, hash).toInt() - 1000;
  if (w <= kLegacyMaxLimitW) return out;
  return out.substring(0, at + 1) + String(kLegacyMaxLimitW + 1000) + "#";
}

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

static String s_legacyOut = "";            // command s_legacyValue was made from

void stmNoteWritten(const String& out);   // echo check (below)

static bool legacyUrgent(const String& raw) {
  return raw == LEGACY_LOCK_VALUE || raw == kLegacyGridTieOff;
}

void legacyWrite(const String& out) {
  // Convert only when the command changes (no String churn every second).
  if (out != s_legacyOut) {
    s_legacyOut = out;
    s_legacyNext = toLegacyValue(out);
  }
  const unsigned long now = millis();

  // A new value: request it (START HIGH) - now if urgent / first write,
  // otherwise once kLegacyChangeMinMs has passed since the last write.
  if (!s_legacyRequested && !s_legacyNext.isEmpty() &&
      s_legacyNext != s_legacyValue &&
      (s_legacyWriteAt == 0 || legacyUrgent(s_legacyNext) ||
       legacyUrgent(s_legacyValue) ||
       now - s_legacyWriteAt >= kLegacyChangeMinMs)) {
    s_legacyValue = s_legacyNext;
    s_legacyValueOut = s_legacyOut;
    s_legacyRequested = true;
    s_legacyStartAt = now;
    digitalWrite(STM_START, HIGH);
  }
  if (!s_legacyRequested) return;

  if (digitalRead(STM_READY) != HIGH) {
    // No answer (STM32 busy, e.g. 1 s in on_DCDC(), or no legacy board):
    // pulse START again later instead of holding the DC/DC off forever.
    if (now - s_legacyStartAt >= kLegacyStartTimeoutMs) {
      digitalWrite(STM_START, LOW);
      s_legacyRequested = false;
      s_legacyValue = "";              // retried on the next cycle
      if (s_legacyNoAnswer < 255) s_legacyNoAnswer++;
    }
    return;
  }
  digitalWrite(STM_START, LOW);        // STM32 leaves its loop after these 8 bytes
  testSerial.write(s_legacyValue.c_str());
  s_legacyRequested = false;
  s_legacyWriteAt = now;
  stmNoteWritten(s_legacyValueOut);
  DBG_PRINT("[APPLY legacy] "); DBG_PRINTLN(s_legacyValue);
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
//   2. If the echo stays wrong >= kTryNewMs after the command was written
//      -> TRY_LEGACY (a legacy board ignores "*...#": it only listens while
//      STM_START is high, and its READY is low at idle, so READY cannot be
//      used to spot it beforehand).
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
// Legacy frames come every ~10 s: 3 echoes need ~30 s after the write.
static const unsigned long kTryLegacyMs    = 60000;
static const unsigned long kRetryLegacyMs  = 600000;
static const int           kEchoConfirm    = 3;
static int                 s_probeDelta    = 3;   // W, set at boot

static bool        s_protoLogPending = true;
static const char* s_protoSource = "auto";

// Called whenever a command actually goes out on the UART (both modes).
void stmNoteWritten(const String& out) {
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
  s_legacyValue = "";
  s_legacyValueOut = "";
  s_legacyNext = "";
  s_legacyOut = "";
  s_legacyRequested = false;
  s_legacyNoAnswer = 0;
  s_legacyWriteAt = 0;
  s_echoHave = false;
  s_echoOk = 0;
  digitalWrite(STM_START, LOW);
}

bool detectLegacyStm() {
  // Pin setup only; the decision is made from the STM32's echo (above).
  // Returns the hint of the previous boot (NVS): try legacy first.
  s_probeDelta = 2 + (int)(esp_random() % 3);
  // Plain INPUT, exactly like the esp-32 (Firebase) firmware. The legacy
  // STM32 drives READY (PB8, push-pull) only while answering START.
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
String stmProbeValue(const String& out) {
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
      // Echo still wrong long after the command went out -> try the
      // legacy handshake (READY answering START decides quickly).
      if (s_echoHave && s_echoOk == 0 &&
          now - s_echoCmdAt >= kTryNewMs && now - s_phaseAt >= kTryNewMs &&
          (long)(now - s_nextLegacyTryAt) >= 0) {
        s_autoPhase = AP_TRY_LEGACY;
        s_phaseAt = now;
        setLegacy(true);
        s_protoSource = "auto-try-legacy";
        s_protoLogPending = true;
      }
    } else if (now - s_phaseAt >= kTryLegacyMs ||
               (s_legacyNoAnswer >= 2 && s_legacyWriteAt == 0)) {
      // AP_TRY_LEGACY without an echo, or READY never answered two STARTs
      // (a new board does not drive READY; two tries cover an STM32 still
      // booting): not a legacy board. The READY check ends the try in ~11 s,
      // so a new board is barely disturbed; the echo still decides when
      // READY floats HIGH.
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
