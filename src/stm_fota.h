#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// STM32 FOTA: flash the power board's STM32 through the existing UART link,
// using the STM32 team's protocol (gti_fota.h). Backend contract:
// nestjs-app/docs/STM32_FOTA.md.
//
//   MQTT  inverter/{uid}/{id}/stm/update       server -> device (trigger)
//   HTTP  GET /api/stm-firmware?deviceId&userId  image url/size/crc32
//   MQTT  inverter/{uid}/{id}/stm/ota/status   device -> server (progress)
//   HTTP  PATCH /api/stm-firmware/info/{uid}/{id}  version after success
//
// Threading:
//   - The MQTT callback (Core 1) only parses the trigger (stmFotaOnTrigger).
//   - loop() (Core 1) enqueues the job (stmFotaLoopTick) and hands the UART
//     over while the worker flashes (stmUartRequest / stmUartReleased).
//   - The whole update runs on the Core 0 worker (stmFotaRun).
// ---------------------------------------------------------------------------

// UART hand-over. Core 0 sets stmUartRequest and waits for stmUartReleased;
// from then on Core 1 neither reads nor writes testSerial. Clearing
// stmUartRequest gives the UART back; Core 1 resyncs and clears stmUartReleased.
extern volatile bool stmUartRequest;
extern volatile bool stmUartReleased;

// STM32 firmware version seen in telemetry field 13 ("x.y.z"), Core 1 writes.
void   stmSetReportedVersion(const String& version);
String stmReportedVersion();            // "" if never seen
unsigned long stmReportedVersionAt();   // millis() of the last report, 0 = never

// MQTT trigger payload {"action":"stm_update","version","crc32","force","ts"}.
// Called from the MQTT callback (Core 1): parse only.
void stmFotaOnTrigger(const String& payload);

// Core 1 loop(): enqueue a pending trigger on the worker.
void stmFotaLoopTick();

// Core 0 worker: run one update. version may be "" (use the server's target),
// crc32 0 = unknown.
void stmFotaRun(const char* version, uint32_t crc32, bool force);
