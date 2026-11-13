# Evidence: data arriving

Real runs: the device in the Wokwi ESP32 simulator, publishing over its WiFi to `broker.hivemq.com`, and `tools/subscriber.py` receiving on a laptop.

| File | What it shows |
|---|---|
| [`subscriber-log.txt`](subscriber-log.txt) | Readings arriving at a separate machine through the public broker, in order (`seq` 67→71). The device's last-will `offline` status, then `online` after it reconnected by itself. |
| [`wokwi-serial.txt`](wokwi-serial.txt) | The device side. Run 1: the first MQTT connect timed out (`state=-4`), the backoff retry 1.24 s later succeeded, and the 2 readings queued meanwhile were sent. Run 2: 65 readings published with an empty buffer. |

What these runs show for real:

- **Data published over WiFi/MQTT, arriving elsewhere:** both files.
- **Reconnect logic:** two real reconnects. The MQTT connect timeout followed by a backoff retry (serial, run 1), and the recovery after the broker had already marked the device offline (subscriber log).
- **Buffering:** run 1 queued 2 readings while MQTT was unavailable and sent them on connect.

A long outage was not captured live with the *drop network* button. That case (buffering for 60 s, a connection drop while sending the backlog, buffer overflow, and in-order delivery with no gaps or duplicates) is covered by the host tests in [`../test`](../test). See the test output in the main README.

The serial logs were captured before the client-ID fix, so they show `night-lamp-000000`. The current code uses the MAC address, or a random ID in the simulator.
