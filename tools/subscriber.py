#!/usr/bin/env python3
"""Evidence collector: subscribe to the night lamp's topics and log every
reading as it arrives.

    pip install paho-mqtt
    python tools/subscriber.py

For each reading it prints whether it was live or buffered (and how long it
waited), and checks the sequence numbers so any lost or duplicated reading
is flagged. Everything is also written to evidence/subscriber-log.txt and
evidence/readings.csv. Stop with Ctrl+C to see a summary.
"""
import csv
import json
import os
import sys
from datetime import datetime

BROKER = "broker.hivemq.com"
PORT = 1883
BASE = "chinmaydubey231/night-lamp"
LIVE_MAX_AGE_MS = 3000  # older than this = it sat in the offline buffer

HERE = os.path.dirname(os.path.abspath(__file__))
EVIDENCE = os.path.join(HERE, "..", "evidence")


class Tracker:
    """Checks the stream of sequence numbers. No network code, so it can
    be tested on its own (see --self-test)."""

    def __init__(self):
        self.last_seq = None
        self.received = self.buffered = self.duplicates = 0
        self.missing = 0
        self.max_age = 0

    def add(self, reading):
        seq, age = reading["seq"], reading.get("age", 0)
        note = []
        if self.last_seq is not None:
            if seq <= self.last_seq:
                self.duplicates += 1
                note.append(f"DUPLICATE/OLD (last was {self.last_seq})")
            elif seq > self.last_seq + 1:
                gap = seq - self.last_seq - 1
                self.missing += gap
                note.append(f"GAP: {gap} missing")
        if self.last_seq is None or seq > self.last_seq:
            self.last_seq = seq
        self.received += 1
        if age > LIVE_MAX_AGE_MS:
            self.buffered += 1
            note.insert(0, f"buffered, waited {age / 1000:.1f} s")
        else:
            note.insert(0, "live")
        self.max_age = max(self.max_age, age)
        return ", ".join(note)

    def summary(self):
        return (f"received={self.received} buffered={self.buffered} "
                f"missing={self.missing} duplicates={self.duplicates} "
                f"longest wait={self.max_age / 1000:.1f} s")


def run():
    import paho.mqtt.client as mqtt

    os.makedirs(EVIDENCE, exist_ok=True)
    log = open(os.path.join(EVIDENCE, "subscriber-log.txt"), "a", buffering=1)
    csv_path = os.path.join(EVIDENCE, "readings.csv")
    new_csv = not os.path.exists(csv_path)
    csv_file = open(csv_path, "a", newline="", buffering=1)
    writer = csv.writer(csv_file)
    if new_csv:
        writer.writerow(["received_at", "seq", "device_t_ms", "age_ms",
                         "level", "lamp", "dropped", "note"])
    tracker = Tracker()

    def out(line):
        stamped = f"{datetime.now().strftime('%H:%M:%S')}  {line}"
        print(stamped)
        log.write(stamped + "\n")

    def on_connect(client, userdata, flags, reason_code, properties):
        out(f"subscriber connected to {BROKER} ({reason_code}), "
            f"listening on {BASE}/#")
        client.subscribe(f"{BASE}/#", qos=1)

    def on_message(client, userdata, msg):
        text = msg.payload.decode(errors="replace")
        if msg.topic.endswith("/status"):
            out(f"STATUS  {text}{'  (retained)' if msg.retain else ''}")
            return
        try:
            r = json.loads(text)
        except json.JSONDecodeError:
            out(f"bad payload: {text}")
            return
        note = tracker.add(r)
        out(f"seq={r['seq']:<5} level={r['level']:<5} lamp={r['lamp']:<4} [{note}]")
        writer.writerow([datetime.now().isoformat(timespec="seconds"), r["seq"],
                         r["t"], r["age"], r["level"], r["lamp"],
                         r.get("dropped", 0), note])

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(BROKER, PORT, keepalive=30)
    try:
        client.loop_forever()
    except KeyboardInterrupt:
        out("SUMMARY " + tracker.summary())


def self_test():
    t = Tracker()
    for seq, age in [(1, 0), (2, 0), (3, 20000), (4, 18000), (5, 0), (7, 0), (7, 0)]:
        t.add({"seq": seq, "age": age})
    assert (t.received, t.buffered, t.missing, t.duplicates) == (7, 2, 1, 1), t.summary()
    print("self-test ok:", t.summary())


if __name__ == "__main__":
    self_test() if "--self-test" in sys.argv else run()
