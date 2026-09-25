#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Core 0 worker: owns all blocking HTTP/OTA. Core 1 talks to it only through the
// enqueue helpers below (all non-blocking; drop silently if the queue is full).
// ---------------------------------------------------------------------------

// Create the queues/mutex and pin the worker task to Core 0. Call from setup().
void workerInit();

// Enqueue requests (safe from Core 1 / callbacks).
void requestFetchSettings();
void requestFetchSchedule();
void requestRegister();
void requestUpdateVersion();
void requestOTA();
// Flash the STM32 (see stm_fota.h). false if the job queue is full.
bool requestStmOta(const char* version, uint32_t crc32, bool force);

// Rate-limited error log — enqueues a JOB_LOG so the HTTP POST runs on Core 0.
// Safe to call from any core / the MQTT callback.
bool trackLog(const String& code, const String& message, unsigned long cooldownMs = 60000);
