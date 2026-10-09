# ghanode — SiWx917 sensor MQTT client

Wi-Fi sensor node firmware for the **SiWx917** (SoC), built on **FreeRTOS** (CMSIS-RTOS2). It joins a WPA2/WPA3 network over DHCP, samples on-board sensors, and publishes JSON telemetry to an MQTT broker.

Two application tasks share the work:

- **`application_start`** — Wi-Fi bring-up, MQTT client connect, and the idle/yield loop that keeps the network/MQTT side alive (events are handled in MQTT callbacks).
- **`sensor_task`** — independent sampling loop (Si7021 + ADC); when MQTT is connected it publishes each reading. Sensor sampling continues even if the network is down.

Forked from the Silicon Labs *Wi-Fi Embedded MQTT Client* example (`wifi_embedded_mqtt_client_soc`); application logic lives mainly in `app.c`.

## Role in the GHA greenhouse platform

This node is the physical edge sensor for **[GHA](https://github.com/HCDani/GHA)** (Greenhouse Monitoring), a 7th-semester bachelor project at VIA University College. GHA is a self-hosted stack where sensor nodes publish temperature, humidity, and light over MQTT; the platform validates and stores the readings and shows each user their greenhouses with embedded Grafana dashboards (plus outdoor weather for the greenhouse location).

How a reading from this firmware is consumed:

1. This node publishes JSON to **`ghanode/sensor`** on Mosquitto (authenticated; anonymous access disabled).
2. **Filebeat** subscribes via its MQTT input and forwards to **Logstash**.
3. **Logstash** parses the JSON, normalises names (`Sensor ID` → `sensor_id`, `temperature` → `temperature_c`, `humidity` → `humidity_pct`), and validates that every reading is present and numeric.
4. Valid documents go to daily **OpenSearch** indices (`sensors-YYYY.MM.dd`); invalid ones to `sensors-errors-YYYY.MM.dd` with a `pipeline_errors` array so bad input is never silently dropped.
5. **Grafana** queries OpenSearch; the React frontend embeds those panels per greenhouse (Keycloak SSO, PocketBase data model).

The payload shape below is what that pipeline expects. Full platform docs, Docker Compose stack, and frontend live in the [GHA repository](https://github.com/HCDani/GHA).

## What it does

1. Connects as a Wi-Fi STA (credentials and DHCP hostname from `secret.h`).
2. Connects to an MQTT broker (default port **1883**, username/password from `secret.h`).
3. Every **20 s**, reads:
   - **Si7021** temperature and humidity (I2C2)
   - **Light** proxy from ADC channel 1 on **GPIO_25** (millivolts as integer)
4. Publishes to topic `ghanode/sensor` with **Sensor ID** = STA Wi-Fi MAC address.

The stock WiSeConnect Si70xx driver (`sl_si91x_si70xx`) converts with floats internally but casts the result to an integer, so it can only report whole °C and %RH. This project uses a small local fork of that path in `si70xx_float.c` / `si70xx_float.h` that returns fractional Celsius (and RH) as `float`.

Example payload:

```json
{"Sensor ID": "8C:65:A3:19:AC:98", "timestamp":"2026-10-06T00:05:12Z+200", "temperature":23.4, "humidity": 47, "light": 812}
```

Timestamps use uptime-based `HH:MM:SS` on a fixed build date until NTP/NWP timesync is available.

## Hardware

| Item | Notes |
|------|--------|
| Radio board | BRD4338A (SiWx917) — see `wifi_embedded_mqtt_client_soc.slcp` |
| Temp / RH | Si7021 on I2C2 |
| Light | ADC CH1 → GPIO_25 |
| SDK | Simplicity SDK **2026.6.1**, WiSeConnect **4.1.1** |

NWP (connectivity) firmware must match the SDK (e.g. **2.16.x** for WiSeConnect 4.1.1). Older NWP images can fail IP bring-up / MQTT with errors such as timestamp notify `0x7`.

## Secrets

Credentials are not committed. Copy the template and edit locally:

```text
copy secret_example.h secret.h
```

| Macro | Purpose |
|-------|---------|
| `SECRET_WIFI_SSID` / `SECRET_WIFI_PASSWORD` | STA credentials |
| `SECRET_WIFI_DHCP_HOSTNAME` | DHCP option 12 hostname (≤30 chars) |
| `SECRET_MQTT_USERNAME` / `SECRET_MQTT_PASSWORD` | MQTT login |

`secret.h` is gitignored. Wi-Fi profile macros in `config/sl_net_default_values.h` pull from these values.

## MQTT / app knobs (`app.c`)

| Define | Default / role |
|--------|----------------|
| `MQTT_BROKER_IP` | Broker IPv4 (e.g. `192.168.6.1`) |
| `MQTT_BROKER_PORT` | `1883` |
| `PUBLISH_TOPIC` | `ghanode/sensor` |
| `SI70XX_SAMPLE_INTERVAL_MS` | Sample + publish period (`20000`) |
| `ENCRYPT_CONNECTION` | `0` (plain MQTT; TLS not enabled in this build) |

## Build

CMake ≥ 3.25, from `cmake_gcc/`:

```bash
cmake --workflow --preset project
```

Or:

```bash
cmake --preset project && cmake --build --preset default_config
```

Output: `cmake_gcc/build/base/wifi_embedded_mqtt_client_soc.rps` (and `.out` / `.map`).

SDK trees (`simplicity_sdk_2026.6.1/`, `wiseconnect3_sdk_4.1.1/`) are produced by `slc generate -cpsdk` and are gitignored; regenerate if missing.

## Flash / run

Flash the application RPS (and NWP firmware if upgrading) with Simplicity Commander or your usual SiWx917 flow, then open the VCOM serial port for logs (`Si7021: …`, `MQTT publish: …`).

## Layout

| Path | Role |
|------|------|
| `app.c` | Wi-Fi, MQTT, sensor task, JSON publish |
| `si70xx_float.c` / `.h` | Si7021 helpers (float °C/%RH; SDK driver is integer-only) |
| `secret_example.h` | Credential template |
| `config/` | Network, ADC, I2C, and other generated configs |
| `cmake_gcc/` | GCC/CMake build |
| `log/` | Local captures (gitignored) |

## Upstream docs

- [WiSeConnect getting started](https://docs.silabs.com/wiseconnect/latest/wiseconnect-getting-started/)
- [Recommended settings](https://docs.silabs.com/wiseconnect/latest/wiseconnect-developers-guide-prog-recommended-settings/)
