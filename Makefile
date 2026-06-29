# betaflight-bridge — convenience wrapper around ESP-IDF's idf.py.
#
#   make                 show this help (the board list is read from boards/)
#   make esp_tools       fetch the ESP-IDF submodule and install the toolchain
#   make <board>         build the flash/OTA image for <board>
#   make <board> VERSION=x.y.z   stamp that version into the image (else default)
#   make clean           remove build artefacts
#
# It sources the vendored ESP-IDF (./esp-idf/export.sh) automatically if idf.py
# is not already on PATH, so a plain `make <board>` works after `make esp_tools`.

SHELL := /bin/bash

# The Espressif chip target everything is built for.
IDF_TARGET := esp32s3

# CMake project() name: the build artefact is build/$(PROJECT).bin and the
# published per-board image is dist/$(PROJECT)-<board>.bin.
PROJECT := betaflight-bridge

# Firmware version. Single source of truth is the BRIDGE_VERSION #define in
# src/main/version.h; we read it so an unversioned `make <board>` still stamps
# and names the artefact with the coded default. Override with `make <board>
# VERSION=x.y.z` (the release workflow passes the tag).
VERSION ?= $(shell sed -n 's/^[[:space:]]*#define[[:space:]]\+BRIDGE_VERSION[[:space:]]\+"\(.*\)".*/\1/p' src/main/version.h)

# Discover boards from the directory list — never hardcoded. Anything under
# boards/ with an sdkconfig.defaults is a valid target.
BOARDS := $(sort $(notdir $(patsubst %/sdkconfig.defaults,%,$(wildcard boards/*/sdkconfig.defaults))))

# The default board is whatever CMakeLists.txt defaults BOARD to.
DEFAULT_BOARD := $(shell grep 'BOARD "' CMakeLists.txt | head -1 | cut -d'"' -f2)

# Remembers the last board built so a board switch triggers a clean reconfigure
# (flash size / partitions / PSRAM differ between boards).
LAST_BOARD := build/.last_board

.DEFAULT_GOAL := help
.PHONY: help list esp_tools clean $(BOARDS)

help:
	@echo "betaflight-bridge — ESP32-S3 USB-host-to-WiFi bridge"
	@echo ""
	@echo "Usage:"
	@echo "  make esp_tools build/update the ESP-IDF submodule and toolchain"
	@echo "  make <board>   build the flash/OTA image for <board>"
	@echo "  make clean     remove build artefacts"
	@echo "  make help      show this message"
	@echo ""
	@echo "Available boards:"
	@for b in $(BOARDS); do \
	  if [ "$$b" = "$(DEFAULT_BOARD)" ]; then \
	    printf "  %-28s (default)\n" "$$b"; \
	  else \
	    printf "  %s\n" "$$b"; \
	  fi; \
	done
	@echo ""
	@echo "The build image is written to dist/$(PROJECT)-<board>.bin"

# Bare board names (machine-readable, one per line) for scripting/CI.
list:
	@for b in $(BOARDS); do echo "$$b"; done

# One-time setup: fetch the pinned ESP-IDF submodule and install its toolchain.
# Re-running it is safe — the submodule is fast-forwarded and install.sh is
# idempotent. The toolchain install needs git, wget, cmake, ninja and a working
# python3 venv on PATH (Debian/Ubuntu: apt install git wget cmake ninja-build
# python3-venv python3-pip).
esp_tools:
	@set -e; \
	if [ ! -f esp-idf/install.sh ]; then \
	  echo "==> Fetching ESP-IDF submodule"; \
	  git submodule update --init --depth 1 esp-idf; \
	fi; \
	echo "==> Installing the ESP-IDF $(IDF_TARGET) toolchain"; \
	./esp-idf/install.sh $(IDF_TARGET); \
	echo ""; \
	echo "==> ESP-IDF ready. Build with: make <board>"

# Published image name: dist/<project>-<board>[-<VERSION>].bin (version appended
# when VERSION is set, e.g. by the release workflow). Sanitise the version for
# the filename only — release tags may contain '/' (e.g. release/v1.2.3), which
# would otherwise turn the cp target into a nested path. -DBRIDGE_VERSION still
# receives the original VERSION, so the UI shows the tag verbatim.
VERSION_FILENAME := $(subst /,-,$(VERSION))
$(BOARDS): IMG = $(PROJECT)-$@$(if $(VERSION_FILENAME),-$(VERSION_FILENAME))

$(BOARDS):
	@echo "==> Building $(PROJECT) for board: $@"
	@set -e; \
	if ! command -v idf.py >/dev/null 2>&1; then \
	  if [ ! -f esp-idf/export.sh ]; then \
	    echo "ESP-IDF not found. Run first-time setup:"; \
	    echo "  make esp_tools"; \
	    exit 1; \
	  fi; \
	  . ./esp-idf/export.sh >/dev/null; \
	fi; \
	if [ "$$(cat $(LAST_BOARD) 2>/dev/null)" != "$@" ]; then \
	  echo "==> Board changed (or first build); reconfiguring"; \
	  rm -f sdkconfig; \
	  idf.py fullclean >/dev/null 2>&1 || true; \
	  idf.py -DBOARD=$@ set-target $(IDF_TARGET); \
	fi; \
	idf.py -DBOARD=$@ $(if $(VERSION),-DBRIDGE_VERSION=$(VERSION),) build; \
	mkdir -p dist; \
	cp build/$(PROJECT).bin dist/$(IMG).bin; \
	echo "$@" > $(LAST_BOARD); \
	echo ""; \
	echo "==> Done. Flash/OTA image: dist/$(IMG).bin"; \
	echo "    Serial flash:  idf.py -DBOARD=$@ -p <PORT> flash monitor"; \
	echo "    OTA:           upload dist/$(IMG).bin from the web UI"

clean:
	@if command -v idf.py >/dev/null 2>&1; then \
	  idf.py fullclean >/dev/null 2>&1 || true; \
	elif [ -f esp-idf/export.sh ]; then \
	  ( . ./esp-idf/export.sh >/dev/null && idf.py fullclean >/dev/null 2>&1 ) || true; \
	fi; \
	rm -rf build dist sdkconfig sdkconfig.old
	@echo "==> Cleaned build/, dist/, sdkconfig"
