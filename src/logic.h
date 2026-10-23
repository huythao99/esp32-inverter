#pragma once

#include <Arduino.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Pure decision logic + the single STM32 writer.
//
// Locking contract:
//   parseScheduleData(), currentScheduleValue() and buildShareValue() touch the
//   shared schedules[]/lastSetupValue and are NOT self-locking — the caller must
//   hold stateMutex. applyCurrentValue() takes stateMutex itself.
// ---------------------------------------------------------------------------

// "99001620" -> "*9900@1620#". Returns "" for non-8-digit input.
String convertSetupValue(const String& input);

bool isTimeInRange(const String& currentTime, const String& startTime, const String& endTime);

// Parse "#"-separated schedule string into schedules[]. Caller holds stateMutex.
void parseScheduleData(const String& scheduleData);

// Converted value of the matching window, or "". Caller holds stateMutex.
String currentScheduleValue();

// Build "*VVVV@share#": cut-off voltage of `base` (the active schedule
// slot's command) or, when base is empty, of lastSetupValue + the clamped
// share watts. Caller holds stateMutex.
String buildShareValue(int shareWatts, const String& base = String());

// THE single writer to the STM32. Picks share > schedule > setting and writes
// on change plus a keepalive. Takes stateMutex internally.
void applyCurrentValue();

// ---- STM32 link protocol (legacy writer + AUTO detection) -----------------
#include "logic_legacy.h"

// jsonEscape(): esp32-common json_util.h.
#include <json_util.h>

// SNTP sync callback (sets isNtpSynced).
void onNtpSync(struct timeval* tv);

// True once the system clock holds a real date — set either by SNTP or by the
// HTTP `Date` header fallback. Use this (not isNtpSynced) to decide whether
// time-based logic such as schedules may run.
bool isTimeValid();

// parseHttpDate(): esp32-common device_api.h (the HTTP Date clock fallback
// itself is done by deviceApiRequest, see netHttpInit()).
