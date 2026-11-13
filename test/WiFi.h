// Fake ESP32 WiFi. The test flips `apUp` to simulate the access point
// disappearing and coming back.
#pragma once
#include "Arduino.h"
#include <vector>

#define WL_IDLE_STATUS 0
#define WL_CONNECTED 3
#define WIFI_STA 1

struct FakeWiFi {
  bool apUp = true;                    // is the network reachable at all?
  unsigned long joinTimeMs = 3000;     // how long a successful join takes
  int st = WL_IDLE_STATUS;
  bool joining = false;
  unsigned long joinStartedAt = 0;
  std::vector<unsigned long> beginTimes;

  int status() {
    if (!apUp) { st = WL_IDLE_STATUS; }
    else if (joining && fake_millis - joinStartedAt >= joinTimeMs) {
      st = WL_CONNECTED;
      joining = false;
    }
    return st;
  }
  void begin(const char*, const char*) {
    beginTimes.push_back(fake_millis);
    joining = true;
    joinStartedAt = fake_millis;
    st = WL_IDLE_STATUS;
  }
  void disconnect() { st = WL_IDLE_STATUS; joining = false; }
  void mode(int) {}
  void setAutoReconnect(bool) {}
  void macAddress(uint8_t* m) { const uint8_t mac[6] = {0x24, 0x0a, 0xc4, 0x12, 0x34, 0x56}; for (int i = 0; i < 6; i++) m[i] = mac[i]; }
  const char* localIP() { return "10.13.37.2"; }
};
extern FakeWiFi WiFi;

class WiFiClient {};
