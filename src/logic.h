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

// Build "*pset@value#" from lastSetupValue's cap + a clamped share. Caller holds stateMutex.
String buildShareValue(int shareWatts);

// THE single writer to the STM32. Picks share > schedule > setting and writes
// on change plus a keepalive. Takes stateMutex internally.
void applyCurrentValue();

// JSON helpers.
String jsonEscape(const String& in);
String createSignedMessage(const String& payload);

// SNTP sync callback (sets isNtpSynced).
void onNtpSync(struct timeval* tv);

// True once the system clock holds a real date — set either by SNTP or by the
// HTTP `Date` header fallback. Use this (not isNtpSynced) to decide whether
// time-based logic such as schedules may run.
bool isTimeValid();

// Parse an HTTP `Date` header (RFC 7231, e.g. "Wed, 23 Sep 2026 08:12:34 GMT")
// into UTC epoch seconds. Returns 0 if it cannot be parsed.
time_t parseHttpDate(const String& s);
