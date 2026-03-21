# ESP32-C3 MQTT

Universal ESP32-C3 firmware with a web configurator, Wi-Fi setup portal, and Home Assistant integration over MQTT Discovery.

## Features

- Configurable outputs: `relay`, `pwm`, `ws2812`, `servo_3wire`, `servo_5wire`
- Configurable inputs and buttons in one editor
- Sensor support: `ds18b20_bus`, `aht20`, `sht3x`, `bme280`
- AP mode for first-time setup
- Startup Wi-Fi scan with cached results in the UI
- Live output test and live input indication in the setup page
- MQTT Discovery for Home Assistant
- Configuration stored in NVS and managed through `/api/config` and `/api/apply`

## Repository Layout

Files required to build the project on another computer are committed to Git:

- `platformio.ini`
- `sdkconfig`
- `partitions.csv`
- `src/idf_component.yml`
- `dependencies.lock`
- source code in `main/`, `src/`, `include/`, `lib/`

Local machine files and generated artifacts are not required and are ignored:

- `.pio/`
- `build/`
- `managed_components/`
- `.vscode/`
- backup `sdkconfig*` files

## First Boot

1. If STA Wi-Fi is not configured, the device scans nearby networks.
2. The device starts an access point like `ESP32-SETUP-XXXXXX`.
3. Open `http://192.168.4.1`.
4. Configure Wi-Fi, MQTT, and GPIO modules.
5. Save and apply the configuration.

## MQTT

Main settings are stored in `connectivity.mqtt`.

Example:

```json
{
  "connectivity": {
    "mqtt": {
      "enable": true,
      "host": "192.168.1.10",
      "port": 1883,
      "user": "",
      "pass": "",
      "client_id": "",
      "topic_prefix": "",
      "discovery_prefix": "homeassistant",
      "discovery": true,
      "retain": true
    }
  }
}
```

Notes:

- `host` may be an IPv4 address, DNS name, or URI like `mqtt://192.168.1.10:1883`
- if `client_id` is empty, the firmware generates a unique ID from the board MAC
- after config changes, call `/api/apply`

## Home Assistant

1. Enable MQTT integration in Home Assistant.
2. Configure the same broker host and credentials.
3. Save the device config and run `POST /api/apply`.
4. Entities appear automatically through MQTT Discovery.

## Build With PlatformIO

Requirements:

- PlatformIO Core
- ESP-IDF toolchain installed through PlatformIO

Build:

```bash
pio run
```

Flash:

```bash
pio run -t upload
```

Monitor:

```bash
pio device monitor -b 115200
```

## Build With ESP-IDF

Requirements:

- ESP-IDF 5.5.x
- Python environment from ESP-IDF install

Build:

```bash
idf.py build
```

Flash:

```bash
idf.py -p COM5 flash
```

Monitor:

```bash
idf.py -p COM5 monitor
```

## Dependency Management

Component versions are pinned in `dependencies.lock`.

On a fresh machine, dependencies are restored automatically during the first build.
