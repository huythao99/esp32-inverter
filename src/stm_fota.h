#pragma once

#include <Arduino.h>
#include <stm_fota_runner.h>   // esp32-common: runner, UART hand-over, reported version

// ---------------------------------------------------------------------------
// STM32 FOTA: flash the power board's STM32 through the existing UART link,
// using the STM32 team's protocol (gti_fota.h). The update itself is the shared
// runner (esp32-common stm_fota_runner.h); this is the inverter's glue.
// Backend contract: nestjs-app/docs/STM32_FOTA.md.
//
//   MQTT  inverter/{uid}/{id}/stm/update       server -> device (trigger)
//   HTTP  GET /api/stm-firmware?deviceId&userId  image url/size/crc32
//   MQTT  inverter/{uid}/{id}/stm/ota/status   device -> server (progress)
//   HTTP  PATCH /api/stm-firmware/info/{uid}/{id}  version after success
//
// The STM32 version comes from telemetry field 13 (main.cpp ->
// stmSetReportedVersion). First-generation boards (stmLegacy) have no FOTA
// bootloader: triggers are refused.
//
// Threading:
//   - The MQTT callback (Core 1) only parses the trigger (stmFotaOnTrigger).
//   - loop() (Core 1) enqueues the job (stmFotaLoopTick) and hands the UART
//     over while the worker flashes (stmUartRequest / stmUartReleased).
//   - The whole update runs on the Core 0 worker (stmFotaRun).
// ---------------------------------------------------------------------------

// setup(): configure the runner (UART, API paths, status queue).
void stmFotaSetup();

// MQTT trigger payload {"action":"stm_update","version","crc32","force","ts"}.
// Called from the MQTT callback (Core 1): parse only.
void stmFotaOnTrigger(const String& payload);

// Core 1 loop(): enqueue a pending trigger on the worker.
void stmFotaLoopTick();

// Core 0 worker: run one update. version may be "" (use the server's target),
// crc32 0 = unknown.
void stmFotaRun(const char* version, uint32_t crc32, bool force);
