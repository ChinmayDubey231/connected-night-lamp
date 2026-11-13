/*
 * Task 03 — Network-connected device
 *
 * ESP32 night lamp that publishes its light readings over MQTT and copes
 * with an unreliable network:
 *
 *  - Every 2 s a reading is put in a RAM ring buffer (300 slots = 10 min).
 *  - Whenever WiFi and MQTT are up, the buffer is drained in order, oldest
 *    first. A reading is removed only after publish() succeeds.
 *  - WiFi and MQTT each reconnect on their own with exponential backoff
 *    (plus jitter), so a dead network is retried without hammering it.
 *  - The lamp keeps working locally whether or not the network is up.
 *  - The button on GPIO 4 simulates the access point going down for 20 s,
 *    so the buffering can be demonstrated in the simulator.
 *
 * Topics (public broker, anyone can subscribe):
 *   chinmaydubey231/night-lamp/readings   one JSON object per reading
 *   chinmaydubey231/night-lamp/status     "online" / "offline" (retained, LWT)
 */

#include <WiFi.h>
#include <PubSubClient.h>

// ---- Network settings -------------------------------------------------------
const char* WIFI_SSID     = "Wokwi-GUEST";   // Wokwi's simulated access point
const char* WIFI_PASSWORD = "";
const char* MQTT_HOST     = "broker.hivemq.com";
const uint16_t MQTT_PORT  = 1883;
const char* TOPIC_READINGS = "chinmaydubey231/night-lamp/readings";
const char* TOPIC_STATUS   = "chinmaydubey231/night-lamp/status";

// ---- Pins (ESP32 DevKit) ----------------------------------------------------
const uint8_t SENSOR_PIN = 34;   // ADC1. ADC2 pins stop working while WiFi is on.
const uint8_t LAMP_PIN   = 18;   // PWM via analogWrite
const uint8_t BUTTON_PIN = 4;    // to GND, internal pull-up

// ---- Timing -----------------------------------------------------------------
const unsigned long SAMPLE_MS        = 100;     // lamp control, local
const unsigned long READING_MS       = 2000;    // one reading queued for MQTT
const unsigned long STATS_MS         = 5000;    // serial status line
const unsigned long SIM_OUTAGE_MS    = 20000;   // button: AP "down" for 20 s
const unsigned long WIFI_CONNECT_WAIT_MS = 10000; // give WiFi.begin() this long
const unsigned long WIFI_BACKOFF_MIN_MS  = 2000;
const unsigned long MQTT_BACKOFF_MIN_MS  = 1000;
const unsigned long BACKOFF_MAX_MS       = 30000;
const uint8_t       MAX_PUBLISH_PER_PASS = 20;  // keep loop() responsive while draining

// ---- Lamp behaviour (12-bit ADC: 0 bright .. 4095 dark) ---------------------
const int DARK_ON   = 1200;
const int DARK_OFF  = 1000;
const int FULL_DARK = 3400;
const uint8_t AVG_SAMPLES = 8;

// ---- Offline buffer ---------------------------------------------------------
struct Reading {
  uint32_t seq;      // 1, 2, 3, ... never reused, so gaps are detectable
  uint32_t takenAt;  // millis() when the reading was taken
  uint16_t level;
  uint8_t  lamp;
};

const uint16_t BUFFER_SIZE = 300;   // 300 x 2 s = 10 minutes offline
Reading  buffer[BUFFER_SIZE];
uint16_t bufHead = 0;               // oldest unsent reading
uint16_t bufCount = 0;
uint32_t droppedReadings = 0;       // overwritten because the buffer was full
uint32_t nextSeq = 1;

void bufferPush(const Reading& r) {
  if (bufCount == BUFFER_SIZE) {
    // Full: drop the oldest so the newest data is kept.
    bufHead = (bufHead + 1) % BUFFER_SIZE;
    bufCount--;
    droppedReadings++;
  }
  buffer[(bufHead + bufCount) % BUFFER_SIZE] = r;
  bufCount++;
}

const Reading& bufferPeek() { return buffer[bufHead]; }

void bufferPop() {
  bufHead = (bufHead + 1) % BUFFER_SIZE;
  bufCount--;
}

// ---- State ------------------------------------------------------------------
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
char clientId[24];

int  samples[AVG_SAMPLES];
uint8_t sampleIndex = 0;
long sampleSum = 0;
int  level = 0;
bool lampOn = false;
uint8_t brightness = 0;

unsigned long lastSampleAt = 0, lastReadingAt = 0, lastStatsAt = 0;

bool wifiWasConnected = false;
unsigned long wifiAttemptAt = 0;     // when the current WiFi.begin() started
bool wifiAttemptActive = false;
unsigned long nextWifiAttemptAt = 0;
uint8_t wifiFailures = 0;
uint32_t wifiReconnects = 0;

unsigned long nextMqttAttemptAt = 0;
uint8_t mqttFailures = 0;
uint32_t mqttReconnects = 0;
uint32_t publishedCount = 0;

unsigned long simOutageUntil = 0;
bool simOutage = false;
int lastButton = HIGH;

// True when `interval` has passed. Stays on the original grid, but if the
// loop was held up for more than one interval it skips ahead instead of
// firing a burst of catch-up events.
bool due(unsigned long& last, unsigned long interval, unsigned long now) {
  if (now - last < interval) return false;
  last += interval;
  if (now - last >= interval) last = now;
  return true;
}

// 1x, 2x, 4x ... the minimum, capped, plus up to 25% random jitter so many
// devices recovering from the same outage do not all retry in lockstep.
unsigned long backoffMs(unsigned long minMs, uint8_t failures) {
  unsigned long d = minMs;
  for (uint8_t i = 1; i < failures && d < BACKOFF_MAX_MS; i++) d *= 2;
  if (d > BACKOFF_MAX_MS) d = BACKOFF_MAX_MS;
  return d + random(0, d / 4 + 1);
}

// ---- Local: sensor and lamp --------------------------------------------------
void sampleAndDrive() {
  int raw = analogRead(SENSOR_PIN);
  sampleSum -= samples[sampleIndex];
  samples[sampleIndex] = raw;
  sampleSum += raw;
  sampleIndex = (sampleIndex + 1) % AVG_SAMPLES;
  level = (int)(sampleSum / AVG_SAMPLES);

  if (!lampOn && level >= DARK_ON)  lampOn = true;
  if (lampOn  && level <  DARK_OFF) lampOn = false;
  brightness = lampOn
      ? (uint8_t)map(constrain(level, DARK_OFF, FULL_DARK), DARK_OFF, FULL_DARK, 20, 255)
      : 0;
  analogWrite(LAMP_PIN, brightness);
}

void queueReading(unsigned long now) {
  Reading r;
  r.seq = nextSeq++;
  r.takenAt = now;
  r.level = (uint16_t)level;
  r.lamp = brightness;
  bufferPush(r);
}

// ---- Simulated outage (button) ------------------------------------------------
void checkButton(unsigned long now) {
  int b = digitalRead(BUTTON_PIN);
  if (b == LOW && lastButton == HIGH && !simOutage) {
    simOutage = true;
    simOutageUntil = now + SIM_OUTAGE_MS;
    // Drop the link without a clean MQTT DISCONNECT, like a real AP failure,
    // so the broker publishes our last-will "offline" message.
    WiFi.disconnect();
    Serial.println(F("[sim] button: access point DOWN for 20 s"));
  }
  lastButton = b;
  if (simOutage && (long)(now - simOutageUntil) >= 0) {
    simOutage = false;
    Serial.println(F("[sim] access point back UP"));
  }
}

// ---- WiFi: connect, detect loss, retry with backoff ----------------------------
void manageWifi(unsigned long now) {
  bool connected = (WiFi.status() == WL_CONNECTED) && !simOutage;

  if (connected) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      wifiAttemptActive = false;
      wifiFailures = 0;
      wifiReconnects++;
      Serial.print(F("[wifi] connected, IP "));
      Serial.println(WiFi.localIP());
      nextMqttAttemptAt = now;          // try MQTT straight away
    }
    return;
  }

  if (wifiWasConnected) {
    wifiWasConnected = false;
    Serial.println(F("[wifi] connection LOST, buffering readings"));
    nextWifiAttemptAt = now;            // first retry immediately
  }

  if (simOutage) return;                // the "AP" is down: nothing to join

  // A WiFi.begin() is in progress: give it time, then count it as failed.
  if (wifiAttemptActive) {
    if (now - wifiAttemptAt < WIFI_CONNECT_WAIT_MS) return;
    wifiAttemptActive = false;
    if (wifiFailures < 255) wifiFailures++;
    unsigned long wait = backoffMs(WIFI_BACKOFF_MIN_MS, wifiFailures);
    nextWifiAttemptAt = now + wait;
    Serial.print(F("[wifi] attempt failed, retry in "));
    Serial.print(wait);
    Serial.println(F(" ms"));
    return;
  }

  if ((long)(now - nextWifiAttemptAt) >= 0) {
    Serial.println(F("[wifi] connecting..."));
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);   // returns at once; status polled above
    wifiAttemptActive = true;
    wifiAttemptAt = now;
  }
}

// ---- MQTT: connect with last will, retry with backoff ------------------------
void manageMqtt(unsigned long now) {
  if (!wifiWasConnected) return;

  if (mqtt.connected()) {
    mqtt.loop();                        // keep-alive and incoming packets
    return;
  }

  if ((long)(now - nextMqttAttemptAt) < 0) return;

  Serial.print(F("[mqtt] connecting to "));
  Serial.print(MQTT_HOST);
  Serial.println(F("..."));
  // Last will: if we vanish, the broker marks us offline (retained).
  bool ok = mqtt.connect(clientId, nullptr, nullptr,
                         TOPIC_STATUS, 1, true, "offline");
  if (ok) {
    mqttFailures = 0;
    mqttReconnects++;
    mqtt.publish(TOPIC_STATUS, "online", true);
    Serial.print(F("[mqtt] connected, "));
    Serial.print(bufCount);
    Serial.println(F(" reading(s) waiting"));
  } else {
    if (mqttFailures < 255) mqttFailures++;
    unsigned long wait = backoffMs(MQTT_BACKOFF_MIN_MS, mqttFailures);
    nextMqttAttemptAt = now + wait;
    Serial.print(F("[mqtt] failed, state="));
    Serial.print(mqtt.state());
    Serial.print(F(", retry in "));
    Serial.print(wait);
    Serial.println(F(" ms"));
  }
}

// ---- Drain the buffer, oldest first -----------------------------------------
void flushBuffer(unsigned long now) {
  if (!wifiWasConnected || !mqtt.connected()) return;

  char payload[160];
  uint8_t sent = 0;
  while (bufCount > 0 && sent < MAX_PUBLISH_PER_PASS) {
    const Reading& r = bufferPeek();
    // "age" = how long the reading waited before being sent (0 when live).
    snprintf(payload, sizeof(payload),
             "{\"seq\":%lu,\"t\":%lu,\"age\":%lu,\"level\":%u,\"lamp\":%u,\"dropped\":%lu}",
             (unsigned long)r.seq, (unsigned long)r.takenAt,
             (unsigned long)(now - r.takenAt), r.level, r.lamp,
             (unsigned long)droppedReadings);
    if (!mqtt.publish(TOPIC_READINGS, payload)) break;  // keep it; retry later
    bufferPop();
    publishedCount++;
    sent++;
  }
  if (sent > 1) {
    Serial.print(F("[mqtt] sent "));
    Serial.print(sent);
    Serial.print(F(" buffered reading(s), "));
    Serial.print(bufCount);
    Serial.println(F(" left"));
  }
}

void printStats() {
  Serial.print(F("[stats] level="));
  Serial.print(level);
  Serial.print(F(" lamp="));
  Serial.print(brightness);
  Serial.print(F(" wifi="));
  Serial.print(wifiWasConnected ? F("up") : F("DOWN"));
  Serial.print(F(" mqtt="));
  Serial.print((wifiWasConnected && mqtt.connected()) ? F("up") : F("DOWN"));
  Serial.print(F(" buffered="));
  Serial.print(bufCount);
  Serial.print(F(" published="));
  Serial.print(publishedCount);
  Serial.print(F(" dropped="));
  Serial.println(droppedReadings);
}

void setup() {
  Serial.begin(115200);
  pinMode(LAMP_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  int first = analogRead(SENSOR_PIN);
  for (uint8_t i = 0; i < AVG_SAMPLES; i++) samples[i] = first;
  sampleSum = (long)first * AVG_SAMPLES;
  level = first;

  // WiFi must be started before reading the MAC, otherwise it reads as
  // all zeros.
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);   // reconnects are handled by manageWifi()

  // Unique client ID from the MAC: two clients with the same ID on a
  // public broker keep kicking each other off.
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(clientId, sizeof(clientId), "night-lamp-%02x%02x%02x", mac[3], mac[4], mac[5]);
  randomSeed(mac[5] ^ micros());
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setKeepAlive(15);
  mqtt.setSocketTimeout(3);

  Serial.print(F("Connected night lamp, client id "));
  Serial.println(clientId);

  unsigned long now = millis();
  lastSampleAt = lastReadingAt = lastStatsAt = now;
}

void loop() {
  unsigned long now = millis();

  checkButton(now);
  if (due(lastSampleAt, SAMPLE_MS, now))   sampleAndDrive();
  if (due(lastReadingAt, READING_MS, now)) queueReading(now);

  manageWifi(now);
  manageMqtt(now);
  flushBuffer(now);

  if (due(lastStatsAt, STATS_MS, now)) printStats();
}
