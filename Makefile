SHELL       := /bin/bash
IDF_PATH    := $(HOME)/source/esp-idf/v6.0
IDF_EXPORT  := source $(IDF_PATH)/export.sh
BUILD_DIR   := build/debug
ELF         := bin/debug/magic_keyboard.elf
# Resolved on every use, not at parse time, so a board attached by `attach` is picked up
SERIAL_PORT  = $(shell ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | head -n1)
CAPTURE_SEC := 10
LOG         := $(BUILD_DIR)/device.log

# WSL only: the CP2104 is passed through from Windows with usbipd-win
USB_HWID    := 10c4:ea60
USBIPD      := /mnt/c/Program Files/usbipd-win/usbipd.exe

.PHONY: build clean flash app-flash run monitor log config decode-log release-port attach

build:
	$(IDF_EXPORT) && idf.py -B $(BUILD_DIR) build

clean:
	rm -rf build bin gen out .cache tmp*idf_* sdkconfig sdkconfig.old managed_components

attach:
	@if [ -z "$(SERIAL_PORT)" ] && [ -x "$(USBIPD)" ]; then \
		"$(USBIPD)" attach --wsl --hardware-id $(USB_HWID) && \
		for i in $$(seq 20); do [ -n "$$(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null)" ] && break; sleep 0.5; done; \
	fi

# Without a port `lsof -t` lists every process, so bail out before it gets there
release-port: attach
	@test -n "$(SERIAL_PORT)" || { echo "No serial port found, is the board plugged in?"; exit 1; }
	@lsof -t $(SERIAL_PORT) 2>/dev/null | xargs -r kill 2>/dev/null; true

flash: release-port
	$(IDF_EXPORT) && idf.py -B $(BUILD_DIR) -p $(SERIAL_PORT) flash

app-flash: release-port
	$(IDF_EXPORT) && idf.py -B $(BUILD_DIR) -p $(SERIAL_PORT) app-flash

run: flash monitor

monitor: release-port
	$(IDF_EXPORT) && exec idf.py -B $(BUILD_DIR) -p $(SERIAL_PORT) monitor --no-reset

config:
	$(IDF_EXPORT) && idf.py -B $(BUILD_DIR) menuconfig

log: release-port
	@stty -F $(SERIAL_PORT) 115200 cs8 -cstopb -parenb raw 2>/dev/null; \
	(cat $(SERIAL_PORT) > $(LOG) 2>&1 & PID=$$!; sleep $(CAPTURE_SEC); kill $$PID 2>/dev/null; wait $$PID 2>/dev/null; true)
	@echo "Captured $(CAPTURE_SEC)s of serial output to $(LOG)"

# Decodes log from log targets
decode-log:
	@$(IDF_EXPORT) && python3 scripts/decode_log.py $(LOG) $(ELF)
