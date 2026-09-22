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
#define MQTT_PORT 1883
#define MQTT_USERNAME "giabao"
#define MQTT_PASSWORD "0918273645"

// SHA256 Authentication Key
#define SHA_SECRET_KEY "K8mN2pQ7vX4bE9fH3gJ6kL1mP5sT8wZ2"

// API base
#define API_BASE "https://giabao-inverter.com"
