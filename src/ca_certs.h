#pragma once

// Root CAs the ESP32 trusts for giabao-inverter.com (HTTPS API/firmware and
// MQTT over TLS). The server certificate is issued by Let's Encrypt, which
// chains to ISRG Root X1 (RSA) or ISRG Root X2 (ECDSA). Both are included so
// a switch of key type on the server keeps working.
//   ISRG Root X1  valid to 2035-06-04  SHA-256 96:BC:EC:06:26:49:76:F3:...:08:C6
//   ISRG Root X2  valid to 2040-09-17  SHA-256 69:72:9B:8E:15:A8:6E:FC:...:14:70
// If the server ever moves to another CA, add its root here BEFORE switching,
// otherwise devices can no longer reach the API or download firmware.
// Defined once in ca_certs.cpp (PEM, both roots concatenated).
extern const char kRootCA[];
