# Root firmware build (independent from firmware/obj).
#
# Toolchain requirements:
#   1) WCH RISC-V GCC toolchain (for CH32 firmware)
#      - Expected via TOOLCHAIN_DIR (default below)
#      - Required binaries in $(TOOLCHAIN_DIR)/bin:
#          riscv-wch-elf-gcc, riscv-wch-elf-objcopy,
#          riscv-wch-elf-objdump, riscv-wch-elf-size
#   2) ZX ROM build tools (for firmware/zx_src -> firmware/User/zx_image.c)
#      - sdcc
#      - sdasz80
#      - objcopy (GNU binutils)
#      - od, awk (for bin -> C array conversion)
#
# Usage:
#   make
#   make TOOLCHAIN_DIR=/path/to/RISC-V_Embedded_GCC12
#   make clean

TOOLCHAIN_DIR ?= /home/makaron/RISC-V_Embedded_GCC12
TOOLCHAIN_BIN := $(TOOLCHAIN_DIR)/bin
TOOL_PREFIX := riscv-wch-elf-

# MounRiver Studio toolchain root (used for OpenOCD flashing).
# Override on the command line if installed elsewhere:
#   make flash MRS_TOOLCHAIN_ROOT=/path/to/MRS_Toolchain_Linux_x64_V1.91
MRS_TOOLCHAIN_ROOT ?= /usr/share/MRS2/MRS-linux-x64/resources/app/resources/linux/components/WCH/OpenOCD
OPENOCD          := $(MRS_TOOLCHAIN_ROOT)/OpenOCD/bin/openocd
OPENOCD_CFG      := $(MRS_TOOLCHAIN_ROOT)/OpenOCD/bin/wch-riscv.cfg

CC      := $(TOOLCHAIN_BIN)/$(TOOL_PREFIX)gcc
OBJCOPY := $(TOOLCHAIN_BIN)/$(TOOL_PREFIX)objcopy
OBJDUMP := $(TOOLCHAIN_BIN)/$(TOOL_PREFIX)objdump
SIZE    := $(TOOLCHAIN_BIN)/$(TOOL_PREFIX)size

ZX_SDCC ?= sdcc
ZX_SDAZ80 ?= sdasz80
ZX_OBJCOPY ?= objcopy

TARGET := RISKYSpeccy
BUILD_DIR ?= build

FW_DIR := firmware
LINKER_SCRIPT := $(FW_DIR)/Ld/Link.ld

ZX_DIR := $(FW_DIR)/zx_src
ZX_BUILD_DIR ?= $(BUILD_DIR)/zx_src
ZX_TARGET := zxprog
ZX_ROM_SIZE := 16384
ZX_OUT_C := $(FW_DIR)/User/zx_image.c
ZX_CRT0_SRC := $(ZX_DIR)/crt0.s
ZX_PROG_SRC := $(ZX_DIR)/zxprog.c
ZX_HDR := $(ZX_DIR)/zx.h

ZX_CRT0_REL := $(ZX_BUILD_DIR)/crt0.rel
ZX_IHX := $(ZX_BUILD_DIR)/$(ZX_TARGET).ihx
ZX_BIN := $(ZX_BUILD_DIR)/$(ZX_TARGET).bin

ZX_SDCFLAGS := -mz80 --no-std-crt0 --code-loc 0x0069 --data-loc 0x3F00 \
	--no-xinit-opt

C_SRCS := \
	$(FW_DIR)/Core/core_riscv.c \
	$(FW_DIR)/Debug/debug.c \
	$(FW_DIR)/Peripheral/src/ch32v30x_dac.c \
	$(FW_DIR)/Peripheral/src/ch32v30x_dbgmcu.c \
	$(FW_DIR)/Peripheral/src/ch32v30x_dma.c \
	$(FW_DIR)/Peripheral/src/ch32v30x_exti.c \
	$(FW_DIR)/Peripheral/src/ch32v30x_flash.c \
	$(FW_DIR)/Peripheral/src/ch32v30x_gpio.c \
	$(FW_DIR)/Peripheral/src/ch32v30x_iwdg.c \
	$(FW_DIR)/Peripheral/src/ch32v30x_misc.c \
	$(FW_DIR)/Peripheral/src/ch32v30x_pwr.c \
	$(FW_DIR)/Peripheral/src/ch32v30x_rcc.c \
	$(FW_DIR)/Peripheral/src/ch32v30x_rng.c \
	$(FW_DIR)/Peripheral/src/ch32v30x_sdio.c \
	$(FW_DIR)/Peripheral/src/ch32v30x_tim.c \
	$(FW_DIR)/Peripheral/src/ch32v30x_usart.c \
	$(FW_DIR)/Peripheral/src/ch32v30x_wwdg.c \
	$(FW_DIR)/User/ch32v30x_it.c \
	$(FW_DIR)/User/gpio.c \
	$(FW_DIR)/User/main.c \
	$(FW_DIR)/User/reset_button.c \
	$(FW_DIR)/User/system_ch32v30x.c \
	$(FW_DIR)/User/tape_player.c \
	$(FW_DIR)/User/z80_loader.c \
	$(FW_DIR)/User/zx_bus.c \
	$(FW_DIR)/User/zx_image.c \
	$(FW_DIR)/User/zx_terminal.c \
	$(FW_DIR)/User/FATFS/diskio.c \
	$(FW_DIR)/User/FATFS/ff.c \
	$(FW_DIR)/User/FATFS/ffsystem.c \
	$(FW_DIR)/User/FATFS/ffunicode.c \
	$(FW_DIR)/User/FATFS/usb_disk.c \
	$(FW_DIR)/User/USB_Host/ch32v30x_usbfs_host.c

ASM_SRCS := $(FW_DIR)/Startup/startup_ch32v30x_D8.S

CFLAGS := -march=rv32imacxw -mabi=ilp32 -msmall-data-limit=8 -msave-restore \
	-fmax-errors=20 -Ofast -fmessage-length=0 -fsigned-char \
	-ffunction-sections -fdata-sections -fno-common -Wunused -Wuninitialized \
	-std=gnu99

CPPFLAGS := \
	-I$(FW_DIR)/Debug \
	-I$(FW_DIR)/Core \
	-I$(FW_DIR)/User \
	-I$(FW_DIR)/Peripheral/inc \
	-I$(FW_DIR)/User/USB_Host \
	-I$(FW_DIR)/User/FATFS

ASFLAGS := -x assembler-with-cpp -march=rv32imacxw -mabi=ilp32 -msmall-data-limit=8 \
	-msave-restore -fmax-errors=20 -Ofast -fmessage-length=0 -fsigned-char \
	-ffunction-sections -fdata-sections -fno-common -Wunused -Wuninitialized \
	-I$(FW_DIR)/Startup -I$(FW_DIR)/User

LDFLAGS := -T$(LINKER_SCRIPT) -nostartfiles -Xlinker --gc-sections \
	-Wl,-Map,$(BUILD_DIR)/$(TARGET).map --specs=nano.specs --specs=nosys.specs

OBJS := $(patsubst $(FW_DIR)/%, $(BUILD_DIR)/%, $(C_SRCS:.c=.o)) \
	$(patsubst $(FW_DIR)/%, $(BUILD_DIR)/%, $(ASM_SRCS:.S=.o))

DEPS := $(OBJS:.o=.d)

ELF := $(BUILD_DIR)/$(TARGET).elf
BIN := $(BUILD_DIR)/$(TARGET).bin
HEX := $(BUILD_DIR)/$(TARGET).hex
LST := $(BUILD_DIR)/$(TARGET).lst
SIZ := $(BUILD_DIR)/$(TARGET).siz

.PHONY: all clean rebuild check-toolchain check-zx-tools zx-src flash erase

all: check-toolchain check-zx-tools zx-src $(ELF) $(BIN) $(HEX) $(LST) $(SIZ)

$(ELF): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o "$@" $(OBJS)

$(ELF): zx-src

$(BIN): $(ELF)
	$(OBJCOPY) -O binary "$<" "$@"

$(HEX): $(ELF)
	$(OBJCOPY) -O ihex "$<" "$@"

$(LST): $(ELF)
	$(OBJDUMP) --all-headers --demangle --disassemble -M xw "$<" > "$@"

$(SIZ): $(ELF)
	$(SIZE) --format=berkeley "$<" > "$@"

zx-src: $(ZX_OUT_C)

$(ZX_OUT_C): $(ZX_BIN)
	@{ \
		echo '/* AUTO-GENERATED -- do not edit by hand.'; \
		echo ' * Rebuilt by: make'; \
		echo ' */'; \
		echo '#include "zx_image.h"'; \
		echo ''; \
		echo 'const uint8_t g_zx_image[16384] = {'; \
		od -An -tx1 -v -N$(ZX_ROM_SIZE) "$<" | \
		awk -v rom=$(ZX_ROM_SIZE) '\
		BEGIN { n=0 } \
		function line_start(off){ printf "    /* 0x%04X */ ", off } \
		{ \
		  for (i = 1; i <= NF && n < rom; ++i) { \
		    if ((n % 16) == 0) line_start(n); \
		    printf "0x%s", toupper($$i); \
		    if ((n % 16) == 15) { printf ",\n"; } else { printf ", "; } \
		    ++n; \
		  } \
		} \
		END { \
		  while (n < rom) { \
		    if ((n % 16) == 0) line_start(n); \
		    printf "0x00"; \
		    if ((n % 16) == 15) { printf ",\n"; } else { printf ", "; } \
		    ++n; \
		  } \
		  print "};"; \
		  print ""; \
		  printf "const uint32_t g_zx_image_size = %du;\n", rom; \
		}'; \
	} > "$@"

$(ZX_BIN): $(ZX_IHX)
	$(ZX_OBJCOPY) -I ihex -O binary --gap-fill=0xFF --pad-to=$(ZX_ROM_SIZE) "$<" "$@"

$(ZX_IHX): $(ZX_CRT0_REL) $(ZX_PROG_SRC) $(ZX_HDR)
	@mkdir -p $(dir $@)
	$(ZX_SDCC) $(ZX_SDCFLAGS) "$(ZX_CRT0_REL)" "$(ZX_PROG_SRC)" -o "$@"

$(ZX_CRT0_REL): $(ZX_CRT0_SRC)
	@mkdir -p $(dir $@)
	$(ZX_SDAZ80) -l -o "$@" "$<"

$(BUILD_DIR)/%.o: $(FW_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Wa,-adhlns="$@.lst" -v \
		-MMD -MP -MF"$(@:.o=.d)" -MT"$@" -c -o "$@" "$<"

$(BUILD_DIR)/%.o: $(FW_DIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -v -MMD -MP -MF"$(@:.o=.d)" -MT"$@" -c -o "$@" "$<"

clean:
	rm -rf $(BUILD_DIR)

rebuild: clean all

check-toolchain:
	@test -x "$(CC)" || \
		( echo "Error: toolchain not found at $(TOOLCHAIN_BIN)"; \
		  echo "Set TOOLCHAIN_DIR, for example:"; \
		  echo "  make TOOLCHAIN_DIR=/home/makaron/RISC-V_Embedded_GCC12"; \
		  exit 1 )

check-zx-tools:
	@command -v "$(ZX_SDCC)" >/dev/null 2>&1 || \
		( echo "Error: ZX tool not found: $(ZX_SDCC)"; exit 1 )
	@command -v "$(ZX_SDAZ80)" >/dev/null 2>&1 || \
		( echo "Error: ZX tool not found: $(ZX_SDAZ80)"; exit 1 )
	@command -v "$(ZX_OBJCOPY)" >/dev/null 2>&1 || \
		( echo "Error: ZX tool not found: $(ZX_OBJCOPY)"; exit 1 )
	@command -v od >/dev/null 2>&1 || \
		( echo "Error: required tool not found: od"; exit 1 )
	@command -v awk >/dev/null 2>&1 || \
		( echo "Error: required tool not found: awk"; exit 1 )

-include $(DEPS)

# Ensure generated ZX ROM image sources are refreshed before any CH32 object build.
$(OBJS): | zx-src

# Flash the firmware binary to the CH32 via WCH-LinkE and MRS OpenOCD.
# Requires sudo for USB access.  Build first if the binary is missing.
flash: $(BIN)
	@if [ ! -f "$(OPENOCD)" ]; then \
		echo "Error: OpenOCD not found at $(OPENOCD)"; \
		echo "Set MRS_TOOLCHAIN_ROOT, for example:"; \
		echo "  make flash MRS_TOOLCHAIN_ROOT=/path/to/MRS_Toolchain_Linux_x64_V1.91"; \
		exit 1; \
	fi
	@echo "Flashing $(BIN) ..."
	sudo "$(OPENOCD)" \
		-f "$(OPENOCD_CFG)" \
		-c "program $(shell pwd)/$(BIN) verify reset exit 0x00000000"

# Erase the entire CH32 flash chip.
erase:
	@if [ ! -f "$(OPENOCD)" ]; then \
		echo "Error: OpenOCD not found at $(OPENOCD)"; \
		echo "Set MRS_TOOLCHAIN_ROOT, for example:"; \
		echo "  make erase MRS_TOOLCHAIN_ROOT=/path/to/MRS_Toolchain_Linux_x64_V1.91"; \
		exit 1; \
	fi
	@echo "Erasing flash ..."
	sudo "$(OPENOCD)" \
		-f "$(OPENOCD_CFG)" \
		-c "init; halt; flash erase_sector wch_riscv 0 last; exit"
