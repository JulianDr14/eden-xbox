# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the Pulse phase-3 audiovisual payload. Deliberately standalone rather than derived from the
# devkitPro application template: this is one translation unit with no assets, and the template's
# machinery (romfs, nacp, icon, build dir) would only obscure it.
#
# Requires devkitPro with switch-dev. From this directory:
#     C:\devkitPro\msys2\usr\bin\make.exe
# The result, pulse_av.nro, is what tools/xbox/package-appx.ps1 takes as -BootNro for the Pulse phase-3 gate.

# GCC needs a writable directory for its temporaries. msys2's make hands the recipe an
# environment with TMP/TEMP/TMPDIR all unset, so GCC on Windows falls back to GetTempPath(),
# which resolves to C:\Windows - not writable, and the compile dies with a confusing
# "Cannot create temporary file". Point it somewhere it can actually write.
ifeq ($(strip $(TMPDIR)),)
export TMPDIR := $(CURDIR)
endif

DEVKITPRO ?= C:/devkitPro
# libnx's switch.specs expands $DEVKITPRO itself, so it has to reach GCC as a real
# environment variable, not just a make variable.
export DEVKITPRO
LIBNX     := $(DEVKITPRO)/libnx
CC        := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-gcc
ELF2NRO   := $(DEVKITPRO)/tools/bin/elf2nro

# Matches the devkitPro switch template: Cortex-A57 (the Switch's CPU), soft thread pointer, PIE.
ARCH    := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS  := -g -Wall -O2 -ffunction-sections $(ARCH) -I$(LIBNX)/include -D__SWITCH__
LDFLAGS := -specs=$(LIBNX)/switch.specs -g $(ARCH)
LIBS    := -L$(LIBNX)/lib -lnx -lm

TARGET := pulse_av

all: $(TARGET).nro

$(TARGET).elf: main.c
	$(CC) $(CFLAGS) $(LDFLAGS) $< $(LIBS) -o $@

$(TARGET).nro: $(TARGET).elf
	$(ELF2NRO) $< $@

clean:
	rm -f $(TARGET).elf $(TARGET).nro

.PHONY: all clean
