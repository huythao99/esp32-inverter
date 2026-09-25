#pragma once

// ---------------------------------------------------------------------------
// Compile-time configuration, pin map, credentials and debug macros.
// No mutable state here — see shared_state.h for that.
// ---------------------------------------------------------------------------

// Debug logging. Set DEBUG to 0 for production to compile out all USB-serial
// debug output (removes ~90 blocking Serial.print calls from the hot paths).
// Note: this only affects the USB Serial; the STM32 link (testSerial) is untouched.
#define DEBUG 0
#if DEBUG
  #define DBG_PRINT(...)   Serial.print(__VA_ARGS__)
  #define DBG_PRINTLN(...) Serial.println(__VA_ARGS__)
#else
  #define DBG_PRINT(...)   do {} while (0)
  #define DBG_PRINTLN(...) do {} while (0)
#endif

#define WIFI_BROADCAST_SSID "GTIControl1369"

#define KEY_SPLIT "&&&&"
#define KEY_SPLIT_DATA "#"

// Note: don't use bare RX/TX — Arduino's pins_arduino.h declares those as pin
// constants, so a #define RX/TX collides depending on include order.
#define STM_RX 13 // 13
#define STM_TX 12 // 12

#define STM_READY 14
#define STM_START 2

// MQTT Configuration
#define MQTT_SERVER "giabao-inverter.com"
#define MQTT_PORT 1883        // plain (legacy / fallback)
#define MQTT_TLS_PORT 8883    // TLS, server cert verified against ca_certs.h
// 1 = connect over TLS (8883). 0 = plain 1883 only.
#define MQTT_USE_TLS 1
// Transition safety net: after MQTT_TLS_MAX_FAILS consecutive TLS failures
// (broker without 8883, cert problem...) fall back to plain 1883 so the device
// stays online, and try TLS again after MQTT_TLS_RETRY_MS. Set to 0 once every
// broker serves 8883: a fallback can be forced by an attacker blocking 8883.
#define MQTT_TLS_FALLBACK_PLAIN 1
#define MQTT_TLS_MAX_FAILS 3
#define MQTT_TLS_RETRY_MS 3600000UL   // 1 h
#define MQTT_USERNAME "giabao"
#define MQTT_PASSWORD "0918273645"

// SHA256 Authentication Key
#define SHA_SECRET_KEY "K8mN2pQ7vX4bE9fH3gJ6kL1mP5sT8wZ2"

// API base
#define API_BASE "https://giabao-inverter.com"
