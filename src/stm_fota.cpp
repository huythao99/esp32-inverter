// STM32 FOTA glue of the inverter (see stm_fota.h). The update itself is the
// shared runner in esp32-common (stm_fota_runner.cpp).
#include "stm_fota.h"
#include "shared_state.h"
#include "config.h"
#include "storage.h"
#include "worker.h"

static String targetPath() {
  return "/api/stm-firmware?deviceId=" + wifiBroadcastSSID + "&userId=" + getUid();
}

static String infoPath() {
  return "/api/stm-firmware/info/" + getUid() + "/" + wifiBroadcastSSID;
}

// Status -> MQTT stm/ota/status (published by Core 1 via otaStatusQueue).
static void publishStatus(const String& json) {
  DBG_PRINT("[STM FOTA] "); DBG_PRINTLN(json);
  if (!otaStatusQueue) return;
  OtaStatusMsg msg = {};
  msg.target = OTA_TARGET_STM;
  strncpy(msg.json, json.c_str(), sizeof(msg.json) - 1);
  xQueueSend(otaStatusQueue, &msg, 0);
}

void stmFotaSetup() {
  StmFotaConfig cfg = {};
  cfg.uart = &testSerial;
  cfg.targetPath = targetPath;
  cfg.infoPath = infoPath;
  cfg.publish = publishStatus;
  cfg.maxImage = 256 * 1024;
  cfg.fsLabel = "spiffs";             // Arduino default partition table
  cfg.stopMessage = "Stopping inverter, flashing STM32 (~40 s)";
#if DEBUG
  cfg.debug = &Serial;
#else
  cfg.debug = nullptr;
#endif
  stmFotaInit(cfg);
}

void stmFotaOnTrigger(const String& payload) {
  // First-generation boards (GPIO handshake) have no FOTA bootloader.
  if (stmLegacy) {
    trackLog("STM_FOTA_REFUSED", "legacy STM32 board (GPIO handshake): no FOTA", 60000);
    return;
  }
  StmFotaTrigger t;
  if (!stmFotaParseTrigger(payload, t)) {
    trackLog("STM_FOTA_STALE", "Ignored stm/update (stale or not a trigger)", 60000);
    return;
  }
  stmFotaSetPending(t);
}

void stmFotaLoopTick() {
  StmFotaTrigger t;
  if (!stmFotaTakePending(t)) return;

  // One update at a time: a repeated trigger while one runs is dropped (the
  // running job already reports its progress on stm/ota/status).
  if (!stmFotaTryBegin()) {
    trackLog("STM_FOTA_DUP", "stm/update ignored: update already running", 60000);
    return;
  }
  if (!requestStmOta(t.version, t.crc32, t.force)) {
    stmFotaAbortBegin();
    stmFotaStatus("failed", -1, "Device busy, try again later");
  }
}

void stmFotaRun(const char* version, uint32_t crc32, bool force) {
  StmFotaTrigger t = {};
  strncpy(t.version, version ? version : "", sizeof(t.version) - 1);
  t.crc32 = crc32;
  t.force = force;
  otaInProgress = true;          // blocks remote restart meanwhile
  stmFotaRunUpdate(t);
  otaInProgress = false;
}
