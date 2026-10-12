#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// STM32 link protocol (see STM_PROTO_* in shared_state.h):
//  - LEGACY writer for first-gen power boards (START/READY handshake, raw
//    "VVVVPPPP", limit clamped to 1200 W, writes spaced out),
//  - AUTO detection (new vs legacy) from the STM32's echo of the command.
// Everything here runs on Core 1 (loop / MQTT callback), like the writer.
// ---------------------------------------------------------------------------

// "*VVVV@PPPP#" -> "VVVVPPPP" for LEGACY boards ("" when it has no legacy
// equivalent, e.g. the unlock pulse). The lock command maps to LEGACY_LOCK_VALUE.
String toLegacyValue(const String& out);

// setup(): STM_START/STM_READY pin modes. Returns the NVS hint "confirmed
// LEGACY on a previous boot" (AUTO then tries legacy first). No pin probing:
// AUTO decides from the STM32's echo of the command (stmProtoOnFrame).
bool detectLegacyStm();

// Every valid STM32 frame: AUTO detection compares fields 7/8 (cut-off /
// limit the STM32 runs) with the last command written.
void stmProtoOnFrame(const String& frame);

// Recompute stmLegacy from stmProtoSetting + stmLegacyDetected (switches the
// writer over and re-sends the current value in the new format).
void updateStmProtocol(const char* source);

// From loop(): AUTO phase changes (try legacy / revert) + the STM_PROTOCOL
// log once MQTT is up.
void stmProtocolTick(unsigned long now);

// ---- Used by the single writer (applyCurrentValue, logic.cpp) -------------
// Legacy mode: request / write the value of this command (handshake).
void legacyWrite(const String& out);

// Legacy mode: limit above 1200 W -> 1200 W (what the board will really run).
String clampLegacyLimit(const String& out);

// While AUTO is detecting: the command with its limit offset by a few W.
String stmProbeValue(const String& out);

// A command really went out on the UART (both protocols): echo bookkeeping.
void stmNoteWritten(const String& out);
