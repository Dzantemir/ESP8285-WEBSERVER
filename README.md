<div align="center">

# 📡 ESP8285 Local Web Server

### Wi-Fi Access Point with Captive Portal

*A lightweight ESP8285 local web server that creates its own Wi-Fi hotspot and serves web content to connected clients. DNS-based captive portal automatically opens the main page — no need to type an IP address. Serves static files from SPIFFS or SD card — fully configurable via menuconfig.*

[![ESP8266 RTOS SDK](https://img.shields.io/badge/ESP8266__RTOS__SDK-v3.4-orange?style=flat-square&logo=espressif)](https://github.com/espressif/esp8266_rtos_sdk)
[![Platform](https://img.shields.io/badge/Platform-ESP8285-blue?style=flat-square)](https://www.espressif.com/en/products/socs/esp8285)
[![Build Tool](https://img.shields.io/badge/Build-ESP8266__IDF-teal?style=flat-square&logo=visualstudiocode)](https://github.com/Dzantemir/ESP8266-IDF)
[![License](https://img.shields.io/badge/License-MIT-green?style=flat-square)](LICENSE)

</div>

---

## ✨ Features

| | Feature | Description |
|---|---------|-------------|
| 📡 | **Captive Portal** | DNS interception auto-redirects clients to the web page upon Wi-Fi connection |
| 📶 | **Smart Channel Selection** | Scans the air and auto-switches to the least congested Wi-Fi channel |
| 🌐 | **Web Interface** | Dark-themed web page served from SPIFFS or SD card |
| 🔔 | **Action Trigger** | HTTP POST `/beep` triggers a GPIO action — buzzer, relay, LED, etc. (active & passive buzzer built-in) |
| 💾 | **Dual Storage** | SPIFFS (internal flash) and/or SD card via SPI (HSPI) |
| 🔐 | **SD Password Mgmt** | Full CMD42: lock/unlock, set/change/clear password, force erase |
| 🔋 | **Battery Control** | ADC voltage monitoring with automatic deep sleep on low charge |
| 🔄 | **Reset Timer** | Random reboot every 1–31 days to free resources |
| 🛑 | **Graceful Shutdown** | Clean termination of all FreeRTOS tasks before reboot or sleep |
| ⚙️ | **Orchestrator** | Serializes server start/stop, race condition protection |

---

## 🖥️ How It Works

```
  ┌──────────┐     Wi-Fi      ┌──────────────────────┐
  │  Phone   │ ────────────── │     ESP8285 AP       │
  │  / PC    │   192.168.4.x  │  ┌────────────────┐  │
  └──────────┘                │  │  DNS Server    │──│── all domains → 192.168.4.1
       │                      │  └────────────────┘  │
       │  Auto-redirect       │  ┌────────────────┐  │
       │  (captive portal)    │  │  HTTP Server   │──│── serves files from SPIFFS/SD
       ▼                      │  └────────────────┘  │
  ┌───────────────────┐      │  ┌────────────────┐  │
  │  🌐 index.html    │◄─────│  │  /beep handler │──│── POST → GPIO action (buzzer / relay / LED / …)
  │  + any static     │      │  └────────────────┘  │
  │  files from       │      └──────────────────────┘
  │  SPIFFS or SD     │
  └───────────────────┘
```

1. **ESP8285 boots** → scans for the best Wi-Fi channel → starts AP (APSTA mode)
2. **Client connects** → DNS intercepts all queries → redirects to `192.168.4.1` → web page opens automatically (captive portal)
3. **HTTP server** serves static files from SPIFFS and/or SD card, plus `/beep` action endpoint (trigger buzzer, relay, LED, or any GPIO action)
4. **Last client disconnects** → DNS + HTTP servers stop → resources freed

---

## 🔧 Hardware

| Component | Details |
|:----------|:--------|
| **MCU** | ESP8285 (ESP-01M or equivalent) |
| **Flash** | 1 MB — SPI, DOUT, 80 MHz |
| **Action Output** | GPIO4 — buzzer (active/passive PWM), or relay / LED / other |
| **SD Card** | Optional — HSPI: MOSI=GPIO13, MISO=GPIO12, SCLK=GPIO14, CS=GPIO5 |
| **Battery** | Optional — voltage divider R1=100k / R2=33k → ADC (TOUT) |

<details>
<summary>📷 Wiring Diagram</summary>

```
ESP-01M Pinout:
  ┌───────────┐
  │  VCC  TX  │─── GPIO1  (UART TX, unused in normal operation)
  │  RST  CH  │
  │  GPIO0 GPIO2│
  │  GPIO4 GPIO5│─── SD CS (optional)
  │  GND  3V3 │
  └───────────┘

Buzzer:
  Active:  GPIO4 ──┤>├── GND  (or via NPN transistor)
  Passive: GPIO4 ── Piezo ── GND  (PWM 2kHz)

SD Card (optional):
  GPIO13 (MOSI) ── DI
  GPIO12 (MISO) ── DO
  GPIO14 (SCLK) ── CLK
  GPIO5  (CS)   ── CS

Battery (optional):
  VBAT ── R1(100k) ──┬── TOUT (ADC)
  GND ── R2(33k)  ───┘
```

</details>

---

## 📁 Project Structure

```
esp8285_webserver/
├── CMakeLists.txt                  # Root CMake (ESP-IDF project)
├── sdkconfig.defaults              # Default menuconfig settings
├── partitions.csv                  # Partition table (NVS, factory, SPIFFS)
├── data_spiffs.bin                 # Pre-built SPIFFS image
├── data/
│   ├── index.html                  # Main web page
│   └── favicon.ico                 # Favicon
├── main/
│   ├── CMakeLists.txt              # Main component build
│   ├── Kconfig.projbuild           # Menuconfig options
│   ├── main.c                      # Entry point, context creation
│   ├── wifi_ap.c                   # Wi-Fi AP, channel scanning
│   ├── dns_server.c                # DNS server (captive portal auto-redirect)
│   ├── web_server.c                # HTTP server, MIME types, file serving
│   ├── orchestrator.c              # Server orchestrator (START/STOP/SHUTDOWN)
│   ├── buzzer.c                    # Action trigger (GPIO output), beep_task
│   ├── power_mgmt.c               # Battery, deep sleep, reset timer
│   ├── storage.c                   # SPIFFS and SD mounting
│   └── include/
│       ├── smart_ap_common.h       # Shared definitions, prototypes
│       └── smart_ap_config.h       # Configuration (Kconfig → #define)
└── components/
    └── sd_spi_driver/
        ├── CMakeLists.txt
        ├── Kconfig.projbuild       # SPI, CRC, SD timeout settings
        ├── diskio_sd_spi.c         # SD SPI driver + CMD42
        └── include/
            └── diskio_sd_spi.h     # SD driver API
```

---

## 🚀 Build & Flash

### Prerequisites

- [ESP8266 RTOS SDK](https://github.com/espressif/esp8266_rtos_sdk) (v3.4+ recommended)
- Python 3.8+
- CMake 3.5+

### Recommended Build Tools

| Tool | Description |
|:-----|:------------|
| [**ESP8266-IDF**](https://github.com/Dzantemir/ESP8266-IDF) | Standalone build environment — no need to manually install the SDK, toolchain, or Python dependencies |
| [**ESP8266-IDF** VSCode Extension](https://marketplace.visualstudio.com/items?itemName=Dzantemir.esp8266-idf) | One-click build, flash & monitor inside VS Code — project wizard, serial monitor, menuconfig GUI |

> 💡 **Tip:** Both tools handle SDK setup, toolchain installation, and PATH configuration automatically — just install and build.

### Quick Start

**Option A — Using ESP8266-IDF (CLI)**

```bash
# 1. Install ESP8266-IDF
git clone https://github.com/Dzantemir/ESP8266-IDF.git
cd ESP8266-IDF
./install.sh

# 2. Clone the project
git clone https://github.com/<username>/esp8285_webserver.git
cd esp8285_webserver

# 3. Build
idf.py build

# 4. Flash
idf.py -p /dev/ttyUSB0 flash

# 5. Monitor
idf.py -p /dev/ttyUSB0 monitor
```

**Option B — Using VSCode Extension**

1. Install the [ESP8266-IDF extension](https://marketplace.visualstudio.com/items?itemName=Dzantemir.esp8266-idf) from the VS Code Marketplace
2. Open the project folder in VS Code
3. Use the extension's commands: **Build** → **Flash** → **Monitor**

**Option C — Manual Setup**

```bash
# 1. Set up ESP8266 RTOS SDK environment
. $HOME/esp/ESP8266_RTOS_SDK/export.sh

# 2. Clone the project
git clone https://github.com/<username>/esp8285_webserver.git
cd esp8285_webserver

# 3. Build
idf.py build

# 4. Flash
idf.py -p /dev/ttyUSB0 flash

# 5. Monitor
idf.py -p /dev/ttyUSB0 monitor
```

### Rebuild SPIFFS Image

If you modified files in `data/`, rebuild the image:

```bash
idf.py spiffs
```

> **Note:** `data_spiffs.bin` is already included in the repo. It's linked to the `storage` partition via CMakeLists.txt and flashed automatically.

---

## ⚙️ Configuration

All settings are available via `idf.py menuconfig` → **SMART AP Configuration**.

<details>
<summary>📶 Wi-Fi</summary>

| Parameter | Default | Description |
|-----------|---------|-------------|
| SSID | `123` | Access Point name |
| Auth mode | OPEN | OPEN / WPA2_PSK / WPA_WPA2_PSK |
| Password | — | For WPA2, min 8 characters |
| SSID hidden | 0 | 0 = visible, 1 = hidden |
| Max connections | 4 | Max simultaneous clients (1–8) |
| Beacon interval | 250 ms | Beacon frame interval |
| Country code | RU | Affects channels & TX power |

</details>

<details>
<summary>🌐 AP Network</summary>

| Parameter | Default | Description |
|-----------|---------|-------------|
| IP address | 192.168.4.1 | Static AP IP |
| Netmask | 255.255.255.0 | Subnet mask |
| DHCP start | 192.168.4.2 | DHCP pool start |
| DHCP end | 192.168.4.10 | DHCP pool end |

</details>

<details>
<summary>🖥️ Servers</summary>

| Parameter | Default | Description |
|-----------|---------|-------------|
| DNS port | 53 | DNS server UDP port |
| DNS rate limit | 50 pkt/s | Max DNS queries per second |
| HTTPD stack | 4096 B | HTTP server task stack size |
| Max open sockets | 12 | Max simultaneously open sockets |
| Scratch buffer | 1024 B | Chunk buffer for file transfers |

</details>

<details>
<summary>💾 Storage</summary>

| Parameter | Default | Description |
|-----------|---------|-------------|
| Storage mode | SPIFFS only | SPIFFS / SD / SPIFFS+SD |
| SPIFFS partition | storage | SPIFFS partition label |
| SD CS GPIO | 5 | SD card chip select pin |
| SD password | — | Password for SD card unlock (CMD42) |

</details>

<details>
<summary>🔔 Action Trigger (Buzzer / Relay / LED)</summary>

The `/beep` endpoint triggers a GPIO output action. By default it drives a buzzer, but the same mechanism works for:

- 🔊 **Buzzer** — active or passive (PWM)
- 🔌 **Relay** — toggle a relay on GPIO4
- 💡 **LED** — blink an LED
- ⚡ **Any GPIO action** — customize `beep_task` for your needs

| Parameter | Default | Description |
|-----------|---------|-------------|
| Buzzer enabled | yes | Enable GPIO action output |
| Passive buzzer | no | Passive (PWM) / active |
| Buzzer GPIO | 4 | Output pin (buzzer, relay, LED, etc.) |
| Active high | 1 | 1 = HIGH=on, 0 = LOW=on (PNP) |
| Frequency | 2000 Hz | PWM frequency (passive buzzer only) |
| Beep duration | 45 sec | Total signal duration |

</details>

<details>
<summary>🔋 Battery</summary>

| Parameter | Default | Description |
|-----------|---------|-------------|
| Battery control | disabled | Enable battery monitoring |
| Critical voltage | 3700 mV | Deep sleep threshold |
| Startup voltage | 3900 mV | Min voltage for boot |
| Divider ratio | 5711 | Coefficient (R1=100k, R2=33k) |
| Sleep time | 30 min | Deep sleep duration |

</details>

---

## 🏗️ Architecture

### System Context

All modules share a single `sys_ctx_t` structure (global pointer `ctx`):

```
                        ┌─────────────────────┐
                        │      sys_ctx_t       │
                        │                      │
                        │  EventGroup ─────────│── state bits
                        │  Mutex ──────────────│── shared resource guard
                        │  Semaphores ─────────│── beep_sem, shutdown_sem
                        │  Queues ─────────────│── beep_queue, cmd_queue
                        │  Handles ────────────│── httpd, dns_socket, dns_task
                        └─────────────────────┘
                                   ▲
          ┌────────────────────────┼────────────────────────┐
          │                        │                        │
    ┌─────┴─────┐          ┌──────┴──────┐          ┌──────┴──────┐
    │  wifi_ap  │          │ orchestrator│          │   buzzer    │
    └───────────┘          └─────────────┘          └─────────────┘
    ┌───────────┐          ┌─────────────┐          ┌─────────────┐
    │dns_server │          │ web_server  │          │power_mgmt   │
    └───────────┘          └─────────────┘          └─────────────┘
    ┌───────────┐          ┌─────────────┐
    │ storage   │          │reset_timer  │
    └───────────┘          └─────────────┘
```

### Task Lifecycle

```
app_main()
  │
  ├── sys_ctx_create()              # Allocate FreeRTOS primitives
  ├── storage_init()                # Mount SPIFFS / SD card
  ├── WiFi init (STA → scan → APSTA)# Scan best channel → start AP
  │
  ├── server_orchestrator_task ─────# Serialize server START/STOP
  ├── reset_timer_task ──────────── # Periodic reboot (1–31 days)
  ├── smart_ap_task ─────────────── # Periodic channel re-scan
  ├── beep_task ─────────────────── # Action trigger processing (buzzer/relay/LED)
  └── battery_monitor_task ──────── # (optional) ADC monitoring
```

### Server Orchestrator

All DNS & HTTP server operations are serialized through a command queue:

| Command | Trigger | Action |
|---------|---------|--------|
| `SERVER_CMD_START` | First client connects | Start DNS + HTTP servers |
| `SERVER_CMD_STOP` | Last client disconnects | Stop DNS + HTTP servers |
| `SERVER_CMD_SHUTDOWN` | Reboot / deep sleep | Gracefully stop everything |
| `SERVER_CMD_DNS_ERROR` | DNS task crashed | Restart DNS (keep HTTP) |

> **Race protection:** Client count is double-checked before START/STOP to handle the race between connect/disconnect events and command processing.

### Graceful Shutdown

`free_proc()` ensures a clean shutdown sequence:

```
1. Set EVT_SHUTDOWN_BIT         ← all tasks check this
2. Send SERVER_CMD_SHUTDOWN     ← to orchestrator queue
3. Wait for all tasks (timeout) ← EVT_*_DONE bits
4. If hung → esp_restart()      ← safety net
5. Cleanup: WiFi, events, storage, NVS, ADC
```

---

## 💽 SD Card Driver

The custom `sd_spi_driver` component implements the full SD SPI protocol with **zero external dependencies**:

| Capability | Details |
|:-----------|:--------|
| Initialization | CMD0 (with retries), CMD8, ACMD41/CMD1 |
| Read / Write | Single and multi-block transfers |
| Erase (TRIM) | CMD32 + CMD33 + CMD38 |
| CRC | CRC7 for commands, CRC16 for data (optional read verification) |
| CMD42 | Lock, unlock, set/change/clear password, force erase |
| Thread safety | SPI bus mutex with priority inheritance |

### Driver Architecture

```
┌─────────────────────────────────────────┐
│  Level 2: VFS                           │
│  esp_vfs_fat_sd_spi_mount / unmount     │
│  (combines all levels in one call)      │
├─────────────────────────────────────────┤
│  Level 1: Disk                          │
│  sd_spi_diskio_register / unregister    │
│  (FatFS callbacks, CS GPIO, state)      │
├─────────────────────────────────────────┤
│  Level 0: SPI Bus                       │
│  sd_spi_bus_init / deinit               │
│  (HSPI hardware, mutex, clock config)   │
└─────────────────────────────────────────┘
```

---

## 📋 Partition Table

| Name | Type | Offset | Size |
|:-----|:-----|:-------|:-----|
| nvs | data | 0x9000 | 24 KB |
| phy_init | data | 0xF000 | 4 KB |
| factory | app | 0x10000 | 640 KB |
| storage (SPIFFS) | data | 0xB0000 | 52 KB |

---

## 📄 License

This project is licensed under the **MIT License** — see the [LICENSE](LICENSE) file for details.

---

<div align="center">

*Built with ❤️ using [ESP8266 RTOS SDK](https://github.com/espressif/esp8266_rtos_sdk) for ESP8285*

</div>
