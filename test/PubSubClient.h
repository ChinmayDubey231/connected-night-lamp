// Fake PubSubClient. The test flips `brokerUp`, or makes publish() fail
// partway through a backlog, to simulate an unreliable broker connection.
#pragma once
#include "WiFi.h"
#include <string>
#include <vector>

struct PublishedMsg {
  std::string topic, payload;
  bool retained;
  unsigned long at;
};

class PubSubClient {
 public:
  explicit PubSubClient(WiFiClient&) {}

  bool brokerUp = true;
  int failPublishAfter = -1;      // >=0: drop the connection after N more publishes
  bool session = false;
  std::vector<unsigned long> connectTimes;
  std::string willTopic, willMessage;
  bool willRetain = false;
  int willQos = -1;
  std::vector<PublishedMsg> published;

  void setServer(const char*, uint16_t) {}
  void setKeepAlive(uint16_t) {}
  void setSocketTimeout(uint16_t) {}

  bool connect(const char*, const char*, const char*,
               const char* wTopic, uint8_t wQos, bool wRetain, const char* wMsg) {
    connectTimes.push_back(fake_millis);
    willTopic = wTopic; willMessage = wMsg; willRetain = wRetain; willQos = wQos;
    session = (WiFi.status() == WL_CONNECTED) && brokerUp;
    return session;
  }
  bool connected() {
    if (session && (WiFi.status() != WL_CONNECTED || !brokerUp)) session = false;
    return session;
  }
  bool publish(const char* topic, const char* payload, bool retained = false) {
    if (!connected()) return false;
    if (failPublishAfter == 0) { session = false; failPublishAfter = -1; return false; }
    if (failPublishAfter > 0) failPublishAfter--;
    published.push_back({topic, payload, retained, fake_millis});
    return true;
  }
  bool loop() { return connected(); }
  int state() { return session ? 0 : -2; }
};
