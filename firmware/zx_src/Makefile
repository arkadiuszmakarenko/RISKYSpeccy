Z88DK    ?= /home/makaron/z88dk
ZCC       = $(Z88DK)/bin/zcc
Z80ASM    = $(Z88DK)/bin/z88dk-z80asm
PYTHON    = python3

export Z88DK
export ZCCCFG := $(Z88DK)/lib/config
export PATH := $(Z88DK)/bin:$(PATH)

TARGET   = zxprog
ROM_SIZE = 16384
MONITOR_START ?= 0x3000

OUT_ZX_C = ../User/zx_image.c
BIN2ZX   = ./bin2zximage.py
PATCH_NMI = ./patch_nmi_vector.py

# Compile+link: startup=2 = ROM mode (no ROM calls, code at 0x0000, BSS in RAM)
CFLAGS = +zx -startup=2 -clib=default -SO3 \
         -subtype=bin \
		 -m \
		 -DMONITOR_START=$(MONITOR_START) \
         -L$(Z88DK)/libsrc

.PHONY: all clean

all: $(OUT_ZX_C)

$(OUT_ZX_C): $(TARGET).bin
	$(PYTHON) $(BIN2ZX) $< $@

$(TARGET).bin: zxprog.c
	$(ZCC) $(CFLAGS) -o $@ zxprog.c
	$(PYTHON) $(PATCH_NMI) $@ $(TARGET).map nmi_irq_wrapper

clean:
	rm -f $(TARGET).bin $(TARGET).map $(TARGET) $(TARGET).tap *.o *.opt
