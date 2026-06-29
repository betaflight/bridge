# Betaflight - USB to Wifi - Bridge

[![build](https://github.com/betaflight/bridge/actions/workflows/build.yml/badge.svg)](https://github.com/betaflight/bridge/actions/workflows/build.yml)

ESP32-S3 USB-host-to-WiFi bridge for Betaflight. The board acts as USB **host**
to a flight controller's Virtual COM Port (VCP) and exposes that serial stream
over TCP, so Betaflight Configurator can connect wirelessly from a phone or
laptop.

```
[FC USB VCP] <--USB host--> [ESP32-S3 mini] <--WiFi / TCP:5761--> [Configurator]
```

It is a transparent byte bridge — no MSP parsing happens on the ESP32.

## Hardware

- **ESP32-S3** mini (the S3's native USB-OTG peripheral is required for USB host).
- A USB-A host port / OTG adapter wired to the S3 OTG pins (D+ GPIO20, D- GPIO19).
- 5 V supply able to power both the ESP32 and the attached FC.

## Layout

| Path | Role |
|------|------|
| `src/main/main.c` | Startup: NVS, bridge, WiFi, TCP server, USB host |
| `src/main/usb_cdc_host.c` | USB host + CDC-ACM; opens the FC VCP, pumps bytes |
| `src/main/tcp_server.c` | TCP listener on 5761; one Configurator client at a time |
| `src/main/ws_serial.c` | WebSocket serial endpoint (`/serial`) for browser clients — ws:// and wss:// |
| `src/main/tls_cert.c` | Self-signed TLS cert generated on first boot, persisted in NVS |
| `src/main/wifi.c` | Station-first WiFi: joins a stored network, SoftAP fallback, creds in NVS |
| `src/main/http_status.c` | Web UI on 80 (HTTP) and 443 (HTTPS): status + scan/join + firmware upload + `/serial` |
| `src/main/ota.c` | `POST /update` OTA handler; streams an uploaded .bin into the spare slot |
| `src/main/bridge.c` | Two stream buffers decoupling USB and network; single-client arbiter (TCP vs WS) |
| `boards/<board>/` | Per-board flash size, partition table, PSRAM and identity |
| `esp-idf/` | Pinned ESP-IDF (git submodule, `release/v5.4`, shallow) |

## Boards

The board is selected at configure time with `-DBOARD=<name>`, where `<name>` is
a directory under `boards/`. Each board provides its own `sdkconfig.defaults`
(flash size, PSRAM, partition CSV) and `board.h` (identity, LED pins), layered
on the shared top-level `sdkconfig.defaults`. The USB-host pins
(D- GPIO19 / D+ GPIO20) are fixed on the ESP32-S3 and identical across boards.

| BOARD | Flash | PSRAM | USB | Status LEDs |
|-------|-------|-------|-----|-------------|
| `esp32s3-zero` (default) | 4 MB | — | single (native) | NeoPixel (GPIO21) |
| `esp32s3-wroom-freenove` | 8 MB | 8 MB octal | dual (native + UART) | WiFi LED (GPIO2) + NeoPixel (GPIO48) |

A board identity (`BOARD_NAME`) is baked into each image (`esp_app_desc.version`)
and checked on OTA, so an image built for one board is refused on another (see
[Updating](#updating-ota)).

### `esp32s3-zero` — Waveshare ESP32-S3-ZERO

- **MCU / memory:** ESP32-S3, 4 MB flash, no PSRAM.
- **USB:** one USB-C, wired to the native ESP32-S3 USB (D- GPIO19 / D+ GPIO20).
  That single port is shared between flashing/console (USB-Serial-JTAG) and the
  USB-host bridge — so **the serial console drops out once host mode engages**.
  Re-flash over the air (OTA) or force download mode (hold BOOT while resetting).
- **LED:** the on-board WS2812 NeoPixel (GPIO21) indicates FC/OTG comms. There is
  no separate WiFi LED on this board.
- **Partitions:** 4 MB dual-OTA, ~1.875 MB per slot
  (`boards/esp32s3-zero/partitions.csv`).

### `esp32s3-wroom-freenove` — Freenove ESP32-S3-WROOM (N8R8, v1.1)

- **MCU / memory:** ESP32-S3-WROOM-1 N8R8 — 8 MB flash, 8 MB **octal** PSRAM
  (`CONFIG_SPIRAM` enabled).
- **USB:** two USB-C ports. One is the native ESP32-S3 USB (D- GPIO19 /
  D+ GPIO20) used for the USB-host bridge; the other goes through a **WCH CH343**
  UART bridge (USB `1a86:55d3`, enumerates as `/dev/ttyACM*`) on UART0
  (TX GPIO43 / RX GPIO44). Flash and monitor over the CH343 port — **the console
  stays up even while the native port is in host mode**.
- **LEDs (v1.1):**
  - **GPIO2** — plain LED, WiFi state (blink cadence). Assumed active-high;
    uncomment `BOARD_WIFI_LED_ACTIVE_LOW` in the board's `board.h` to invert.
  - **GPIO48** — on-board WS2812 NeoPixel, FC/OTG comms.
- **Partitions:** 8 MB dual-OTA, ~3 MB per slot
  (`boards/esp32s3-wroom-freenove/partitions.csv`).

### Status LED behaviour

| LED | State | Meaning |
|-----|-------|---------|
| WiFi LED (GPIO, if present) | solid | joined a network as a station |
| | fast blink (~5 Hz) | associating |
| | slow blink (~1 Hz) | SoftAP setup mode / idle |
| NeoPixel (WS2812) | dim red | no FC attached |
| | amber | FC VCP open, idle |
| | green | FC + Configurator (TCP) linked |
| | blue flash | bytes flowing to/from the FC |

### Adding a board

Create `boards/<name>/` with three files:

- `sdkconfig.defaults` — set `CONFIG_ESPTOOLPY_FLASHSIZE*`,
  `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="boards/<name>/partitions.csv"`, any
  PSRAM options, and the identity
  (`CONFIG_APP_PROJECT_VER_FROM_CONFIG=y` + `CONFIG_APP_PROJECT_VER="<name>"`).
- `partitions.csv` — a dual-OTA table sized for the board's flash.
- `board.h` — `BOARD_NAME` and any LED pins (`BOARD_WIFI_LED_GPIO`,
  `BOARD_RGB_LED_GPIO`); a board may define neither LED.

Then build with `make <name>` — it is picked up automatically (the board list is
read from `boards/`) and reconfigures cleanly when you switch boards.

## Setup

ESP-IDF is vendored as a submodule. After cloning this repo:

```sh
# Host prerequisites (Debian/Ubuntu): ESP-IDF's installer needs a working
# venv + pip to build its Python environment.
sudo apt install git wget cmake ninja-build python3-venv python3-pip

# Fetch the pinned ESP-IDF submodule and install its toolchain.
make esp_tools
```

`make esp_tools` is idempotent — re-run it to pull toolchain updates. `make`
sources the vendored ESP-IDF automatically, so you don't need to
`. ./esp-idf/export.sh` for a build (do that only when calling `idf.py`
directly, e.g. to flash over serial).

## Build & flash

A `make` wrapper around `idf.py` handles board selection, reconfigures
automatically when you switch boards, and writes a per-board image to `dist/`.
It sources the vendored ESP-IDF (`./esp-idf/export.sh`) for you, so a plain
`make <board>` works straight after `install.sh`.

```sh
make                        # list the available boards (read from boards/)
make esp32s3-wroom-freenove # build the image for a board
make clean                  # remove build/, dist/ and sdkconfig
```

Each build writes `dist/betaflight-bridge-<board>.bin` — the image used for both
serial flashing and OTA. Switching boards triggers a clean reconfigure, so you
don't need to delete `sdkconfig` by hand.

To flash and monitor over serial, use `idf.py` directly:

```sh
idf.py -DBOARD=esp32s3-wroom-freenove -p /dev/ttyACM0 flash monitor
```

On the dual-USB Freenove, flash/monitor over the CH343 UART port — it stays
connected even after the firmware switches the native USB into host mode. On the
single-port ZERO the console drops once host mode engages.

## Connecting

On first boot — or whenever no network has been configured — the board brings
up its own SoftAP so you can set it up:

1. Power the board with the FC plugged into the host port.
2. Join the WiFi network **`betaflight-bridge`** (default password `betaflight`).
   The board runs a DHCP server, so you'll get a `192.168.4.x` lease
   automatically with `192.168.4.1` as the gateway.
3. Browse to `http://192.168.4.1/`. The page shows live USB/TCP/WiFi status and
   lets you **scan for and join your home network**: pick an SSID (or type one),
   enter the password, and hit *Join*. Credentials are saved to NVS and applied
   immediately — the status panel shows the assigned IP and netmask.
4. In Configurator choose the **TCP** connection, host `192.168.4.1`, port
   `5761` (or the station IP once joined).

After a network is stored, **subsequent boots join it directly as a station and
the SoftAP is not started** — reach the web UI and Configurator at the IP your
router assigns (shown on the page). If that network is ever unreachable at boot,
the SoftAP comes back up automatically so you can reconfigure. Use *Forget* on
the page to clear the stored network and return to AP-only setup mode.

### Connecting from a browser (WebSocket / WSS)

The desktop (Tauri) and Android (Capacitor) apps connect over the raw **TCP**
transport above. A **browser** can't open a raw TCP socket, so the bridge also
exposes the serial stream as a WebSocket at `/serial`:

- `ws://<ip>/serial` (port 80) — usable when the app page is served over plain
  HTTP (e.g. a local dev build).
- `wss://<ip>/serial` (port 443) — required by the hosted app at
  `app.betaflight.com` (an HTTPS page may only open a *secure* WebSocket).

In Configurator, enable expert mode, pick the manual connection option, and
enter the URL (the status page shows the exact `wss://<ip>/serial` to use).

**Certificate acceptance (one-time).** The TLS server uses a **self-signed**
certificate generated on the device's first boot and stored in NVS, so it is
stable across reboots and OTA updates. Because it isn't from a public CA, a
browser will refuse the `wss://` connection until the certificate is trusted:
**visit `https://<ip>/` once and click through the warning** ("Proceed to …").
The browser then remembers the exception and `wss://<ip>/serial` connects from
then on — including from `app.betaflight.com`. You only need to do this again if
the bridge's IP changes or the certificate is cleared (e.g. an NVS erase).

## Updating (OTA)

After the first serial flash, firmware is updated over WiFi — no cable. On the
web page use **Firmware update**: pick the `dist/betaflight-bridge-<board>.bin`
built for this board and hit *Upload & reboot*. The image streams into the spare OTA slot, the boot partition
is switched, and the board restarts (~10 s); reconnect to the page afterwards.

The layout is dual-OTA (`ota_0`/`ota_1`) with rollback enabled: a freshly
uploaded image boots in *pending-verify* state and only sticks once it comes up
healthy (`ota_mark_valid()` in `main.c`). A bad image that fails to boot is
rolled back to the previous slot automatically.

Both boards are `esp32s3`, so the image validator can't catch a wrong-*board*
upload (different partition layout). The board id baked into each image is
therefore checked on upload: an image for another board is rejected with a 400
and the boot partition is left untouched. The running board and slot are shown
on the status page.

> The dual-OTA partition table only takes effect from a **serial** flash, so the
> initial `idf.py ... flash` is the last one that needs the cable.

## Notes

- Known FC VCP USB IDs (ST / Artery / Geehy) are listed in `usb_cdc_host.c`;
  add new vendors there.
- Single client at a time — one Configurator connection, shared across the TCP
  and WebSocket transports (a new browser connection supersedes a stale one; a
  raw-TCP client in progress is left alone).
- `TCP_NODELAY` is set and Nagle effectively disabled to keep MSP latency low.

## Licence

GPL-3.0, matching Betaflight. See [LICENSE](LICENSE).
