# Evidence: data arriving

Captured from a real run: the device running in the Wokwi simulator, publishing to `broker.hivemq.com`, and `tools/subscriber.py` receiving on another machine.

| File | What it shows |
|---|---|
| `subscriber-log.txt` | Every message as it arrived. Live readings, the `offline`/`online` status changes, and the held-back readings arriving after the outage marked `[buffered, waited N s]`. Ends with a summary line showing `missing=0`. |
| `readings.csv` | The same readings as a table: arrival time, `seq`, device time, `age`, level, lamp. |
| `wokwi-serial.txt` | The device's own serial log for the same run: connection lost → buffering → reconnect → backlog sent. |

How the run was done:

1. Start the device in Wokwi and run `python tools/subscriber.py`.
2. Let it publish live readings for about 30 s.
3. Press **drop network** in Wokwi and wait for it to recover (about 30 s).
4. Let it run another 30 s, then press Ctrl+C in the subscriber to print the summary.
