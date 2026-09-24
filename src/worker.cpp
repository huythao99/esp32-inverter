#include "worker.h"
#include "shared_state.h"
#include "config.h"
#include "storage.h"
#include "net_http.h"

static TaskHandle_t s_workerTask = nullptr;

// Push a job without blocking Core 1. Drop if the queue is full (jobs are either
// idempotent re-fetches or best-effort logs).
static void enqueue(const Job& job) {
  if (jobQueue) {
    xQueueSend(jobQueue, &job, 0);
  }
}

void requestFetchSettings() { Job j = {}; j.type = JOB_FETCH_SETTINGS; enqueue(j); }
void requestFetchSchedule() { Job j = {}; j.type = JOB_FETCH_SCHEDULE; enqueue(j); }
void requestRegister()      { Job j = {}; j.type = JOB_REGISTER;       enqueue(j); }
void requestUpdateVersion() { Job j = {}; j.type = JOB_UPDATE_VERSION; enqueue(j); }
void requestOTA()           { Job j = {}; j.type = JOB_OTA;            enqueue(j); }

bool trackLog(const String& code, const String& message, unsigned long cooldownMs) {
  Job j = {};
  j.type = JOB_LOG;
  strncpy(j.code, code.c_str(), sizeof(j.code) - 1);
  strncpy(j.message, message.c_str(), sizeof(j.message) - 1);
  j.cooldownMs = cooldownMs;
  enqueue(j);
  return true;
}

static void workerTask(void* /*arg*/) {
  Job job;
  for (;;) {
    if (xQueueReceive(jobQueue, &job, portMAX_DELAY) != pdTRUE) continue;

    switch (job.type) {
      case JOB_FETCH_SETTINGS:
        getDeviceSettings(getUid(), wifiBroadcastSSID);
        break;

      case JOB_FETCH_SCHEDULE:
        getScheduleSettings(getUid(), wifiBroadcastSSID);
        break;

      case JOB_REGISTER: {
        String currentUid = getUid();
        if (!currentUid.isEmpty()) {
          if (!registerDevice(wifiBroadcastSSID, wifiBroadcastSSID, currentUid)) {
            trackLogRL("DEVICE_REGISTER_FAILED", "Failed to register device via API");
          }
        }
        break;
      }

      case JOB_UPDATE_VERSION:
        if (updateFirmwareVersion(currentFirmwareVersion)) {
          isUpdateVersion = true;
        }
        break;

      case JOB_OTA:
        otaInProgress = true;    // blocks remote restart while flashing
        handleFirmwareUpdate();  // may ESP.restart() on success
        otaInProgress = false;
        break;

      case JOB_LOG:
        trackLogRL(String(job.code), String(job.message), job.cooldownMs);
        break;
    }
  }
}

void workerInit() {
  stateMutex     = xSemaphoreCreateMutex();
  jobQueue       = xQueueCreate(12, sizeof(Job));
  otaStatusQueue = xQueueCreate(8,  sizeof(OtaStatusMsg));

  // Large stack: TLS (WiFiClientSecure) + OTA need plenty of headroom.
  xTaskCreatePinnedToCore(
    workerTask,       // task function
    "http_worker",    // name
    16384,            // stack bytes
    nullptr,          // arg
    1,                // priority (below the Arduino loop task)
    &s_workerTask,    // handle
    0                 // Core 0
  );
}
