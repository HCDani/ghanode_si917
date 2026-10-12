# ghanode — SiWx917 sensor MQTT client

Wi-Fi sensor node firmware for the **SiWx917** (SoC), built on **FreeRTOS** (CMSIS-RTOS2). The SiWx917 pairs an application core (Cortex-M4) with a separate network processor (NWP) that runs the Wi-Fi and TCP/IP stack. The node joins a WPA2/WPA3 network as a Wi-Fi station (STA) over DHCP, samples on-board sensors, and publishes JSON telemetry to an MQTT broker.

Sampling and networking run as a producer and a consumer: a sensor task queues a reading every 20 s, and a network task publishes the queue while the broker is reachable and reconnects Wi-Fi and MQTT when it is not. Readings taken during an outage are buffered and sent after the reconnect. See [RTOS design](#rtos-design).

Forked from the Silicon Labs *Wi-Fi Embedded MQTT Client* example (`wifi_embedded_mqtt_client_soc`); application logic lives mainly in `app.c`.

## Relationship to GHA

**[GHA](https://github.com/HCDani/GHA)** (Greenhouse Monitoring) was my 7th-semester bachelor project at VIA University College, finished in February 2026: a self-hosted stack where sensor nodes published temperature, humidity, and light over MQTT, and the platform validated and stored the readings and showed each user their greenhouses with embedded Grafana dashboards. This firmware was written afterwards, against the ingest contract that GHA's pipeline expects.

The pipeline it targets:

1. The node publishes JSON to **`ghanode/sensor`** on Mosquitto (authenticated; anonymous access disabled).
2. **Filebeat** subscribes via its MQTT input and forwards to **Logstash**.
3. **Logstash** parses the JSON, normalises names (`Sensor ID` → `sensor_id`, `temperature` → `temperature_c`, `humidity` → `humidity_pct`), and validates that every reading is present and numeric.
4. Valid documents go to daily **OpenSearch** indices (`sensors-YYYY.MM.dd`); invalid ones to `sensors-errors-YYYY.MM.dd` with a `pipeline_errors` array so bad input is never silently dropped.
5. **Grafana** queries OpenSearch; the React frontend embeds those panels per greenhouse (Keycloak SSO, PocketBase data model).

The payload shape below is what that pipeline expects. Platform docs, the Docker Compose stack, and the frontend live in the [GHA repository](https://github.com/HCDani/GHA).

## What it does

1. Connects as a Wi-Fi STA (credentials and DHCP hostname from `secret.h`).
2. Connects to an MQTT broker (default port **1883**, username/password from `secret.h`).
3. Every **20 s**, reads:
   - **Si7021** temperature and humidity (I2C2)
   - **Light** in lux (integer), estimated from a GL5528 photoresistor on ADC channel 1 (**GPIO_25**). It's used to measure how long each day the light stays above 2,000 lux; the node reports lux, and the daily duration is worked out downstream.
4. Publishes to topic `ghanode/sensor` with **Sensor ID** = STA Wi-Fi MAC address. The same MAC is the MQTT client ID, so several nodes can share one broker without disconnecting each other. No last-will message is registered.

The stock WiSeConnect Si70xx driver (`sl_si91x_si70xx`) converts with floats internally but casts the result to an integer, so it can only report whole °C and %RH. This project uses a small local fork of that path in `si70xx_float.c` / `si70xx_float.h` that returns fractional Celsius (and RH) as `float`.

Example payload:

```json
{"Sensor ID": "8C65A319AC98", "temperature":23.4, "humidity": 47, "light": 767}
```

The payload carries no timestamp; the node has no real-time clock source, so readings are timed on arrival by the pipeline.

## RTOS design

FreeRTOS, used through the CMSIS-RTOS2 API (`osThreadNew`, `osMessageQueueNew`, `osEventFlagsNew`).

| Thread | CMSIS priority | Stack (peak use) | Role | Blocks on |
|--------|----------------|------------------|------|-----------|
| `sensor` | `osPriorityBelowNormal` | 2048 B (1416 B) | Producer: reads the Si7021 and the ADC, queues a reading | `osDelayUntil`, a fixed 20 s period |
| `net` | `osPriorityLow` | 3072 B (2308 B) | Bring-up, then consumer: owns Wi-Fi and MQTT, publishes the queue | Event flags |
| SDK event engine / HAL / command engine | 50 / 49 / 48 | SDK | NWP communication; run the MQTT and Wi-Fi callbacks | SDK internal |

Peak use is the high-water mark measured on the board across bring-up, publishing, broker outages and a full Wi-Fi outage with rejoin. That leaves about 25 % headroom on `net` and 30 % on `sensor`.

- **Reading queue.** `osMessageQueue` of 30 readings, 10 minutes at 20 s. When it fills during a long outage, the oldest reading is dropped.
- **Event flags.** The SDK callbacks run on the SDK's threads, so they only set flags (`MQTT_UP`, `MQTT_DOWN`, `PUBLISHED`, `PUBLISH_FAILED`, `WIFI_DOWN`) and never block. The sensor task sets `READING` after queuing. The network task blocks on these and does all the work.
- **One publish in flight.** The network task waits for the `PUBLISHED` event before formatting the next payload, because the SDK publishes from the payload buffer asynchronously. A reading whose publish wasn't confirmed is kept and sent first after the reconnect.
- **Recovery.** A failed MQTT connect is retried after 5 s, doubling up to 60 s. That wait ends early if Wi-Fi is lost. After beacon loss the NWP rejoins by itself; if it gives up, the join callback sets `WIFI_DOWN`, and the network task rescans and rejoins, with the same backoff.
- **Dead sessions with no disconnect event.** After a Wi-Fi drop on the board, no disconnect or join-failure event arrived; the SDK still reported MQTT as connected while the NWP rejected every publish. Three rejections in a row, or a publish not confirmed within 10 s, are therefore treated as a dead session. The unsent reading is kept, not dropped.
- **A fresh local port for every connect.** The example used a fixed client port. After a Wi-Fi drop the broker still held the old TCP session, so a reconnect from the same address and port collided with it and failed until the broker gave up on the session. With a 2000 s keep-alive, that took about 50 minutes. Each connect now uses a new ephemeral port, and the keep-alive is 60 s.
- **Never two connects at once.** On the board, a TCP connect to a stopped broker took about 30 s to fail. An earlier version gave up waiting after 15 s and started a second connect on top of the first; when both failed, the SDK's own cleanup failed and left its client stuck, so every later attempt was refused. The network task now waits up to 60 s for each attempt to resolve.
- **Recovery ladder.** A stuck client, an attempt that never resolves, or a dead session first restarts Wi-Fi and re-initialises the MQTT client. The NWP keeps its own MQTT client instance until it is told to deinit, so that is released explicitly first; resetting only the host-side struct left every later connect failing. If no publish gets through before the next failure, or three connects in a row fail after the restart, the MCU resets, which is known to recover. A broker that is really down therefore costs at most one reset per outage.
- **Priorities.** The sensor task sits above the network task so a slow reconnect never delays a sample. Both sit below the SDK threads, which must service the NWP.
- **Stacks.** Overflow checking is on (`configCHECK_FOR_STACK_OVERFLOW 2`). `vApplicationStackOverflowHook` records the task name in `stack_overflow_task` and halts. Both tasks' high-water marks are logged after the first sample and then hourly (`Stack unused at high-water: …`). The sensor task stores its own handle on entry: it outranks the task that creates it, so it runs before `osThreadNew()` has returned the handle, and the first report otherwise read 0 B.

## What wasn't in the example

- **Si7021 power gating.** The sensor sits on the WSTK mainboard behind `SENSOR_ENABLE` (UULP_VBAT_GPIO_1). Until that pad is configured as an M4-owned output and driven high, the bus is isolated and every transaction fails. The firmware reads the output latch back from the NPSS GPIO register to confirm it, then waits the datasheet's 80 ms power-up time.
- **I2C block left disabled.** `sl_si91x_i2c_init()` leaves `IC_ENABLE` off, so writes report success with no clock on SCL and the first read times out (`0x10`). Configuring the FIFO thresholds re-enables the block.
- **Integer-only SDK driver.** The stock Si70xx driver computes in float but returns an integer; `si70xx_float.c` keeps the fraction.
- **ADC offset binary.** The calibrated ADC read returns offset-binary, which is undone (XOR `0x800`) to get a 0–4095 count before scaling to volts.
- **Lux from a photoresistor.** The GL5528 sits between GPIO_25 and GND with a 4.7 kΩ pull-up, so its resistance is `4.7 kΩ · V / (3.3 V − V)`. Lux follows from the datasheet's power law, `lux = 10 · (R10 / R)^(1/γ)`, with R10 = 15 kΩ (the middle of the datasheet's 10–20 kΩ at 10 lux) and γ = 0.7. A full-scale reading (open sensor or darkness) reports 0.
- **Bring-up order.** The NWP has to boot before any ULP peripheral is touched; ULP activity during boot hung the app without returning an error.
- **EU channels and PMF.** The SDK default region is US, which only scans channels 1–11. The node uses the EU region, scans, picks the strongest matching AP, and pins its BSSID and channel; 802.11w stays on because the target AP requires it.
- **Diagnostics.** At boot the node logs an AP scan, its IP configuration and the NWP firmware version, and sends an ICMP probe to the broker, so a failed MQTT connection can be told apart from a routing problem.

## Known limitations

- **No timestamp in the payload.** Readings are timed when the pipeline receives them, so readings buffered during an outage arrive together and are stamped with the reconnect time, not the time they were taken.
- **Plain MQTT on 1883.** TLS is wired behind `ENCRYPT_CONNECTION` but not enabled.
- **Lux is uncalibrated.** The light reading exists to measure how much of the day exceeds 2,000 lux, so only accuracy near that threshold matters. Resolution there is fine (about 0.24 V, ADC count ~297, roughly 10 lux per count), but GL5528 parts vary: a true 2,000 lux reads anywhere from about 1,300 to 3,600 lux depending on the part. A one-point check against a lux meter at ~2,000 lux, adjusting `LDR_R10_OHMS`, removes that. Above the threshold the value saturates quickly, which doesn't matter for this purpose.
- **Humidity is rounded** to whole %RH in the payload.
- **A failed Si7021 read skips the sample** rather than sending zeros, so a dead sensor shows up as a gap in the data.
- **Sampling starts after the first Wi-Fi join.** Bring-up keeps the tested order (NWP, Wi-Fi, then sensors); readings taken during later outages are buffered.
- **No unit tests yet**; verified on the board over VCOM.

## Hardware

| Item | Notes |
|------|--------|
| Radio board | BRD4338A (SiWx917) — see `wifi_embedded_mqtt_client_soc.slcp` |
| Mainboard | BRD4002A (WSTK); carries the Si7021 |
| Temp / RH | Si7021 on I2C2, powered through `SENSOR_ENABLE` |
| Light | GL5528 photoresistor, GPIO_25 to GND, 4.7 kΩ pull-up to 3.3 V; ADC CH1 |
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
| `app.c` | Bring-up, sensor and network tasks, Wi-Fi/MQTT recovery, JSON publish |
| `config/FreeRTOSConfig.h` | Kernel config; stack overflow checking enabled |
| `si70xx_float.c` / `.h` | Si7021 helpers (float °C/%RH; SDK driver is integer-only) |
| `secret_example.h` | Credential template |
| `config/` | Network, ADC, I2C, and other generated configs |
| `cmake_gcc/` | GCC/CMake build |

## Upstream docs

- [WiSeConnect getting started](https://docs.silabs.com/wiseconnect/latest/wiseconnect-getting-started/)
- [Recommended settings](https://docs.silabs.com/wiseconnect/latest/wiseconnect-developers-guide-prog-recommended-settings/)
