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

# minichlink (ch32v003fun) - WCH-LinkE programmer.
# On CH32V30x the program flash is mapped at 0x08000000 (alias of 0x00000000
# for code fetches); minichlink expects addresses in that window.
MINICHLINK       ?= minichlink
MC_FLASH_BASE    := 0x08000000
MC_APP_OFFSET     = $(shell printf '0x%08x' $$(( $(MC_FLASH_BASE) + $(BOOT_OFFSET_BYTES) )))

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
	$(FW_DIR)/User/if2_cart.c \
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
	flash flash-combined flash-combined-unlock flash-standalone flash-standalone-unlock flash-bootloader flash-app-upd flash-all-versions \
	erase \
	check-minichlink mc-info \
	flash-minichlink flash-combined-minichlink flash-bootloader-minichlink \
	flash-app-upd-minichlink flash-standalone-minichlink \
	erase-minichlink unbrick unprotect-minichlink protect-minichlink \
	halt halt-reboot resume reboot \
	split-info split-set split-set-openocd split-set-minichlink

all: check-toolchain check-zx-tools zx-src app bootloader combine

help:
	@echo "Build targets:"
	@echo "  make all                     - build all variants + combined image (default)"
	@echo "  make build-all-versions      - alias for all"
	@echo "  make bootloader              - bootloader only (build/bootloader/Bootloader.bin)"
	@echo "  make app                     - both app variants (bootloader-linked + standalone)"
	@echo "  make app-bootloader          - app linked at 0x00004000 -> build/RISKYZXS.UPD"
	@echo "  make build-bootloader-version  - alias for app-bootloader"
	@echo "  make app-standalone          - app linked at 0x00000000 -> build/RISKYSpeccy.bin"
	@echo "  make build-normal-version    - alias for app-standalone"
	@echo "  make combine                 - merged bootloader+app -> build/RISKYZXS_COMBINED.bin"
	@echo "  make zx-src                  - regenerate ZX ROM source (firmware/User/zx_image.c)"
	@echo "Flash targets:"
	@echo "  make flash                   - flash combined image (alias for flash-combined)"
	@echo "  make flash-all-versions      - flash combined image (alias)"
	@echo "  make flash-combined          - flash combined image at 0x00000000"
	@echo "  make flash-combined-unlock   - unprotect (clear RDP) + flash combined image"
	@echo "  make flash-bootloader        - flash bootloader only at 0x00000000"
	@echo "  make flash-app-upd           - flash app update image at 0x00004000"
	@echo "  make flash-standalone        - flash standalone app at 0x00000000"
	@echo "  make flash-standalone-unlock - unprotect (clear RDP) + flash standalone app"
	@echo "  make erase                   - erase entire on-chip flash via OpenOCD"
	@echo "Flash targets (minichlink / WCH-LinkE):"
	@echo "  make flash-minichlink          - flash combined image (alias for flash-combined-minichlink)"
	@echo "  make flash-combined-minichlink - flash combined image at 0x08000000"
	@echo "  make flash-bootloader-minichlink - flash bootloader only at 0x08000000"
	@echo "  make flash-app-upd-minichlink  - flash app update image at 0x08004000"
	@echo "  make flash-standalone-minichlink - flash standalone app at 0x08000000"
	@echo "  make erase-minichlink          - erase chip (minichlink -E)"
	@echo "  make unbrick                   - power-cycle erase to recover a locked chip (minichlink -u)"
	@echo "  make unprotect-minichlink      - disable read protection (minichlink -p)"
	@echo "  make protect-minichlink        - enable read protection (minichlink -P)"
	@echo "  make mc-info                   - show chip info / option bytes (minichlink -i)"
	@echo "Debug control (minichlink):"
	@echo "  make halt                      - halt CPU without reset (minichlink -A)"
	@echo "  make halt-reboot               - reset + halt at entry point (minichlink -a)"
	@echo "  make resume                    - resume execution from halt (minichlink -e)"
	@echo "  make reboot                    - reboot out of halt (minichlink -b)"
	@echo "RAM / FLASH split (SRAM_CODE_MODE) configuration:"
	@echo "  This project uses mode 00: 192K FLASH + 128K RAM."
	@echo "  You must set the correct split on the chip BEFORE flashing firmware."
	@echo "  make split-info                  - read current option bytes (minichlink -i)"
	@echo "  make split-set MODE=N            - set SRAM_CODE_MODE and reboot."
	@echo "                                       MODE=0 -> 192K FLASH + 128K RAM (default)"
	@echo "                                       MODE=1 -> 224K FLASH +  96K RAM"
	@echo "                                       MODE=2 -> 256K FLASH +  64K RAM"
	@echo "                                       MODE=3 -> 288K FLASH +  32K RAM"
	@echo "                                     Uses minichlink by default. Pass TOOL=openocd"
	@echo "                                     to use the OpenOCD path instead."
	@echo "  make split-set-openocd MODE=N    - set split via OpenOCD"
	@echo "  make split-set-minichlink MODE=N - set split via minichlink"
	@echo "Maintenance:"
	@echo "  make clean                   - remove the build/ directory"
	@echo "  make rebuild                 - clean + all"
	@echo "  make check-toolchain         - verify riscv-wch-elf-gcc is available"
	@echo "  make check-zx-tools          - verify sdcc/sdasz80/objcopy/od/awk are available"
	@echo "  make check-minichlink        - verify minichlink is available"
	@echo "Info:"
	@echo "  make help                    - show this message"
	@echo "  make versions                - build artifacts and print their paths"
	@echo "Overridable variables:"
	@echo "  TOOLCHAIN_DIR=<path>         - RISC-V GCC root (default: $(TOOLCHAIN_DIR))"
	@echo "  MRS_TOOLCHAIN_ROOT=<path>    - MounRiver OpenOCD root (default: $(MRS_TOOLCHAIN_ROOT))"
	@echo "  MINICHLINK=<path>            - minichlink binary (default: $(MINICHLINK))"
	@echo "  BUILD_DIR=<path>             - output directory (default: $(BUILD_DIR))"
	@echo "  ZX_SDCC / ZX_SDAZ80 / ZX_OBJCOPY - ZX cross-tools (defaults: sdcc/sdasz80/objcopy)"

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
		-c "program $(shell pwd)/$(COMBINED_BIN) verify reset exit 0x00000000" || \
		( echo "Hint: if OpenOCD reports read-protect enabled, run: make flash-combined-unlock"; exit 1 )

# Unprotect (RDP clear), then flash combined image.
# NOTE: This operation may erase flash before programming.
flash-combined-unlock: $(COMBINED_BIN)
	@if [ ! -f "$(OPENOCD)" ]; then \
		echo "Error: OpenOCD not found at $(OPENOCD)"; \
		echo "Set MRS_TOOLCHAIN_ROOT, for example:"; \
		echo "  make flash-combined-unlock MRS_TOOLCHAIN_ROOT=/path/to/MRS_Toolchain_Linux_x64_V1.91"; \
		exit 1; \
	fi
	@echo "Unprotecting + flashing $(COMBINED_BIN) at 0x00000000 ..."
	sudo "$(OPENOCD)" \
		-f "$(OPENOCD_CFG)" \
		-c "init; halt; flash write_image erase unlock $(shell pwd)/$(COMBINED_BIN) 0x00000000 bin; verify_image $(shell pwd)/$(COMBINED_BIN) 0x00000000 bin; reset run; shutdown"

# Flash standalone app image at 0x00000000 (no bootloader layout).
flash-standalone: $(APP_STANDALONE_BIN)
	@if [ ! -f "$(OPENOCD)" ]; then \
		echo "Error: OpenOCD not found at $(OPENOCD)"; \
		exit 1; \
	fi
	@echo "Flashing $(APP_STANDALONE_BIN) at 0x00000000 ..."
	sudo "$(OPENOCD)" \
		-f "$(OPENOCD_CFG)" \
		-c "program $(shell pwd)/$(APP_STANDALONE_BIN) verify reset exit 0x00000000" || \
		( echo "Hint: if OpenOCD reports read-protect enabled, run: make flash-standalone-unlock"; exit 1 )

# Unprotect (RDP clear), then flash standalone app image.
# NOTE: This operation may erase flash before programming.
flash-standalone-unlock: $(APP_STANDALONE_BIN)
	@if [ ! -f "$(OPENOCD)" ]; then \
		echo "Error: OpenOCD not found at $(OPENOCD)"; \
		echo "Set MRS_TOOLCHAIN_ROOT, for example:"; \
		echo "  make flash-standalone-unlock MRS_TOOLCHAIN_ROOT=/path/to/MRS_Toolchain_Linux_x64_V1.91"; \
		exit 1; \
	fi
	@echo "Unprotecting + flashing $(APP_STANDALONE_BIN) at 0x00000000 ..."
	sudo "$(OPENOCD)" \
		-f "$(OPENOCD_CFG)" \
		-c "init; halt; flash write_image erase unlock $(shell pwd)/$(APP_STANDALONE_BIN) 0x00000000 bin; verify_image $(shell pwd)/$(APP_STANDALONE_BIN) 0x00000000 bin; reset run; shutdown"

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

# ----------------------------------------------------------------------------
# minichlink (ch32v003fun) targets — alternative programmer path via WCH-LinkE.
#
# Override the binary location with: make MINICHLINK=/path/to/minichlink ...
# All program/erase/unbrick targets require the WCH-LinkE to be plugged in and
# the target board powered (or powered by the LinkE via the -3 / -5 args).
# ----------------------------------------------------------------------------

check-minichlink:
	@command -v "$(MINICHLINK)" >/dev/null 2>&1 || \
		( echo "Error: minichlink not found ($(MINICHLINK))"; \
		  echo "Install from https://github.com/cnlohr/ch32v003fun or set MINICHLINK=/path/to/minichlink"; \
		  exit 1 )

# Show chip info / read option bytes.
mc-info: check-minichlink
	"$(MINICHLINK)" -i

# Flash combined image (bootloader + app) at flash base.
flash-minichlink: flash-combined-minichlink

flash-combined-minichlink: check-minichlink $(COMBINED_BIN)
	@echo "Flashing $(COMBINED_BIN) at $(MC_FLASH_BASE) via minichlink ..."
	"$(MINICHLINK)" -w "$(COMBINED_BIN)" $(MC_FLASH_BASE) -b

# Flash bootloader only.
flash-bootloader-minichlink: check-minichlink $(BL_BIN)
	@echo "Flashing $(BL_BIN) at $(MC_FLASH_BASE) via minichlink ..."
	"$(MINICHLINK)" -w "$(BL_BIN)" $(MC_FLASH_BASE) -b

# Flash app update image at app slot (assumes bootloader already present).
flash-app-upd-minichlink: check-minichlink $(APP_BOOT_BIN)
	@echo "Flashing $(APP_BOOT_BIN) at $(MC_APP_OFFSET) via minichlink ..."
	"$(MINICHLINK)" -w "$(APP_BOOT_BIN)" $(MC_APP_OFFSET) -b

# Flash standalone app at flash base.
flash-standalone-minichlink: check-minichlink $(APP_STANDALONE_BIN)
	@echo "Flashing $(APP_STANDALONE_BIN) at $(MC_FLASH_BASE) via minichlink ..."
	"$(MINICHLINK)" -w "$(APP_STANDALONE_BIN)" $(MC_FLASH_BASE) -b

# Erase entire chip via minichlink (-E).
erase-minichlink: check-minichlink
	@echo "Erasing chip via minichlink ..."
	"$(MINICHLINK)" -E -b

# Disable read protection (RDP). May erase flash as a side effect.
unprotect-minichlink: check-minichlink
	@echo "Disabling read protection via minichlink ..."
	"$(MINICHLINK)" -p -b

# Enable read protection (RDP).
protect-minichlink: check-minichlink
	@echo "Enabling read protection via minichlink ..."
	"$(MINICHLINK)" -P -b

# Unbrick: power-cycle erase that recovers a chip locked or stuck in a bad state.
# Uses minichlink -u (clears all code flash by power-off cycling the LinkE rail).
unbrick: check-minichlink
	@echo "Unbricking chip via minichlink (-u) ..."
	"$(MINICHLINK)" -u

# Halt the CPU without resetting (minichlink -A).
halt: check-minichlink
	@echo "Halting CPU via minichlink (-A) ..."
	"$(MINICHLINK)" -A

# Reset the CPU and halt at the entry point (minichlink -a).
halt-reboot: check-minichlink
	@echo "Resetting + halting CPU via minichlink (-a) ..."
	"$(MINICHLINK)" -a

# Resume execution from halt (minichlink -e).
resume: check-minichlink
	@echo "Resuming CPU via minichlink (-e) ..."
	"$(MINICHLINK)" -e

# Reboot out of halt (minichlink -b).
reboot: check-minichlink
	@echo "Rebooting CPU via minichlink (-b) ..."
	"$(MINICHLINK)" -b

# ----------------------------------------------------------------------------
# RAM / FLASH split configuration.
#
# The CH32V303 SRAM_CODE_MODE field (USER[7:6] in the option byte at
# 0x1FFFF802) selects the FLASH/RAM split.  This project requires MODE=0
# (192K FLASH + 128K RAM) to match the linker scripts in Bootloader/Ld and
# firmware/Ld.  The new value is applied at next reset.
#
# Option byte format: a 16-bit half-word where the low byte is the value and
# the high byte is its bitwise complement.  Other USER bits default to 0x3F
# (bits 0..5 = 1, bits 6..7 = SRAM_CODE_MODE).
#
#   MODE=0 -> byte 0x3F, half-word 0xC03F  (192K FLASH + 128K RAM) <-- required
#   MODE=1 -> byte 0x7F, half-word 0x807F  (224K FLASH +  96K RAM)
#   MODE=2 -> byte 0xBF, half-word 0x40BF  (256K FLASH +  64K RAM)
#   MODE=3 -> byte 0xFF, half-word 0x00FF  (288K FLASH +  32K RAM)
# ----------------------------------------------------------------------------

# Default to the project's required mode.
SPLIT_MODE ?= 0

# Validate the mode argument and compute the FLASH/SRAM kB pair for minichlink's
# `-S FLASH_kbytes SRAM_kbytes` option.  These are the standard CH32V30x
# SRAM_CODE_MODE options exposed via the USER option byte at 0x1FFFF802.
ifeq ($(SPLIT_MODE),0)
SPLIT_OB_WORD := 0xC03F
SPLIT_FLASH  := 192
SPLIT_SRAM   := 128
SPLIT_DESC   := "192K FLASH + 128K RAM"
else ifeq ($(SPLIT_MODE),1)
SPLIT_OB_WORD := 0x807F
SPLIT_FLASH  := 224
SPLIT_SRAM   := 96
SPLIT_DESC   := "224K FLASH + 96K RAM"
else ifeq ($(SPLIT_MODE),2)
SPLIT_OB_WORD := 0x40BF
SPLIT_FLASH  := 256
SPLIT_SRAM   := 64
SPLIT_DESC   := "256K FLASH + 64K RAM"
else ifeq ($(SPLIT_MODE),3)
SPLIT_OB_WORD := 0x00FF
SPLIT_FLASH  := 288
SPLIT_SRAM   := 32
SPLIT_DESC   := "288K FLASH + 32K RAM"
else
$(error Invalid SPLIT_MODE='$(SPLIT_MODE)'; must be 0, 1, 2, or 3)
endif

# Read current option bytes via minichlink.
split-info: check-minichlink
	@echo "Reading chip info / option bytes via minichlink (-i) ..."
	"$(MINICHLINK)" -i

# Common warning emitted by both programmer paths before changing option bytes.
define SPLIT_WARN
	@echo ""; \
	echo "*** WARNING: changing SRAM_CODE_MODE ***"; \
	echo "Target split: $(SPLIT_DESC) (MODE=$(SPLIT_MODE))"; \
	echo "Option byte at 0x1FFFF802 will be programmed with $(SPLIT_OB_WORD)."; \
	echo "A system reset is required for the new split to take effect."; \
	echo ""
endef

# Set SRAM_CODE_MODE via OpenOCD.
split-set-openocd:
	@if [ ! -f "$(OPENOCD)" ]; then \
		echo "Error: OpenOCD not found at $(OPENOCD)"; \
		echo "Set MRS_TOOLCHAIN_ROOT, for example:"; \
		echo "  make split-set-openocd MRS_TOOLCHAIN_ROOT=/path/to/MRS_Toolchain_Linux_x64_V1.91"; \
		exit 1; \
	fi
	$(SPLIT_WARN)
	sudo "$(OPENOCD)" \
		-f "$(OPENOCD_CFG)" \
		-c "init; halt; flash write_word 0x1FFFF802 $(SPLIT_OB_WORD); reset; exit"
	@echo "Split updated. Power-cycle the board before flashing firmware."

# Set SRAM_CODE_MODE via minichlink.  minichlink exposes a dedicated `-S
# FLASH_kbytes SRAM_kbytes` option that programs the USER option byte and
# reboots the chip in one step.
split-set-minichlink: check-minichlink
	$(SPLIT_WARN)
	@echo "Programming SRAM_CODE_MODE via minichlink -S $(SPLIT_FLASH) $(SPLIT_SRAM) ..."
	"$(MINICHLINK)" -S $(SPLIT_FLASH) $(SPLIT_SRAM) -b
	@echo "Split updated. Power-cycle the board before flashing firmware."

# Convenience dispatcher: pick the programmer from TOOL=minichlink|openocd.
SPLIT_TOOL ?= minichlink
split-set:
	@if [ "$(SPLIT_TOOL)" = "openocd" ]; then \
		$(MAKE) split-set-openocd SPLIT_MODE=$(SPLIT_MODE); \
	elif [ "$(SPLIT_TOOL)" = "minichlink" ]; then \
		$(MAKE) split-set-minichlink SPLIT_MODE=$(SPLIT_MODE); \
	else \
		echo "Error: SPLIT_TOOL must be 'minichlink' or 'openocd' (got '$(SPLIT_TOOL)')"; \
		exit 1; \
	fi
