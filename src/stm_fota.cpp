#include "stm_fota.h"
#include "shared_state.h"
#include "config.h"
#include "storage.h"
#include "logic.h"
#include "worker.h"
#include "gti_fota.h"
#include "ca_certs.h"
#include <LittleFS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------
volatile bool stmUartRequest  = false;
volatile bool stmUartReleased = false;

static portMUX_TYPE  s_verMux = portMUX_INITIALIZER_UNLOCKED;
static char          s_version[16] = "";
static unsigned long s_versionAt = 0;

void stmSetReportedVersion(const String& version) {
  const unsigned int n = version.length();
  if (n == 0 || n >= sizeof(s_version)) return;
  unsigned long now = millis();
  if (now == 0) now = 1;
  portENTER_CRITICAL(&s_verMux);
  memcpy(s_version, version.c_str(), n + 1);
  s_versionAt = now;
  portEXIT_CRITICAL(&s_verMux);
}

String stmReportedVersion() {
  char buf[sizeof(s_version)];
  portENTER_CRITICAL(&s_verMux);
  memcpy(buf, s_version, sizeof(buf));
  portEXIT_CRITICAL(&s_verMux);
  return String(buf);
}

unsigned long stmReportedVersionAt() {
  portENTER_CRITICAL(&s_verMux);
  unsigned long at = s_versionAt;
  portEXIT_CRITICAL(&s_verMux);
  return at;
}

// ---------------------------------------------------------------------------
// Status -> MQTT stm/ota/status (published by Core 1 via otaStatusQueue)
// ---------------------------------------------------------------------------
static void publishStmStatus(const char* status, int progress, const String& message,
                             const String& version = "") {
  String json = "{\"status\":\"";
  json += status;
  json += "\"";
  if (progress >= 0) json += ",\"progress\":" + String(progress);
  if (!message.isEmpty()) json += ",\"message\":\"" + jsonEscape(message) + "\"";
  if (!version.isEmpty()) json += ",\"version\":\"" + jsonEscape(version) + "\"";
  json += "}";

  DBG_PRINT("[STM FOTA] "); DBG_PRINTLN(json);

  if (otaStatusQueue) {
    OtaStatusMsg msg = {};
    msg.target = OTA_TARGET_STM;
    strncpy(msg.json, json.c_str(), sizeof(msg.json) - 1);
    xQueueSend(otaStatusQueue, &msg, 0);
  }
}

// ---------------------------------------------------------------------------
// Trigger (Core 1)
// ---------------------------------------------------------------------------
static bool          s_trigPending = false;          // Core 1 only
static char          s_trigVersion[16] = "";
static uint32_t      s_trigCrc = 0;
static bool          s_trigForce = false;
static volatile bool s_jobActive = false;            // queued or running

static uint32_t parseCrc(JsonVariantConst v) {
  if (v.is<uint32_t>()) return v.as<uint32_t>();
  const char* s = v.as<const char*>();
  if (!s || !*s) return 0;
  return (uint32_t)strtoul(s, nullptr, 16);          // accepts "0x..." and bare hex
}

void stmFotaOnTrigger(const String& payload) {
  JsonDocument doc;
  if (deserializeJson(doc, payload) != DeserializationError::Ok) return;

  const char* action = doc["action"] | "stm_update";
  if (strcmp(action, "stm_update") != 0) return;

  // Ignore stale triggers (only checkable once the clock is set).
  if (isTimeValid() && doc["ts"].is<double>()) {
    double ageMs = (double)time(nullptr) * 1000.0 - doc["ts"].as<double>();
    if (ageMs > 120000.0) {
      trackLog("STM_FOTA_STALE", "Ignored stale stm/update trigger", 60000);
      return;
    }
  }

  const char* v = doc["version"] | "";
  strncpy(s_trigVersion, v, sizeof(s_trigVersion) - 1);
  s_trigVersion[sizeof(s_trigVersion) - 1] = '\0';
  s_trigCrc = parseCrc(doc["crc32"]);
  s_trigForce = doc["force"] | false;
  s_trigPending = true;
}

void stmFotaLoopTick() {
  if (!s_trigPending) return;
  s_trigPending = false;

  // One update at a time: a repeated trigger while one runs is dropped (the
  // running job already reports its progress on stm/ota/status).
  if (s_jobActive) {
    trackLog("STM_FOTA_DUP", "stm/update ignored: update already running", 60000);
    return;
  }
  s_jobActive = true;
  if (!requestStmOta(s_trigVersion, s_trigCrc, s_trigForce)) {
    s_jobActive = false;
    publishStmStatus("failed", -1, "Device busy, try again later");
  }
}

// ---------------------------------------------------------------------------
// Worker helpers (Core 0)
// ---------------------------------------------------------------------------
static const char*    kNewImage  = "/stm_new.bin";   // downloaded, not flashed yet
static const char*    kCurImage  = "/stm_cur.bin";   // last image flashed OK
static const char*    kPrevImage = "/stm_prev.bin";  // the one before (way back)
static const uint32_t kMaxImage  = 256 * 1024;

struct StmTarget {
  String   version;
  String   url;
  uint32_t size = 0;
  uint32_t crc  = 0;
};

// "a.b.c" -> numbers. false if it isn't exactly three numbers.
static bool parseVersion(const String& v, int& major, int& mid, int& patch) {
  char tail;
  return sscanf(v.c_str(), "%d.%d.%d%c", &major, &mid, &patch, &tail) == 3 &&
         major >= 0 && mid >= 0 && patch >= 0;
}

// zlib / PNG CRC32 (same as the backend and the STM32 bootloader). Start at 0.
static uint32_t crc32Update(uint32_t crc, const uint8_t* p, size_t n) {
  crc = ~crc;
  while (n--) {
    crc ^= *p++;
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

static bool crc32File(const char* path, uint32_t& crc, uint32_t& size) {
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  uint8_t buf[256];
  crc = 0;
  size = f.size();
  int n;
  while ((n = f.read(buf, sizeof(buf))) > 0) crc = crc32Update(crc, buf, n);
  f.close();
  return true;
}

// Version stored in the image tail ("GTIV" ... fw_version 0x00MMmmpp).
// "" when the image has no tail / no version.
static String imageTailVersion(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) return "";
  const uint32_t size = f.size();
  uint8_t t[GTI_FOTA_TAIL_SIZE];
  bool ok = size >= GTI_FOTA_TAIL_SIZE && f.seek(size - GTI_FOTA_TAIL_SIZE) &&
            f.read(t, GTI_FOTA_TAIL_SIZE) == GTI_FOTA_TAIL_SIZE;
  f.close();
  if (!ok) return "";
  auto get32 = [&](int i) -> uint32_t {
    return (uint32_t)t[i] | ((uint32_t)t[i + 1] << 8) |
           ((uint32_t)t[i + 2] << 16) | ((uint32_t)t[i + 3] << 24);
  };
  const uint32_t w0 = get32(0), w1 = get32(4), w2 = get32(8), chk = get32(12);
  if (w0 != GTI_FOTA_TAIL_MAGIC || chk != ~(w0 + w1 + w2) || w2 == 0xFFFFFFFFu) return "";
  return String((w2 >> 16) & 0xFF) + "." + String((w2 >> 8) & 0xFF) + "." + String(w2 & 0xFF);
}

static bool s_fsMounted = false;
static bool mountFs() {
  // The default partition table's "spiffs" partition (1.4 MB) is otherwise
  // unused; formatted as LittleFS on first use.
  if (!s_fsMounted) s_fsMounted = LittleFS.begin(true, "/littlefs", 4, "spiffs");
  return s_fsMounted;
}

static String deviceQuery() {
  return "deviceId=" + wifiBroadcastSSID + "&userId=" + getUid();
}

// GET /api/stm-firmware -> target image. Returns the HTTP code (<0 = no connection).
static int fetchTarget(StmTarget& t) {
  WiFiClientSecure client;
  client.setCACert(kRootCA);   // verify the server (ca_certs.h)
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(10000);
  if (!http.begin(client, String(API_BASE) + "/api/stm-firmware?" + deviceQuery())) return -1;

  const int code = http.GET();
  if (code == 200) {
    JsonDocument doc;
    if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
      t.version = doc["version"] | "";
      t.url     = doc["url"] | "";
      t.size    = doc["size"] | 0;
      t.crc     = parseCrc(doc["crc32"]);
    }
  }
  http.end();
  return code;
}

// Download t.url to kNewImage, checking size and CRC32 on the way.
static bool downloadImage(const StmTarget& t, String& err) {
  WiFiClientSecure client;
  client.setCACert(kRootCA);   // verify the server (ca_certs.h)
  client.setHandshakeTimeout(30);
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, t.url)) { err = "Bad firmware URL"; return false; }

  const int code = http.GET();
  if (code != 200) {
    http.end();
    err = "Download failed (HTTP " + String(code) + ")";
    return false;
  }
  const int len = http.getSize();
  if (len > 0 && (uint32_t)len != t.size) {
    http.end();
    err = "Size mismatch (" + String(len) + " != " + String(t.size) + ")";
    return false;
  }

  File f = LittleFS.open(kNewImage, "w");
  if (!f) { http.end(); err = "Storage write failed"; return false; }

  WiFiClient* s = http.getStreamPtr();
  uint8_t buf[512];
  uint32_t got = 0, crc = 0;
  int lastPct = 0;
  unsigned long lastData = millis();
  bool writeErr = false;

  while (got < t.size) {
    size_t avail = s->available();
    if (avail == 0) {
      if (!http.connected() || millis() - lastData > 15000) break;
      delay(5);
      continue;
    }
    size_t want = t.size - got;
    if (want > sizeof(buf)) want = sizeof(buf);
    if (want > avail) want = avail;
    const size_t n = s->readBytes(buf, want);
    if (n == 0) continue;
    if (f.write(buf, n) != n) { writeErr = true; break; }
    crc = crc32Update(crc, buf, n);
    got += n;
    lastData = millis();
    const int pct = (int)((uint64_t)got * 100 / t.size);
    if (pct >= lastPct + 20 || got == t.size) {
      lastPct = pct;
      publishStmStatus("downloading", pct, "Downloading " + t.version);
    }
  }
  f.close();
  http.end();

  if (writeErr)        { err = "Storage write failed"; return false; }
  if (got != t.size)   { err = "Download incomplete (" + String(got) + "/" + String(t.size) + ")"; return false; }
  if (crc != t.crc) {
    char m[64];
    snprintf(m, sizeof(m), "CRC32 mismatch (0x%08X != 0x%08X)", (unsigned)crc, (unsigned)t.crc);
    err = m;
    return false;
  }
  return true;
}

// PATCH /api/stm-firmware/info/{uid}/{id} after a successful update.
static void reportVersion(const String& version, uint32_t crc) {
  WiFiClientSecure client;
  client.setCACert(kRootCA);   // verify the server (ca_certs.h)
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(10000);
  const String url = String(API_BASE) + "/api/stm-firmware/info/" + getUid() + "/" + wifiBroadcastSSID;
  if (!http.begin(client, url)) return;
  http.addHeader("Content-Type", "application/json");
  char body[80];
  snprintf(body, sizeof(body), "{\"version\":\"%s\",\"crc32\":\"0x%08X\"}", version.c_str(), (unsigned)crc);
  http.sendRequest("PATCH", String(body));
  http.end();
}

// Take the UART from Core 1 (it stops reading telemetry and writing values).
static bool acquireUart() {
  stmUartRequest = true;
  const unsigned long t0 = millis();
  while (!stmUartReleased) {
    if (millis() - t0 > 15000) { stmUartRequest = false; return false; }
    delay(10);
  }
  return true;
}

static void releaseUart() { stmUartRequest = false; }

static const char* errName(GtiFotaResult r) {
  switch (r) {
    case GTI_FOTA_OK:                return "ok";
    case GTI_FOTA_ERR_NO_ACK:        return "no_ack";
    case GTI_FOTA_ERR_BUSY:          return "busy";
    case GTI_FOTA_ERR_NO_BOOTLOADER: return "no_bootloader";
    case GTI_FOTA_ERR_BEGIN:         return "begin";
    case GTI_FOTA_ERR_DATA:          return "data";
    case GTI_FOTA_ERR_END_CRC:       return "end_crc";
    case GTI_FOTA_ERR_READ:          return "read";
    case GTI_FOTA_ERR_VARIANT:       return "variant";
    case GTI_FOTA_ERR_PRODUCT:       return "product";
  }
  return "unknown";
}

// Errors after which the STM32 app may already be erased (the STM32 then waits
// in its bootloader; update() can flash it again without rescue()).
static bool mayBeErased(GtiFotaResult r) {
  return r == GTI_FOTA_ERR_BEGIN || r == GTI_FOTA_ERR_DATA ||
         r == GTI_FOTA_ERR_END_CRC || r == GTI_FOTA_ERR_READ;
}

static GtiFota s_fota(testSerial);

// One update() pass with progress on stm/ota/status. The UART must be held.
static GtiFotaResult flashFile(const char* path, uint32_t crc) {
  File f = LittleFS.open(path, "r");
  if (!f) return GTI_FOTA_ERR_READ;
  const uint32_t size = f.size();
  bool started = false;   // update() reads the 16-byte tail first: skip that
  int lastPct = -1;

#if DEBUG
  s_fota.setLog(&Serial);
#endif
  GtiFotaResult r = s_fota.update(size, crc,
    [&](uint32_t off, uint8_t* buf, uint32_t n) -> bool {
      if (off == 0) started = true;
      if (started) {
        int pct = (int)((uint64_t)(off + n) * 100 / size);
        if (pct > 99) pct = 99;               // 100 only once END is accepted
        if (pct >= lastPct + 10) {
          lastPct = pct;
          publishStmStatus("flashing", pct, "Flashing STM32");
        }
      }
      return f.seek(off) && (uint32_t)f.read(buf, n) == n;
    });
  f.close();
  return r;
}

static void fail(const char* status, const String& message) {
  publishStmStatus(status, -1, message);
  trackLog("STM_FOTA_FAILED", message, 60000);
}

static void runUpdate(const char* trigVersion, uint32_t trigCrc, bool force) {
  publishStmStatus("starting", 0, "Checking STM32 firmware");

  if (WiFi.status() != WL_CONNECTED) { fail("failed", "No WiFi"); return; }

  // 1. What should be flashed (server is the source of truth).
  StmTarget t;
  const int code = fetchTarget(t);
  if (code == 404) { fail("failed", "No STM32 firmware for this board"); return; }
  int tMajor, tMid, tPatch;
  if (code != 200 || t.url.isEmpty() || t.size == 0 ||
      !parseVersion(t.version, tMajor, tMid, tPatch)) {
    fail("failed", "Cannot get STM32 firmware info (HTTP " + String(code) + ")");
    return;
  }
  if (t.size > kMaxImage) { fail("failed", "Image too large"); return; }
  if (trigCrc != 0 && t.version == trigVersion && trigCrc != t.crc) {
    fail("failed", "CRC32 differs between trigger and server");
    return;
  }

  // 2. Same chip (major) and voltage (middle number) as the running board.
  const String before = stmReportedVersion();
  int bMajor, bMid, bPatch;
  if (before.isEmpty() || !parseVersion(before, bMajor, bMid, bPatch)) {
    if (!force) { fail("failed", "STM32 version unknown (no telemetry field 13)"); return; }
  } else {
    if (bMajor != tMajor || bMid != tMid) {
      fail("failed", "Image " + t.version + " does not match board " + before);
      return;
    }
    if (before == t.version && !force) {
      publishStmStatus("success", 100, "Already up to date", before);
      return;
    }
  }

  // 3. Download + verify BEFORE touching the STM32.
  if (!mountFs()) { fail("failed", "Storage unavailable"); return; }
  LittleFS.remove(kNewImage);
  publishStmStatus("downloading", 0, "Downloading " + t.version);
  String err;
  if (!downloadImage(t, err)) {
    LittleFS.remove(kNewImage);
    fail("failed", err);
    return;
  }
  publishStmStatus("verifying", 100, "Image OK");
  const String tailVer = imageTailVersion(kNewImage);
  if (!tailVer.isEmpty() && tailVer != t.version) {
    LittleFS.remove(kNewImage);
    fail("failed", "Image says " + tailVer + ", expected " + t.version);
    return;
  }

  // 4. Flash. Nothing else may use the UART meanwhile.
  if (!acquireUart()) {
    LittleFS.remove(kNewImage);
    fail("failed", "STM32 UART busy");
    return;
  }
  publishStmStatus("flashing", 0, "Stopping inverter, flashing STM32 (~40 s)");

  GtiFotaResult r = GTI_FOTA_ERR_NO_ACK;
  int attempts = 0;
  while (attempts < 3) {
    attempts++;
    r = flashFile(kNewImage, t.crc);
    if (r == GTI_FOTA_OK || r == GTI_FOTA_ERR_VARIANT ||
        r == GTI_FOTA_ERR_PRODUCT || r == GTI_FOTA_ERR_NO_ACK) break;
    delay(r == GTI_FOTA_ERR_BUSY ? 5000 : 1000);
  }

  // Still failing with the app possibly erased: put back the image the board
  // ran before, if we have exactly that one.
  bool restored = false;
  if (r != GTI_FOTA_OK && mayBeErased(r) && !before.isEmpty() &&
      LittleFS.exists(kCurImage) && imageTailVersion(kCurImage) == before) {
    uint32_t crc = 0, size = 0;
    if (crc32File(kCurImage, crc, size)) {
      publishStmStatus("flashing", 0, "Update failed, restoring " + before);
      restored = flashFile(kCurImage, crc) == GTI_FOTA_OK;
    }
  }
  releaseUart();

  if (r != GTI_FOTA_OK) {
    LittleFS.remove(kNewImage);
    String msg = String("STM32 flash failed: ") + errName(r) + " (" + String(attempts) + "x)";
    if (r == GTI_FOTA_ERR_VARIANT) {
      char m[40];
      snprintf(m, sizeof(m), ", image %02X board %02X", s_fota.imageVariant(), s_fota.boardVariant());
      msg += m;
    }
    if (restored) {
      fail("failed", msg + ", restored " + before);
    } else if (mayBeErased(r)) {
      fail("rescue_needed", msg + ", STM32 waits in bootloader - retry update");
    } else {
      fail("failed", msg);
    }
    return;
  }

  // 5. Keep the new image (and the previous one as the way back).
  LittleFS.remove(kPrevImage);
  if (LittleFS.exists(kCurImage)) LittleFS.rename(kCurImage, kPrevImage);
  LittleFS.rename(kNewImage, kCurImage);

  // 6. Wait for the new app to report its version in telemetry (field 13).
  const unsigned long t0 = millis();
  String now;
  while (millis() - t0 < 30000) {
    if (stmReportedVersionAt() - t0 < 0x80000000UL && stmReportedVersionAt() != 0) {
      now = stmReportedVersion();
      if (now == t.version) break;
    }
    delay(250);
  }
  reportVersion(t.version, t.crc);
  if (now == t.version) {
    publishStmStatus("success", 100, "STM32 updated to " + t.version, t.version);
  } else {
    publishStmStatus("success", 100, "Flashed " + t.version + ", waiting for STM32 to report", t.version);
  }
  trackLog("STM_FOTA_OK", (before.isEmpty() ? String("?") : before) + " -> " + t.version, 0);
}

void stmFotaRun(const char* version, uint32_t crc32, bool force) {
  otaInProgress = true;          // blocks remote restart meanwhile
  runUpdate(version, crc32, force);
  otaInProgress = false;
  s_jobActive = false;
}
