// Host-side tests for sketch.ino: WiFi and broker outages, reconnect
// backoff, offline buffering, ordering, overflow. Build with run_tests.sh.
#include "Arduino.h"
#include "WiFi.h"
#include "PubSubClient.h"
#include <cstring>
#include <algorithm>

unsigned long fake_millis = 0;
int fake_analog = 600;     // bright room
int fake_button = HIGH;
int fake_pwm = -1;
FakeSerial Serial;
FakeWiFi WiFi;

#include "../sketch.ino"

static int failures = 0;
#define CHECK(cond, ...)                                       \
  do {                                                         \
    std::printf((cond) ? "  ok   " : "  FAIL ");               \
    std::printf(__VA_ARGS__);                                  \
    std::printf("\n");                                         \
    if (!(cond)) failures++;                                   \
  } while (0)

static size_t maxPerPass = 0;

// Run loop() every 10 ms of simulated time.
static void run_for(unsigned long ms) {
  unsigned long end = fake_millis + ms;
  while (fake_millis < end) {
    size_t before = mqtt.published.size();
    loop();
    size_t n = 0;   // count readings only, not the one-off "online" status
    for (size_t i = before; i < mqtt.published.size(); i++)
      if (mqtt.published[i].topic == TOPIC_READINGS) n++;
    maxPerPass = std::max(maxPerPass, n);
    fake_millis += 10;
  }
}

struct Seen { unsigned long seq, age, dropped; };
static std::vector<Seen> readings() {
  std::vector<Seen> out;
  for (auto& m : mqtt.published) {
    if (m.topic != TOPIC_READINGS) continue;
    Seen s{};
    std::sscanf(std::strstr(m.payload.c_str(), "\"seq\":"), "\"seq\":%lu", &s.seq);
    std::sscanf(std::strstr(m.payload.c_str(), "\"age\":"), "\"age\":%lu", &s.age);
    std::sscanf(std::strstr(m.payload.c_str(), "\"dropped\":"), "\"dropped\":%lu", &s.dropped);
    out.push_back(s);
  }
  return out;
}

// Every reading from `from` onward arrived exactly once, in order.
static bool contiguous(size_t from = 0) {
  auto r = readings();
  for (size_t i = from + 1; i < r.size(); i++)
    if (r[i].seq != r[i - 1].seq + 1) return false;
  return true;
}

static std::vector<unsigned long> gaps(const std::vector<unsigned long>& t, size_t from) {
  std::vector<unsigned long> g;
  for (size_t i = from + 1; i < t.size(); i++) g.push_back(t[i] - t[i - 1]);
  return g;
}

int main() {
  std::printf("Connected night lamp (sketch.ino)\n");
  setup();

  // ---- 1. Normal start-up ------------------------------------------------
  run_for(30000);
  auto r = readings();
  CHECK(std::strcmp(clientId, "night-lamp-123456") == 0, "client id taken from the MAC (%s), not zeros", clientId);
  CHECK(wifiWasConnected && mqtt.connected(), "connects to WiFi and the MQTT broker");
  CHECK(mqtt.willTopic == TOPIC_STATUS && mqtt.willMessage == "offline" && mqtt.willRetain,
        "last will set: \"offline\" on status topic, retained");
  bool online = false;
  for (auto& m : mqtt.published) if (m.topic == TOPIC_STATUS && m.payload == "online" && m.retained) online = true;
  CHECK(online, "publishes retained \"online\" after connecting");
  CHECK(r.size() >= 13 && r.size() <= 15, "%zu readings published in 30 s (one per 2 s)", r.size());
  CHECK(r.front().seq == 1 && contiguous(), "readings arrive in order, no gaps (seq %lu..%lu)", r.front().seq, r.back().seq);
  CHECK(r.back().age == 0, "live readings are sent immediately (age %lu ms)", r.back().age);

  // ---- 2. WiFi outage, 60 s ----------------------------------------------
  size_t pubBefore = readings().size();
  size_t beginsBefore = WiFi.beginTimes.size();
  WiFi.apUp = false;
  run_for(60000);
  CHECK(readings().size() == pubBefore, "WiFi down: nothing published (%zu new)", readings().size() - pubBefore);
  CHECK(bufCount >= 29 && bufCount <= 31, "WiFi down: %u readings buffered in 60 s", bufCount);
  auto wg = gaps(WiFi.beginTimes, beginsBefore);
  bool growing = true, capped = true;
  for (size_t i = 1; i < wg.size(); i++) if (wg[i] + 1000 < wg[i - 1]) growing = false;
  for (auto g : wg) if (g > WIFI_CONNECT_WAIT_MS + BACKOFF_MAX_MS * 5 / 4 + 20) capped = false;
  std::printf("       WiFi retry gaps (ms):");
  for (auto g : wg) std::printf(" %lu", g);
  std::printf("\n");
  CHECK(wg.size() >= 3 && growing && capped, "WiFi retries back off and are capped (%zu retries)", wg.size());

  WiFi.apUp = true;
  run_for(60000);
  r = readings();
  CHECK(bufCount == 0, "WiFi back: buffer drained (%u left)", bufCount);
  CHECK(contiguous(), "WiFi back: every reading delivered once, in order (seq 1..%lu)", r.back().seq);
  unsigned long maxAge = 0;
  for (auto& s : r) maxAge = std::max(maxAge, s.age);
  CHECK(maxAge >= 55000, "oldest buffered reading waited %lu ms (it is marked with its age)", maxAge);

  // ---- 3. Broker outage, WiFi still up -----------------------------------
  size_t connBefore = mqtt.connectTimes.size();
  mqtt.brokerUp = false;
  run_for(120000);
  auto mg = gaps(mqtt.connectTimes, connBefore);
  std::printf("       MQTT retry gaps (ms):");
  for (auto g : mg) std::printf(" %lu", g);
  std::printf("\n");
  bool doubling = mg.size() >= 6;
  const unsigned long expect[] = {1000, 2000, 4000, 8000, 16000, 30000};
  for (size_t i = 0; i < 6 && i < mg.size(); i++)
    if (mg[i] < expect[i] || mg[i] > expect[i] * 5 / 4 + 20) doubling = false;
  CHECK(doubling, "MQTT retries: ~1, 2, 4, 8, 16, 30 s (+ up to 25%% jitter)");
  bool mcapped = true;
  for (auto g : mg) if (g > BACKOFF_MAX_MS * 5 / 4 + 20) mcapped = false;
  CHECK(mcapped, "MQTT retries never wait longer than 30 s + jitter");
  CHECK(wifiWasConnected, "broker outage does not tear down WiFi");
  mqtt.brokerUp = true;
  run_for(60000);
  CHECK(bufCount == 0 && contiguous(), "broker back: backlog delivered in order (seq up to %lu)", readings().back().seq);

  // ---- 4. Connection drops in the middle of sending the backlog ----------
  WiFi.apUp = false;
  run_for(60000);
  WiFi.apUp = true;
  mqtt.failPublishAfter = 5;          // dies after 5 of ~30 buffered readings
  run_for(90000);
  CHECK(bufCount == 0 && contiguous(), "drop mid-flush: no reading lost or duplicated");

  // ---- 5. Draining is rate-limited ----------------------------------------
  CHECK(maxPerPass <= MAX_PUBLISH_PER_PASS, "at most %zu publishes per loop pass (limit %u)", maxPerPass, MAX_PUBLISH_PER_PASS);

  // ---- 6. Button: simulated access point outage ---------------------------
  size_t b0 = WiFi.beginTimes.size();
  size_t p0 = readings().size();
  fake_button = LOW;  run_for(200);
  fake_button = HIGH; run_for(19000);
  CHECK(!wifiWasConnected && WiFi.beginTimes.size() == b0, "button: link down, no join attempts during the 20 s outage");
  CHECK(readings().size() == p0, "button: readings held back while down (%u buffered)", bufCount);
  run_for(40000);
  CHECK(wifiWasConnected && bufCount == 0 && contiguous(), "button: reconnects by itself and delivers the backlog in order");

  // ---- 7. Lamp keeps working offline --------------------------------------
  WiFi.apUp = false;
  fake_analog = 4000;
  run_for(3000);
  CHECK(lampOn && fake_pwm == 255, "offline: lamp still responds to the dark (pwm=%d)", fake_pwm);
  fake_analog = 600;
  run_for(3000);
  CHECK(!lampOn && fake_pwm == 0, "offline: lamp still turns off in light");

  // ---- 8. Long outage overflows the buffer --------------------------------
  size_t before = readings().size();
  run_for(15UL * 60 * 1000);          // 15 more minutes offline
  CHECK(bufCount == BUFFER_SIZE, "15 min offline: buffer capped at %u readings", bufCount);
  CHECK(droppedReadings > 0, "oldest readings dropped and counted (%lu dropped)", (unsigned long)droppedReadings);
  WiFi.apUp = true;
  run_for(120000);
  r = readings();
  CHECK(bufCount == 0, "after long outage: buffer drained");
  CHECK(contiguous(before), "after long outage: newest 10 min delivered in order");
  unsigned long gap = r[before].seq - r[before - 1].seq - 1;
  CHECK(gap == droppedReadings && r.back().dropped == droppedReadings,
        "the seq gap (%lu) matches the reported \"dropped\" count, so loss is visible", gap);

  std::printf(failures ? "  %d test(s) FAILED\n" : "  all passed\n", failures);
  return failures ? 1 : 0;
}
