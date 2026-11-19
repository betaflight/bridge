# Betaflight - USB to Wifi - Bridge

[![build](https://github.com/betaflight/bridge/actions/workflows/build.yml/badge.svg)](https://github.com/betaflight/bridge/actions/workflows/build.yml)

ESP32-S3 USB-host-to-WiFi bridge for Betaflight. The board acts as USB **host**
to a flight controller's Virtual COM Port (VCP) and exposes that serial stream
over TCP, so the Betaflight app can connect wirelessly from a phone or
laptop.

```
[FC USB VCP] <--USB host--> [ESP32-S3 mini] <--WiFi / TCP:5761--> [Betaflight app]
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
| `src/main/tcp_server.c` | TCP listener on 5761; one client at a time, newest connection wins |
| `src/main/ws_serial.c` | WebSocket serial endpoint (`/serial`) for browser clients — ws:// and wss:// |
| `src/main/ble_serial.c` | Bluetooth LE serial on the Nordic UART service, for phones without WiFi |
| `src/main/usb_download.c` | USB flash mode: reboots into the ROM loader on a long BOOT press or from the touch UI |
| `src/main/tls_cert.c` | Self-signed TLS cert generated on first boot, persisted in NVS |
| `src/main/bridge_mdns.c` | mDNS responder: `betaflight-bridge-<mac>.local`, `_betaflight._tcp` service |
| `src/main/wifi.c` | Station-first WiFi: joins a stored network, SoftAP fallback, creds in NVS |
| `src/main/http_status.c` | Web UI on 80 (HTTP) and 443 (HTTPS): status + scan/join + firmware upload + FC flashing + `/serial` |
| `src/main/ota.c` | `POST /update` OTA handler; streams an uploaded .bin into the spare slot |
| `src/main/bridge.c` | Two stream buffers decoupling USB and network; single-client arbiter (TCP vs WS vs flasher) |
| `src/main/fc_flash.c` | Flashes the FC: job state machine, phase progress, the `/dfu/*` routes |
| `src/main/dfu_host.c` | Second USB host client; DFU 1.1 + STM32 DfuSe over EP0 |
| `src/main/dfu_layout.c` | Parses the bootloader's memory layout out of its interface strings |
| `src/main/hex_parser.c` | Streaming Intel HEX reader; never holds the whole image |
| `src/main/fc_cli.c` | MSP reboot and CLI `diff all` backup / restore, over the byte bridge |
| `boards/<board>/` | Per-board component: flash size, partition table, PSRAM, identity and any board-specific hardware |
| `esp-idf/` | Pinned ESP-IDF (git submodule, `release/v5.5`, shallow) |

## Boards

The board is selected at configure time with `-DBOARD=<name>`, where `<name>` is
a directory under `boards/`. Only the selected board joins the build, as a
component of its own, providing `sdkconfig.defaults` (flash size, PSRAM,
partition CSV) and `board.h` (identity, LED pins), layered on the shared
top-level `sdkconfig.defaults`. Anything vendor-specific — panel drivers,
fonts, logos, managed components — stays there. The USB-host pins
(D- GPIO19 / D+ GPIO20) are fixed on the ESP32-S3 and identical across boards.

| BOARD | Flash | PSRAM | USB | Status LEDs |
|-------|-------|-------|-----|-------------|
| `esp32s3-zero` (default) | 4 MB | — | single (native) | NeoPixel (GPIO21) |
| `esp32s3-wroom-freenove` | 8 MB | 8 MB octal | dual (native + UART) | WiFi LED (GPIO2) + NeoPixel (GPIO48) |
| `esp32s3-touch-lcd-4b` | 16 MB | 8 MB octal | single (native) | 4" LCD touch UI |
| `esp32s3-hglrc-a1` | 4 MB | — | dual | NeoPixel (GPIO2) + 1.47" LCD |
| `esp32s3-touch-amoled-1-75c` | 32 MB | 8 MB octal | single (native) | 1.75" round AMOLED touch UI |
| `esp32s3-touch-amoled-1-8` | 16 MB | 8 MB octal | single (native) | 1.8" AMOLED touch UI |
| `esp32s3-touch-lcd-1-47` | 16 MB | 8 MB octal | single (native) | 1.47" LCD |

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

### `esp32s3-touch-lcd-4b` — Waveshare ESP32-S3-Touch-LCD-4B

- **MCU / memory:** ESP32-S3 N16R8 — 16 MB flash, 8 MB **octal** PSRAM
  (holds the LCD framebuffer).
- **Display:** 4.0" 480×480 IPS (ST7701, 16-bit parallel RGB565) with GT911
  capacitive touch, driven by the Waveshare BSP + LVGL
  (`CONFIG_BRIDGE_DISPLAY_TOUCH`). The screen shows the same status as the web
  page — FC link, app client, WiFi, IP — and offers on-screen WiFi
  scan/join/forget with a touch keyboard.
- **USB:** one USB-C, wired to the native ESP32-S3 USB (D- GPIO19 / D+ GPIO20),
  shared between flashing/console and the USB-host bridge — **the serial
  console drops out once host mode engages**. UART0 (TX GPIO43 / RX GPIO44) is
  broken out on the header for a persistent log console. Power the board from
  its DC terminal or battery input when the USB-C is hosting the FC. A LiPo on
  the battery input is read by the board's AXP2101 PMU, and its voltage and
  charge are shown on the status page and web UI
  (`CONFIG_BRIDGE_BATTERY_AXP2101`).
- **LEDs:** none used — status is on the LCD.
- **Partitions:** 16 MB dual-OTA, 6 MB per slot
  (`boards/esp32s3-touch-lcd-4b/partitions.csv`).

### `esp32s3-hglrc-a1` — HGLRC ESP32-S3-A1

- **MCU / memory:** ESP32-S3, 4 MB flash, no PSRAM.
- **Display:** 1.47" 172x320 IPS ST7789 display driven over SPI with LVGL
  (`CONFIG_BRIDGE_DISPLAY_COMPACT`). The compact status screen shows the FC and
  app links, WiFi state, BLE name suffix, connection addresses, a QR code for
  the web UI, and the battery voltage (the higher of the XT60 input on GPIO4 and the
  XT30 input on GPIO6).
- **USB:** one USB-C, wired to the native ESP32-S3 USB (D- GPIO19 / D+ GPIO20),
  shared between flashing/console and the USB-host bridge — **the serial
  console drops out once host mode engages**. UART0 is available on the header
  when a persistent log console is required.
- **LED:** the on-board WS2812 NeoPixel (GPIO2) indicates FC/OTG comms.
- **Partitions:** 4 MB dual-OTA, ~1.8 MB per slot
  (`boards/esp32s3-hglrc-a1/partitions.csv`).

### `esp32s3-touch-lcd-1-47` — [Waveshare ESP32-S3-Touch-LCD-1.47](https://www.waveshare.com/esp32-s3-touch-lcd-1.47.htm)

- **MCU / memory:** ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM. The `-M`
  variant (pre-soldered header) uses the same image.
- **Display:** 1.47" 172x320 IPS JD9853 display driven over SPI with LVGL
  (`CONFIG_BRIDGE_DISPLAY_COMPACT`), showing the same compact status screen as
  the HGLRC A1. The 1S LiPo voltage (GPIO12) is shown in the header. The
  AXS5106L touch controller is not used.
- **USB:** one USB-C, wired to the native ESP32-S3 USB (D- GPIO19 / D+ GPIO20),
  shared between flashing/console and the USB-host bridge — **the serial
  console drops out once host mode engages**. For a persistent log on the
  header UART0 (TX GPIO43 / RX GPIO44), build with
  `CONFIG_ESP_CONSOLE_UART_DEFAULT`.
- **Power:** the USB-C never sources VBUS. VBUS only feeds the board, through a
  diode, and on battery the 1S LiPo powers the board alone. When it hosts the
  FC, either the FC is powered on its own (flight battery), or 5 V is fed into
  the header VBUS pin (P1 pin 1), which is the same net as the USB-C VBUS.
- **LEDs:** none used — status is on the LCD.
- **Partitions:** 16 MB dual-OTA, 6 MB per slot
  (`boards/esp32s3-touch-lcd-1-47/partitions.csv`).

### `esp32s3-touch-amoled-1-75c` — [Waveshare ESP32-S3-Touch-AMOLED-1.75C](https://www.waveshare.com/esp32-s3-touch-amoled-1.75c.htm)

- **MCU / memory:** ESP32-S3R8 — 32 MB flash, 8 MB **octal** PSRAM.
- **Display:** 1.75" 466×466 round AMOLED (CO5300, QSPI) with CST9217
  capacitive touch, driven directly with LVGL
  (`CONFIG_BRIDGE_DISPLAY_TOUCH` + `CONFIG_BRIDGE_DISPLAY_ROUND`). Same status
  and WiFi scan/join/forget as the 4" touch UI, laid out for the circle as two
  swipeable pages.
- **USB:** one USB-C, wired to the native ESP32-S3 USB (D- GPIO19 / D+ GPIO20),
  shared between flashing/console and the USB-host bridge — **the serial
  console drops out once host mode engages**. Run it from a 3.7 V LiPo on the
  MX1.25 battery header (AXP2101 PMU, PWR button) when the USB-C is hosting
  the FC. The status page and web UI show the cell's voltage and charge
  (`CONFIG_BRIDGE_BATTERY_AXP2101`).
- **LEDs:** none — status is on the display.
- **Partitions:** 32 MB flash, dual-OTA with 6 MB per slot
  (`boards/esp32s3-touch-amoled-1-75c/partitions.csv`).

### `esp32s3-touch-amoled-1-8` — [Waveshare ESP32-S3-Touch-AMOLED-1.8](https://www.waveshare.com/esp32-s3-touch-amoled-1.8.htm)

- **MCU / memory:** ESP32-S3R8 — 16 MB flash, 8 MB **octal** PSRAM.
- **Display:** 1.8" 368×448 AMOLED (QSPI) with capacitive touch, driven
  directly with LVGL (`CONFIG_BRIDGE_DISPLAY_TOUCH`), using the rectangular
  tabbed layout of the 4" board. Two hardware revisions ship under this name,
  SH8601 + FT3168 and CO5300 + CST816; whether an FT3168 answers at boot
  selects the panel offset and touch driver. Panel and touch resets are on a TCA9554 I/O expander.
- **USB:** one USB-C, wired to the native ESP32-S3 USB (D- GPIO19 / D+ GPIO20),
  shared between flashing/console and the USB-host bridge — **the serial
  console drops out once host mode engages**. Run it from a 3.7 V LiPo on the
  MX1.25 battery header (AXP2101 PMU) when the USB-C is hosting the FC
  (`CONFIG_BRIDGE_BATTERY_AXP2101`).
- **LEDs:** none — status is on the display.
- **Partitions:** 16 MB flash, dual-OTA with 6 MB per slot
  (`boards/esp32s3-touch-amoled-1-8/partitions.csv`).

### Status LED behaviour

| LED | State | Meaning |
|-----|-------|---------|
| WiFi LED (GPIO, if present) | solid | joined a network as a station |
| | fast blink (~5 Hz) | associating |
| | slow blink (~1 Hz) | SoftAP setup mode / idle |
| NeoPixel (WS2812) | dim red | no FC attached |
| | amber | FC VCP open, idle |
| | green | FC + app (TCP) linked |
| | blue flash | bytes flowing to/from the FC |

### Adding a board

Create `boards/<name>/` with four files:

- `sdkconfig.defaults` — set `CONFIG_ESPTOOLPY_FLASHSIZE*`,
  `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="boards/<name>/partitions.csv"`, any
  PSRAM options, the identity (`CONFIG_APP_PROJECT_VER_FROM_CONFIG=y` +
  `CONFIG_APP_PROJECT_VER="<name>"`) and any `CONFIG_BRIDGE_*` features the
  board supports.
- `partitions.csv` — a dual-OTA table sized for the board's flash.
- `board.h` — `BOARD_NAME` and any LED pins (`BOARD_WIFI_LED_GPIO`,
  `BOARD_RGB_LED_GPIO`); a board may define neither LED.
- `CMakeLists.txt` — an `idf_component_register(INCLUDE_DIRS ".")` so the
  directory joins the build as a component. The selected board is the only one
  added, so anything vendor-specific — panel drivers, fonts, logos, managed
  components — belongs here rather than in `src/main`.

Boards with extra hardware may also add:

- `board_display.h` — declares `bsp_display_start/lock/unlock/backlight_on()`
  for `CONFIG_BRIDGE_DISPLAY_TOUCH` or `CONFIG_BRIDGE_DISPLAY_COMPACT`, either
  by including a vendor BSP header or by declaring a local driver.
- `CONFIG_BRIDGE_DISPLAY_ROUND` alongside `CONFIG_BRIDGE_DISPLAY_TOUCH` lays the
  touch UI out for a circular panel.
- `CONFIG_BRIDGE_BATTERY_AXP2101` reads the LiPo voltage and charge from an
  AXP2101 PMU and shows them on the display and web page. `board.h` must define
  `BOARD_PMU_I2C_PORT`, the I2C port the display code brings up with the PMU on
  it.
- For `CONFIG_BRIDGE_DISPLAY_COMPACT` only, `display.c` also expects a
  `board_logo.h` defining an `lv_image_dsc_t board_logo`, and the LVGL fonts
  `ui_font_size14` and `ui_font_size24`. Register their `.c` files and the
  directory holding `board_logo.h` in the board's `CMakeLists.txt`; see
  `boards/esp32s3-hglrc-a1/` for the layout.
- `idf_component.yml` — managed components only this board needs. Mark a
  dependency `public: true` when `board_display.h` exposes its types.
- `board.cmake` — build tweaks that can only run once every component is
  registered (patching a fetched BSP, say).

Then build with `make <name>` — it is picked up automatically (the board list is
read from `boards/`) and reconfigures cleanly when you switch boards.

## Setup

ESP-IDF is vendored as a submodule. After cloning this repo:

```sh
# Host prerequisites (Debian/Ubuntu): ESP-IDF's installer needs a working
# venv + pip to build its Python environment.
sudo apt install git wget cmake ninja-build python3-venv python3-pip libusb-1.0-0 libusb-1.0-0-dev

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

Each build writes two images to `dist/`. Switching boards triggers a clean
reconfigure, so you don't need to delete `sdkconfig` by hand.

| File | Offset | Use |
|------|--------|-----|
| `betaflight-bridge-<board>-<version>.bin` | `0x20000` | OTA update from the web UI |
| `betaflight-bridge-<board>-<version>-factory.bin` | `0x0` | first flash of a stock board |

The factory image is the app plus the bootloader, partition table and a blank
otadata/NVS, merged into one blob — everything a bare board needs. The plain
image is the app alone, which is what the OTA endpoint expects.

The `-<version>` suffix comes from `src/main/version.h` and is the same string
the firmware reports in the web UI; `make version` prints it. Examples below
use `2026.9.1` in place of the version.

### Versioning and releases

Calendar versioning, `YEAR.MONTH.PATCH` with an optional pre-release suffix,
the shape betaflight uses — but on the bridge's own release clock, so the
numbers will not line up with a betaflight release of the same name. master
carries the month of the next release with `PATCH` `0` (for example
`2026.12.0`); releases for that month are `.1`, `.2` and so on.

`src/main/version.h` is the only place it is written down. There is no way to
override it at build time: an image is whatever the committed header says, so a
filename can never contradict the firmware inside it.

To cut a release:

1. For the first release of a month, branch `YEAR.MONTH-maintenance` (for
   example `2026.12-maintenance`) off master, then move master on to the next
   release month by setting `BRIDGE_VERSION_YEAR` and `BRIDGE_VERSION_MONTH`
   there. Later releases for that month reuse the existing branch, so master
   always keeps `PATCH` `0`.
2. On the maintenance branch, set `BRIDGE_VERSION_PATCH` in
   `src/main/version.h` to the release number and merge that. Leave
   `BRIDGE_VERSION_SUFFIX` as `""` for a final. A release candidate keeps
   `PATCH` `0` and sets the suffix, e.g. `2026.12.0-rc1`.
3. Tag the merge commit with exactly `make version`, no `v` prefix, and push
   the tag:

   ```sh
   git tag "$(make version)" && git push origin "$(make version)"
   ```

4. `.github/workflows/release.yml` picks the tag up. It fails immediately if
   the tag disagrees with `src/main/version.h`, so a mistyped tag never becomes
   a release. Otherwise it builds every board and leaves a **draft** release
   carrying both images for each, with generated notes. A suffixed version is
   marked as a pre-release automatically; you don't have to tick the box.
5. Check the workflow succeeded and the draft carries both images for every
   board, review the notes, then publish it.

The tag is public once pushed, but the release stays a draft until step 5.

Release notes are GitHub's own generator, bucketed by the labels in
`.github/release.yml`. An unlabelled PR still appears, under "Changes".

To flash and monitor over serial, use `idf.py` directly:

```sh
idf.py -DBOARD=esp32s3-wroom-freenove -p /dev/ttyACM0 flash monitor
```

On the dual-USB Freenove, flash/monitor over the CH343 UART port — it stays
connected even after the firmware switches the native USB into host mode. On the
single-port ZERO the console drops once host mode engages.

## First flash from a browser

The factory image needs no toolchain and no clone — download
`betaflight-bridge-<board>-<version>-factory.bin` for your board from the
[releases page](../../releases) and flash it at offset `0x0`:

1. Open <https://espressif.github.io/esptool-js/> in a browser that supports Web
   Serial — Chrome/Edge 89+, Opera 76+, Firefox 151+, or Chrome on Android 152+.
   Safari does not support it.
2. **Connect**, and pick the board's port.
3. Set the offset to `0x0`, choose the factory `.bin`, then **Program**.

Or, with esptool installed locally:

```sh
esptool.py --chip esp32s3 write_flash 0x0 \
    betaflight-bridge-esp32s3-wroom-freenove-2026.9.1-factory.bin
```

Afterwards the board is updated over WiFi — see [Updating](#updating-ota).

**Pick the image matching your board.** All three boards are ESP32-S3, so a
flasher cannot tell them apart, but the flash sizes (4/8/16 MB) and partition
tables are not interchangeable. A mismatched image may fail to boot — hold
**BOOT** while resetting to get back into download mode and re-flash.

**A factory flash wipes stored settings.** The saved WiFi credentials and the
self-signed TLS certificate both live in NVS, which the merged image blanks. The
board comes back up in SoftAP mode as if new, and the browser certificate
exception has to be accepted again (see [Connecting](#connecting)).

To get a port the flasher can see, hold **BOOT** while resetting the board. On
the single-port ZERO and Touch-LCD-4B this is required once the firmware has
switched the native USB into host mode; on the Freenove use the CH343 UART port,
which is always available.

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
   immediately — the status panel shows the assigned IP and netmask. The SoftAP
   stays up for 30 s after the join so you can note the new address, then
   stops.
4. In the app's **Options**, turn on expert mode and **Enable manual
   connection mode**.
5. Pick **Manual Selection** and enter `tcp://192.168.4.1:5761` (or the
   station IP once joined).

After a network is stored, **subsequent boots join it directly as a station and
the SoftAP is not started** — reach the web UI and the app at the IP your
router assigns (shown on the page). If that network is ever unreachable at boot,
the SoftAP comes back up automatically so you can reconfigure, and stops again
30 s after the network is rejoined. Use *Forget* on
the page to clear the stored network and return to AP-only setup mode.

### Discovery (mDNS)

The bridge announces itself on the local network so the app (or any Bonjour /
Avahi browser) can find it without knowing the IP:

- hostname `betaflight-bridge-<mac6>.local` (last three bytes of the WiFi MAC,
  unique per unit; shown on the status page)
- service `_betaflight._tcp` on port 5761 with TXT records `tcp=5761`, `ws=80`,
  `wss=443`, `path=/serial`, `board=<board>`, `version=<firmware>`
- service `_http._tcp` on port 80 (the web UI)

```sh
avahi-browse -rt _betaflight._tcp
```

### Bluetooth LE

Every board also offers the serial stream over Bluetooth LE, so a phone can
connect without any WiFi. The bridge advertises the Nordic UART service
(`6E400001-B5A3-F393-E0A9-E50E24DCCA9E`) as **`Betaflight Bridge <mac6>`** (the
same suffix as the mDNS hostname; the GAP device name matches).

Clients disagree on which Nordic UART characteristic carries which direction, so
both `6E400002` and `6E400003` accept writes (with or without response) and both
notify: FC data goes out on whichever one the client subscribed to.
Notifications are split at the negotiated MTU (247 requested), and the bridge
asks for a 15-30 ms connection interval. There is no pairing.

A Bluetooth client takes the FC over when it subscribes, like a new TCP or
WebSocket connection, and is dropped when another client takes over. While the
FC is being flashed it is refused. The status bar on display boards shows the
Bluetooth mark (green with a client connected) and the client row says whether
the app came in over WiFi or Bluetooth.

### Connecting from a browser (WebSocket / WSS)

The desktop (Tauri) and Android (Capacitor) apps connect over the raw **TCP**
transport above. A **browser** can't open a raw TCP socket, so the bridge also
exposes the serial stream as a WebSocket at `/serial`:

- `ws://<ip>/serial` (port 80) — usable when the app page is served over plain
  HTTP (e.g. a local dev build).
- `wss://<ip>/serial` (port 443) — required by the hosted app at
  `app.betaflight.com` (an HTTPS page may only open a *secure* WebSocket).

In the app, enable expert mode, pick the manual connection option, and
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

After the first flash, firmware is updated over WiFi — no cable. On the web page
use **Firmware update**: pick the plain
`betaflight-bridge-<board>-<version>.bin` built for this board — *not* the
`-factory.bin`, which is a whole-flash image and is rejected — and hit
*Upload & reboot*. The image streams into the spare OTA slot,
the boot partition is switched, and the board restarts (~10 s); reconnect to the
page afterwards.

The layout is dual-OTA (`ota_0`/`ota_1`) with rollback enabled: a freshly
uploaded image boots in *pending-verify* state and only sticks once it comes up
healthy (`ota_mark_valid()` in `main.c`). A bad image that fails to boot is
rolled back to the previous slot automatically.

Both boards are `esp32s3`, so the image validator can't catch a wrong-*board*
upload (different partition layout). The board id baked into each image is
therefore checked on upload: an image for another board is rejected with a 400
and the boot partition is left untouched. The running board and slot are shown
on the status page.

> The partition table only takes effect from a flash over the wire, so the first
> flash — `idf.py flash` or the factory image from a browser — is the last one
> that needs the cable.

### USB flash mode

Boards with a single USB-C use it to host the FC, so it is not available to a PC
for flashing. **Hold BOOT for three seconds** (or hold *USB flash mode* on the
touch status page) and the bridge reboots into the ESP32-S3 ROM loader on that
port, with a notice on the display. Flash with esptool, erasing `otadata` so
the image in `ota_0` boots even if an earlier OTA left `ota_1` selected, and
leave with a watchdog reset (a plain RTS reset returns to the loader):

```sh
esptool.py --chip esp32s3 -p <PORT> --before no_reset --after no_reset \
  erase_region 0xf000 0x2000
esptool.py --chip esp32s3 -p <PORT> --before no_reset --after watchdog_reset \
  write_flash 0x20000 dist/betaflight-bridge-<board>-<version>.bin
```

RESET also leaves USB flash mode.

## Flashing the flight controller

The **Flash flight controller** card on the web page flashes the FC itself over
USB DFU — no cable, no app. Pick a Betaflight `.hex`, choose the
options, and the bridge does the rest:

| Phase | What happens |
| --- | --- |
| Back up config | Enters the CLI and captures `diff all` |
| Reboot to bootloader | CLI `bl`, then waits for the FC to reappear as a DFU device |
| Identify target | Reads the memory layout and transfer size from the bootloader |
| Erase | Every page, or just the ones the image touches |
| Write | Streams the hex straight to flash |
| Verify | Reads it all back and compares |
| Start firmware | Leaves DFU |
| Restore backup | Replays the saved config and saves it |

Each phase reports its own progress, and a failure marks the phase it happened
in rather than a generic error.

**The backup matters.** On targets like the H743 the config sector sits inside
the flashed range, so every DFU flash wipes the configuration. Leave *Back up
config* ticked; the page downloads the capture to your browser as soon as it
exists, because the bridge only holds it in RAM. *Restore backup* replays it
once the new firmware has booted.

Flashing takes the FC exclusively: a connected app is dropped and new
connections are refused, with a reason, until it finishes.

Only Intel HEX is supported today. `.bin` and `.uf2`, and flashing boards whose
bootloader presents as mass storage rather than DFU, are not implemented yet.

If the FC is already sitting in its bootloader the backup and reboot phases are
skipped, so a board left in DFU by a failed attempt can simply be flashed again.
A failure during erase, write or verify leaves it in DFU and recoverable, no
power cycle needed. A backup failure happens before the bootloader is entered
and a restore failure after it has been left, so neither leaves it in DFU.

### Endpoints

Driven by the page, but usable directly:

| Route | Purpose |
| --- | --- |
| `POST /dfu/start` | Begin a job. Form-encoded `size`, `backup`, `erase_all`, `verify`, `restore`. 409 if one is already running |
| `POST /dfu/data` | The next slice of the hex; returns the progress snapshot |
| `GET /dfu/status` | Progress snapshot as JSON |
| `GET /dfu/backup` | The captured `diff all`, as text |
| `POST /dfu/abort` | Cancel the running job |

The image is never buffered whole — the 4 MB boards have neither a spare
partition nor the PSRAM for it — so the browser uploads it in slices and TCP
backpressure paces the upload to whatever speed the FC's flash can take.

## Notes

- Known FC VCP USB IDs (ST / Artery / Geehy) are listed in `usb_cdc_host.c`;
  add new vendors there.
- Single client at a time — one connection, shared across the TCP and
  WebSocket transports. The newest connection wins: connecting from another
  device (or a browser reconnecting) drops the current client.
- `TCP_NODELAY` is set and Nagle effectively disabled to keep MSP latency low.

## Licence

GPL-3.0, matching Betaflight. See [LICENSE](LICENSE).
