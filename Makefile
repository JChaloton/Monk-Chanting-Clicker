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

.PHONY: flash monitor clean-all

flash: $(TARGET).bin
	$(MINICHLINK)/minichlink -w $< flash -b

# Debug printf over PD5 (UART TX) once you wire a USB-serial dongle.
monitor:
	$(MINICHLINK)/minichlink -T

clean-all: clean
	rm -f $(TARGET).bin $(TARGET).elf $(TARGET).map
