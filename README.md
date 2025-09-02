# ESP8266 Diffuser Controller

Features:
- Wi-Fi provisioning portal (AP mode) on boot if not connected, with scanning and selection (via WiFiManager).
- Web dashboard:
  - Manual trigger button
  - Interval trigger every X seconds
  - Daily schedule at specific times (multiple times, HH:MM)
- Config page:
  - Trigger pin selection and active level (HIGH or LOW)
  - Trigger pulse duration (ms)
  - MQTT broker settings (host, port, username, password, subscribe topic)
  - Timezone offset in minutes
- MQTT:
  - Subscribes to a topic to accept commands:
    - `TRIGGER` or `1` — pulse the trigger
    - `INTERVAL:X` — set interval to X seconds and enable
    - `STOP_INTERVAL` — disable the interval
    - `ADD_SCHEDULE:HH:MM` — add a scheduled time
    - `CLEAR_SCHEDULE` — remove all times
  - Publishes online status to `<topic>/status`

## Hardware

- Board: ESP8266 NodeMCU v3 (board ID: `nodemcuv2`)
- Trigger output pin configurable; default is D1 (GPIO 5). You can choose among D0–D8. Ensure the logic level drives your diffuser’s input safely (use a transistor/relay if needed).

## Build (PlatformIO)

1. Install PlatformIO (VS Code extension or CLI).
2. Open this folder in VS Code (or `pio project init` if needed).
3. Connect the ESP8266 via USB.
4. Build and upload:
   ```
   pio run --target upload
   pio device monitor
   ```

Dependencies are declared in `platformio.ini`:
- tzapu/WiFiManager
- bblanchon/ArduinoJson
- knolleary/PubSubClient

## First Boot / Wi-Fi Setup

- On first boot (or if it cannot connect), the device opens an AP named `Diffuser-XXXXXX`.
- Connect to it and follow the captive portal to scan and configure your Wi-Fi.
- Once connected, browse to the device IP (shown on serial monitor) for the dashboard.

You can also start the Wi-Fi portal from the UI via “WiFi Setup” link (`/api/wifi-portal`).

## Web UI

- Dashboard (`/`):
  - Status, Manual Trigger button
  - Interval controls
  - Schedule list/add/remove
- Config (`/config`):
  - Trigger pin, active level, pulse duration
  - MQTT settings
  - Timezone offset (minutes from UTC)

All settings are persisted in LittleFS (`/config.json`).

## MQTT

- Configure broker host/port and optional username/password on the Config page.
- Subscribe topic defaults to `diffuser/trigger`.
- Commands (publish to the configured topic):
  - `TRIGGER` or `1`
  - `INTERVAL:10` (every 10 seconds)
  - `STOP_INTERVAL`
  - `ADD_SCHEDULE:08:00`
  - `CLEAR_SCHEDULE`
- Status is published to `<topic>/status` as `"online"` on connection.

## Time and Scheduling

- NTP is used to obtain UTC; timezone offset is applied locally (no DST rules).
- Schedules fire at the start of the specified minute (second 0).
- Flags reset at local midnight.

## Notes

- Active level: If your diffuser expects a LOW-going pulse, set “Active Level = LOW”.
- Pulse duration: Adjust to what your diffuser expects (default 1000 ms).
- Safety: The ESP8266 pin outputs 3.3V logic; use appropriate level shifting/driver circuitry for your diffuser input if required.