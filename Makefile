# Unified build for:
# - Bootloader (linked at 0x00000000, 16KB region)
# - Application for bootloader update flow (linked at 0x00004000) -> RISKYZXS.UPD
# - Standalone application (linked at 0x00000000)
# - Combined flash image: bootloader + app-update image

TOOLCHAIN_DIR ?= /home/makaron/RISC-V_Embedded_GCC12
TOOLCHAIN_BIN := $(TOOLCHAIN_DIR)/bin
TOOL_PREFIX := riscv-wch-elf-

# MounRiver Studio OpenOCD root.
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

BUILD_DIR ?= build

# Paths
FW_DIR := firmware
BL_DIR := Bootloader

# Targets and linker scripts
APP_TARGET := RISKYSpeccy
APP_UPD_NAME := RISKYZXS.UPD
COMBINED_NAME := RISKYZXS_COMBINED.bin
APP_LINKER_BOOT := $(FW_DIR)/Ld/Link.ld
APP_LINKER_STANDALONE := $(FW_DIR)/Ld/Link_standalone.ld
BL_LINKER := $(BL_DIR)/Ld/Link.ld

# ZX ROM build inputs
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

# Firmware sources
APP_C_SRCS := \
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

APP_ASM_SRCS := $(FW_DIR)/Startup/startup_ch32v30x_D8.S

# Bootloader sources
BL_C_SRCS := \
	$(wildcard $(BL_DIR)/Core/*.c) \
	$(wildcard $(BL_DIR)/Debug/*.c) \
	$(wildcard $(BL_DIR)/Peripheral/src/*.c) \
	$(wildcard $(BL_DIR)/User/*.c) \
	$(wildcard $(BL_DIR)/User/Host_IAP/*.c) \
	$(wildcard $(BL_DIR)/User/Pff/*.c) \
	$(wildcard $(BL_DIR)/User/USB_Host/*.c)

BL_ASM_SRCS := $(BL_DIR)/Startup/startup_ch32v30x_D8.S

# Flags
COMMON_ARCH_FLAGS := -march=rv32imacxw -mabi=ilp32 -msmall-data-limit=8 -msave-restore
COMMON_WARN_FLAGS := -fmax-errors=20 -fmessage-length=0 -fsigned-char -ffunction-sections \
	-fdata-sections -fno-common -Wunused -Wuninitialized

APP_CFLAGS := $(COMMON_ARCH_FLAGS) $(COMMON_WARN_FLAGS) -Ofast -std=gnu99
APP_CPPFLAGS := \
	-I$(FW_DIR)/Debug \
	-I$(FW_DIR)/Core \
	-I$(FW_DIR)/User \
	-I$(FW_DIR)/Peripheral/inc \
	-I$(FW_DIR)/User/USB_Host \
	-I$(FW_DIR)/User/FATFS
APP_ASFLAGS := -x assembler-with-cpp $(COMMON_ARCH_FLAGS) $(COMMON_WARN_FLAGS) -Ofast \
	-I$(FW_DIR)/Startup -I$(FW_DIR)/User

BL_CFLAGS := $(COMMON_ARCH_FLAGS) $(COMMON_WARN_FLAGS) -Os -std=gnu99
BL_CPPFLAGS := \
	-I$(BL_DIR)/Debug \
	-I$(BL_DIR)/Core \
	-I$(BL_DIR)/User \
	-I$(BL_DIR)/User/Host_IAP \
	-I$(BL_DIR)/Peripheral/inc \
	-I$(BL_DIR)/User/USB_Host \
	-I$(BL_DIR)/User/Pff \
	-I$(BL_DIR)/Startup
BL_ASFLAGS := -x assembler-with-cpp $(COMMON_ARCH_FLAGS) $(COMMON_WARN_FLAGS) -Os \
	-I$(BL_DIR)/Startup -I$(BL_DIR)/User

APP_LDFLAGS_COMMON := -nostartfiles -Xlinker --gc-sections --specs=nano.specs --specs=nosys.specs
BL_LDFLAGS := -T$(BL_LINKER) -nostartfiles -Xlinker --gc-sections \
	-Wl,-Map,$(BUILD_DIR)/bootloader/Bootloader.map --specs=nano.specs --specs=nosys.specs

# Object/dependency paths
APP_OBJS := $(patsubst $(FW_DIR)/%, $(BUILD_DIR)/app/%, $(APP_C_SRCS:.c=.o)) \
	$(patsubst $(FW_DIR)/%, $(BUILD_DIR)/app/%, $(APP_ASM_SRCS:.S=.o))
APP_DEPS := $(APP_OBJS:.o=.d)

BL_OBJS := $(patsubst $(BL_DIR)/%, $(BUILD_DIR)/bootloader/%, $(BL_C_SRCS:.c=.o)) \
	$(patsubst $(BL_DIR)/%, $(BUILD_DIR)/bootloader/%, $(BL_ASM_SRCS:.S=.o))
BL_DEPS := $(BL_OBJS:.o=.d)

# Firmware outputs
APP_BOOT_ELF := $(BUILD_DIR)/app_bootloader/$(APP_TARGET).elf
APP_BOOT_BIN := $(BUILD_DIR)/$(APP_UPD_NAME)
APP_BOOT_HEX := $(BUILD_DIR)/app_bootloader/$(APP_TARGET).hex
APP_BOOT_LST := $(BUILD_DIR)/app_bootloader/$(APP_TARGET).lst
APP_BOOT_SIZ := $(BUILD_DIR)/app_bootloader/$(APP_TARGET).siz

APP_STANDALONE_ELF := $(BUILD_DIR)/app_standalone/$(APP_TARGET).elf
APP_STANDALONE_BIN := $(BUILD_DIR)/$(APP_TARGET).bin
APP_STANDALONE_HEX := $(BUILD_DIR)/$(APP_TARGET).hex
APP_STANDALONE_LST := $(BUILD_DIR)/$(APP_TARGET).lst
APP_STANDALONE_SIZ := $(BUILD_DIR)/$(APP_TARGET).siz

# Bootloader outputs
BL_ELF := $(BUILD_DIR)/bootloader/Bootloader.elf
BL_BIN := $(BUILD_DIR)/bootloader/Bootloader.bin
BL_HEX := $(BUILD_DIR)/bootloader/Bootloader.hex
BL_LST := $(BUILD_DIR)/bootloader/Bootloader.lst
BL_SIZ := $(BUILD_DIR)/bootloader/Bootloader.siz

# Combined image
COMBINED_BIN := $(BUILD_DIR)/$(COMBINED_NAME)
BOOT_OFFSET_BYTES := 16384

.PHONY: all help versions clean rebuild check-toolchain check-zx-tools zx-src \
	app app-bootloader app-standalone bootloader combine \
	build-all-versions build-bootloader-version build-normal-version \
	flash flash-combined flash-standalone flash-bootloader flash-app-upd flash-all-versions \
	erase

all: check-toolchain check-zx-tools zx-src app bootloader combine

help:
	@echo "Build targets:"
	@echo "  make all                     - build all variants + combined image"
	@echo "  make build-all-versions      - alias for all"
	@echo "  make bootloader              - bootloader only"
	@echo "  make app-bootloader          - app linked for bootloader (RISKYZXS.UPD)"
	@echo "  make app-standalone          - normal app linked at 0x00000000"
	@echo "  make combine                 - build merged bootloader+app image"
	@echo "Flash targets:"
	@echo "  make flash                   - flash combined image"
	@echo "  make flash-bootloader        - flash bootloader only"
	@echo "  make flash-app-upd           - flash app update image at 0x00004000"
	@echo "  make flash-standalone        - flash normal app at 0x00000000"
	@echo "  make flash-all-versions      - flash combined image (alias)"
	@echo "Info:"
	@echo "  make versions                - print artifact paths"

build-all-versions: all

build-bootloader-version: app-bootloader

build-normal-version: app-standalone

versions: app-bootloader app-standalone bootloader combine
	@echo "Built artifacts:"
	@echo "  Bootloader:         $(BL_BIN)"
	@echo "  App update (.UPD):  $(APP_BOOT_BIN)"
	@echo "  Normal app (.bin):  $(APP_STANDALONE_BIN)"
	@echo "  Combined image:     $(COMBINED_BIN)"

app: app-bootloader app-standalone

app-bootloader: $(APP_BOOT_ELF) $(APP_BOOT_BIN) $(APP_BOOT_HEX) $(APP_BOOT_LST) $(APP_BOOT_SIZ)

app-standalone: $(APP_STANDALONE_ELF) $(APP_STANDALONE_BIN) $(APP_STANDALONE_HEX) $(APP_STANDALONE_LST) $(APP_STANDALONE_SIZ)

bootloader: $(BL_ELF) $(BL_BIN) $(BL_HEX) $(BL_LST) $(BL_SIZ)

combine: $(COMBINED_BIN)

# Firmware link variants (same objects, different linker scripts)
$(APP_BOOT_ELF): $(APP_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(APP_CFLAGS) -T$(APP_LINKER_BOOT) $(APP_LDFLAGS_COMMON) \
		-Wl,-Map,$(BUILD_DIR)/app_bootloader/$(APP_TARGET).map \
		-o "$@" $(APP_OBJS)

$(APP_STANDALONE_ELF): $(APP_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(APP_CFLAGS) -T$(APP_LINKER_STANDALONE) $(APP_LDFLAGS_COMMON) \
		-Wl,-Map,$(BUILD_DIR)/app_standalone/$(APP_TARGET).map \
		-o "$@" $(APP_OBJS)

$(APP_BOOT_BIN): $(APP_BOOT_ELF)
	$(OBJCOPY) -O binary "$<" "$@"

$(APP_BOOT_HEX): $(APP_BOOT_ELF)
	$(OBJCOPY) -O ihex "$<" "$@"

$(APP_BOOT_LST): $(APP_BOOT_ELF)
	$(OBJDUMP) --all-headers --demangle --disassemble -M xw "$<" > "$@"

$(APP_BOOT_SIZ): $(APP_BOOT_ELF)
	$(SIZE) --format=berkeley "$<" > "$@"

$(APP_STANDALONE_BIN): $(APP_STANDALONE_ELF)
	$(OBJCOPY) -O binary "$<" "$@"

$(APP_STANDALONE_HEX): $(APP_STANDALONE_ELF)
	$(OBJCOPY) -O ihex "$<" "$@"

$(APP_STANDALONE_LST): $(APP_STANDALONE_ELF)
	$(OBJDUMP) --all-headers --demangle --disassemble -M xw "$<" > "$@"

$(APP_STANDALONE_SIZ): $(APP_STANDALONE_ELF)
	$(SIZE) --format=berkeley "$<" > "$@"

# Bootloader outputs
$(BL_ELF): $(BL_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(BL_CFLAGS) $(BL_LDFLAGS) -o "$@" $(BL_OBJS)

$(BL_BIN): $(BL_ELF)
	$(OBJCOPY) -O binary "$<" "$@"

$(BL_HEX): $(BL_ELF)
	$(OBJCOPY) -O ihex "$<" "$@"

$(BL_LST): $(BL_ELF)
	$(OBJDUMP) --all-headers --demangle --disassemble -M xw "$<" > "$@"

$(BL_SIZ): $(BL_ELF)
	$(SIZE) --format=berkeley "$<" > "$@"

# Combined image: pad to 0x4000 then append/update app image at offset.
$(COMBINED_BIN): $(BL_BIN) $(APP_BOOT_BIN)
	@mkdir -p $(dir $@)
	dd if=/dev/zero of="$@" bs=1 count=$(BOOT_OFFSET_BYTES) status=none
	dd if="$(BL_BIN)" of="$@" conv=notrunc status=none
	dd if="$(APP_BOOT_BIN)" of="$@" bs=1 seek=$(BOOT_OFFSET_BYTES) conv=notrunc status=none
	@echo "Created $@ (bootloader @0x00000000, app @0x00004000)"

# ZX ROM generation
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

# Compile rules
$(BUILD_DIR)/app/%.o: $(FW_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(APP_CFLAGS) $(APP_CPPFLAGS) -Wa,-adhlns="$@.lst" \
		-MMD -MP -MF"$(@:.o=.d)" -MT"$@" -c -o "$@" "$<"

$(BUILD_DIR)/app/%.o: $(FW_DIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(APP_ASFLAGS) -MMD -MP -MF"$(@:.o=.d)" -MT"$@" -c -o "$@" "$<"

$(BUILD_DIR)/bootloader/%.o: $(BL_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BL_CFLAGS) $(BL_CPPFLAGS) -Wa,-adhlns="$@.lst" \
		-MMD -MP -MF"$(@:.o=.d)" -MT"$@" -c -o "$@" "$<"

$(BUILD_DIR)/bootloader/%.o: $(BL_DIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(BL_ASFLAGS) -MMD -MP -MF"$(@:.o=.d)" -MT"$@" -c -o "$@" "$<"

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

-include $(APP_DEPS)
-include $(BL_DEPS)

# Ensure generated ZX ROM source exists before any firmware object build.
$(APP_OBJS): | zx-src

# Flash combined image (default flash target).
flash: flash-combined

flash-all-versions: flash-combined

flash-combined: $(COMBINED_BIN)
	@if [ ! -f "$(OPENOCD)" ]; then \
		echo "Error: OpenOCD not found at $(OPENOCD)"; \
		echo "Set MRS_TOOLCHAIN_ROOT, for example:"; \
		echo "  make flash MRS_TOOLCHAIN_ROOT=/path/to/MRS_Toolchain_Linux_x64_V1.91"; \
		exit 1; \
	fi
	@echo "Flashing $(COMBINED_BIN) at 0x00000000 ..."
	sudo "$(OPENOCD)" \
		-f "$(OPENOCD_CFG)" \
		-c "program $(shell pwd)/$(COMBINED_BIN) verify reset exit 0x00000000"

# Flash standalone app image at 0x00000000 (no bootloader layout).
flash-standalone: $(APP_STANDALONE_BIN)
	@if [ ! -f "$(OPENOCD)" ]; then \
		echo "Error: OpenOCD not found at $(OPENOCD)"; \
		exit 1; \
	fi
	@echo "Flashing $(APP_STANDALONE_BIN) at 0x00000000 ..."
	sudo "$(OPENOCD)" \
		-f "$(OPENOCD_CFG)" \
		-c "program $(shell pwd)/$(APP_STANDALONE_BIN) verify reset exit 0x00000000"

# Flash bootloader only.
flash-bootloader: $(BL_BIN)
	@if [ ! -f "$(OPENOCD)" ]; then \
		echo "Error: OpenOCD not found at $(OPENOCD)"; \
		exit 1; \
	fi
	@echo "Flashing $(BL_BIN) at 0x00000000 ..."
	sudo "$(OPENOCD)" \
		-f "$(OPENOCD_CFG)" \
		-c "program $(shell pwd)/$(BL_BIN) verify reset exit 0x00000000"

# Flash app update image into app slot (for existing bootloader).
flash-app-upd: $(APP_BOOT_BIN)
	@if [ ! -f "$(OPENOCD)" ]; then \
		echo "Error: OpenOCD not found at $(OPENOCD)"; \
		exit 1; \
	fi
	@echo "Flashing $(APP_BOOT_BIN) at 0x00004000 ..."
	sudo "$(OPENOCD)" \
		-f "$(OPENOCD_CFG)" \
		-c "program $(shell pwd)/$(APP_BOOT_BIN) verify reset exit 0x00004000"

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
