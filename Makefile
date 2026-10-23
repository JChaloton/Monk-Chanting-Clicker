# chant-clicker — CH32V003 player firmware
#
# Uses ch32v003fun. Get it with either:
#   git submodule add https://github.com/cnlohr/ch32v003fun ch32v003fun
# or drop a checkout at ../ch32v003fun and set CH32V003FUN below.
#
# Flashing needs a WCH-LinkE (minichlink, which ships with ch32v003fun).

TARGET      := chant
CH32V003FUN ?= ch32v003fun/ch32fun
MINICHLINK  ?= ch32v003fun/minichlink

TARGET_MCU  := CH32V003
ADDITIONAL_C_FILES := src/main.c src/audio.c src/spiflash.c
CFLAGS_EXTRA := -Isrc

# ch32v003fun ships a ready-made rules file; this is the whole build.
include $(CH32V003FUN)/ch32fun.mk

.PHONY: flash load load-swio monitor clean-all

flash: $(TARGET).bin
	$(MINICHLINK)/minichlink -w $< flash -b

# The whole audio-load cycle in one command: loader -> MCU, audio -> SPI flash,
# player -> MCU. Override the file or port from the command line:
#   make load CHANT=other.raw PORT=COM9
# PORT may be left unset; load_audio.py auto-detects it.
CHANT ?= chant8k-limit.raw
PORT  ?=

load:
	python tools/load_audio.py $(CHANT) $(if $(PORT),--port $(PORT),)

# The same cycle with no UART at all: the WCH-LinkE's SWIO wire carries the
# audio too. This is the route for boards that have nothing on PD5/PD6.
#   make load-swio CHANT=other.raw
load-swio:
	python tools/load_audio_swio.py $(CHANT)

# Debug printf over PD5 (UART TX) once you wire a USB-serial dongle.
monitor:
	$(MINICHLINK)/minichlink -T

clean-all: clean
	rm -f $(TARGET).bin $(TARGET).elf $(TARGET).map
