SDCC     = sdcc
# -mz80              : Z80 target
# --no-std-crt0      : suppress z88dk/SDCC default CRT0; crt0.s provides ours
# --code-loc 0x0069  : _CODE section after the 0x0000..0x0068 vector area
# --data-loc 0x3F00  : BSS/DATA in cart RAM (wcmd_last_seq at 0x3F00, etc.)
# --no-xinit-opt     : do not emit XINIT section (no initialised data to copy)
SDCFLAGS = -mz80 --no-std-crt0 --code-loc 0x0069 --data-loc 0x3F00 \
           --no-xinit-opt

OBJCOPY  = objcopy
PYTHON   = python3

TARGET   = zxprog
ROM_SIZE = 16384

OUT_ZX_C = ../User/zx_image.c
BIN2ZX   = ./bin2zximage.py

.PHONY: all clean

all: $(OUT_ZX_C)

$(OUT_ZX_C): $(TARGET).bin
	$(PYTHON) $(BIN2ZX) $< $@

# Convert Intel HEX -> flat binary, padding to 16 KiB with 0xFF
$(TARGET).bin: $(TARGET).ihx
	$(OBJCOPY) -I ihex -O binary --gap-fill=0xFF \
	    --pad-to=$(ROM_SIZE) $< $@

# Compile + link:
#   Step 1: assemble crt0.s -> crt0.rel using sdasz80
#   Step 2: compile zxprog.c and link with crt0.rel -> zxprog.ihx
# crt0.rel must come first so its _CODE functions (startup, nmi_wrapper,
# zx_launcher) land at the start of the 0x0069 code window.
crt0.rel: crt0.s
	sdasz80 -l -o $@ $<

$(TARGET).ihx: crt0.rel zxprog.c zx.h
	$(SDCC) $(SDCFLAGS) crt0.rel zxprog.c -o $@

clean:
	rm -f $(TARGET).bin $(TARGET).ihx $(TARGET).map $(TARGET).lk \
	      $(TARGET).noi $(TARGET).sym $(TARGET).lst \
	      crt0.rel crt0.sym crt0.lst crt0.asm \
	      zxprog.rel zxprog.sym zxprog.lst zxprog.asm
