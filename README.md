# RISKY Speccy

A CH32V307 RISC-V cartridge that plugs into the ZX Spectrum edge connector
and provides:

- **Snapshot loader** — load `.z80` v1/v2/v3 files from USB storage and
  launch them with full register restore.
- **Tape player** — play `.tap` / `.tzx` files by injecting the EAR signal
  directly onto the data bus, compatible with the Spectrum's built-in tape
  loader.
- **On-screen UI** — full-screen file browser and VT100-compatible terminal
  rendered by the CH32 onto the ZX screen via the NMI mailbox, without any
  Z80 cooperation.

See [docs/](docs/) for architecture and subsystem documentation.

---

## Repository layout

```
board/          KiCad schematic and PCB design
docs/           Architecture and subsystem documentation
firmware/       CH32 firmware source
  Core/         WCH RISC-V core support files
  Debug/        UART debug driver
  Ld/           Linker script
  Peripheral/   CH32V30x peripheral drivers
  Startup/      Startup assembly
  User/         Application code (main subsystems)
  zx_src/       Z80-side cart ROM firmware (SDCC)
Makefile        Root build file (builds everything)
```

---

## Building

### Prerequisites

**CH32 RISC-V toolchain** (WCH GCC 12):

Download from [MounRiver Studio](http://www.mounriver.com/) or extract the
standalone toolchain archive.  The default path expected by the Makefile is:

```
~/RISC-V_Embedded_GCC12
```

**Z80 toolchain** (for the `zx_src` sub-firmware):

```
sudo apt install sdcc
```

The `sdasz80` assembler is included with SDCC.  GNU `objcopy`, `od`, and
`awk` must also be available (standard on most Linux systems).

### Build

```sh
make
```

Override the toolchain path if needed:

```sh
make TOOLCHAIN_DIR=/path/to/RISC-V_Embedded_GCC12
```

Useful targets:

```sh
make help                # full list of targets
make build-all-versions  # bootloader + both app variants + combined image
make versions            # show all output artifact paths
make clean
```

Build outputs land in `build/`.

### Continuous integration (GitHub Actions)

A workflow at [`.github/workflows/build.yml`](.github/workflows/build.yml)
builds the firmware on every push, pull request, tag, and manual dispatch.
It:

- installs the WCH RISC-V toolchain (V2.10) and SDCC from `apt`
- runs `make` at the repo root
- verifies that all expected artifacts (`Bootloader.bin`, `RISKYSpeccy.bin`,
  `RISKYZXS.UPD`, `RISKYZXS_COMBINED.bin`) are produced
- checks that the bootloader and app-update binaries fit their respective
  flash regions
- uploads the binaries as a workflow artifact
  (`RISKYZXSpectrum-firmware`, retained for 90 days) and the listings/maps
  as a separate logs artifact
- on tag pushes (`v*`) and on `release` events, attaches the user-facing
  binaries (`*.bin` / `*.UPD` at the top of `build/`) to a GitHub Release

The default `latest` release is marked as prerelease and is updated on
every non-PR run; tagged builds (`v1.2.3`, etc.) become stable releases.
No secrets are required — the workflow uses the default `GITHUB_TOKEN`
to create/update releases.

### Flashing options

| Mode | Command | Output | When to use |
|------|---------|--------|-------------|
| **Combined** (default) | `make flash` | `build/RISKYZXS_COMBINED.bin` | Fresh cartridge setup or bootloader + app in one shot |
| **Standalone (no BL)** | `make flash-standalone` | `build/RISKYSpeccy.bin` | Development or single-firmware deployment |
| **Bootloader only** | `make flash-bootloader` | `build/bootloader/Bootloader.bin` | Bootloader maintenance / recovery |
| **App via USB** | _(copy to USB drive)_ | `build/RISKYZXS.UPD` | After bootloader is installed |

The Linux `make flash*` targets drive a **WCH-LinkE** via OpenOCD.

#### Programming on Windows with WCH tools

1. Install [WCHISPTool](https://www.wch.cn/downloads/WCHISPTool_Setup_exe.html)
   and the **WCH-LinkE** USB driver (WCH provides a signed INF/VCP driver
   bundle — let the installer do it, or grab the driver from the
   MounRiver Studio distribution).
2. Connect the **WCH-LinkE** to the cartridge's SWD header (SWDIO / SWCLK /
   GND).  Power the Spectrum so the CH32V307 is alive on the edge connector.
3. Launch **WCHISPTool** and pick the chip family **CH32V30x** (the
   CH32V307 lives in this group).
4. In the device list, select the WCH-LinkE you just plugged in.  The tool
   should show the chip's `Device ID` once it talks to the target.
5. Click **...** next to the file box and point it at the build output:
   - `build/RISKYZXS_COMBINED.bin` — bootloader + application in one shot
     (use the **.bin** image, not `.elf`/`.hex`)
   - `build/RISKYSpeccy.bin` — application only (no bootloader)
   - `build/bootloader/Bootloader.bin` — bootloader only
6. Set the start address to **`0x08000000`** for any of the above.
7. Enable **"Erase before program"** and **"Verify after program"**, then
   click **Download** (or press F5).  Progress shows in the status bar;
   "Success" means programming finished and verified.

#### Updating the application via USB (no programmer)

For the **app-via-USB** path, the bootloader must already be installed on
the cartridge (use the WCHISPTool flow above once with the combined or
bootloader image to put it in place).

1. Copy `build/RISKYZXS.UPD` to the **root** of a **FAT12/FAT16/FAT32**
   formatted USB drive.
2. Insert the USB drive into the cartridge's USB port.
3. To **force IAP mode** (e.g. if the application is wedged and you want
   to reflash), fit a jumper across the **SWDIO / SWCLK** header pins
   before powering on.
4. Power on (or reset) the cartridge. The Ready LED should flash 3 times.
   The bootloader detects the drive and programs the application. If
   programming was successful, the Ready LED will flash slowly 10 times.
   If unsuccessful, it will start rapidly flashing.


See [docs/bootloader.md](docs/bootloader.md) for the full update flow,
state-machine details, and recovery options.

---

## CH32V303 RAM / FLASH split

The CH32V303 lets you trade FLASH for RAM (or vice versa) via the
**SRAM_CODE_MODE** field — bits `[7:6]` of the USER option byte at
`0x1FFFF802`.  The four available splits are:

| SRAM_CODE_MODE | FLASH  | RAM    |
|----------------|--------|--------|
| `00`           | 192 KB | 128 KB |
| `01`           | 224 KB |  96 KB |
| `10`           | 256 KB |  64 KB |
| `11`           | 288 KB |  32 KB |

This project uses **`00`** (192 KB FLASH + 128 KB RAM).  **You must set
this split before flashing the firmware**, otherwise RAM and FLASH regions
will not match the linker script and the firmware will not run.

### Setting the split with WCHISPTool (recommended)

This is the easiest method and requires only the WCH-LinkE programmer.

1. Open **WCHISPTool**, select chip family **CH32V30x**, and connect to the
   target (same as for flashing firmware — see [Flashing options](#flashing-options)).
2. Switch to the **Config** (or **Option Byte**) tab.
3. Locate the **SRAM_CODE_MODE** field (labelled `USER[7:6]` in some
   versions).
4. Select **`00`** from the drop-down to configure 192 KB FLASH + 128 KB RAM.
5. Click **Program** (or **Download**) to write the option byte.
6. Power-cycle or reset the board.  The new split is now active and you can
   proceed to flash the firmware.

### Setting the split with the Makefile (Linux + WCH-LinkE)

The repository provides Makefile targets that set the split and reboot the
chip in one step.  This is the quickest path on Linux.

| Command | Description |
|---------|-------------|
| `make split-info` | Read current option bytes (uses `minichlink -i`) |
| `make split-set` | Set the split to `MODE=0` (192K FLASH + 128K RAM) via minichlink |
| `make split-set MODE=0 SPLIT_TOOL=openocd` | Same, but uses OpenOCD |
| `make split-set-minichlink MODE=N` | Set split via minichlink, `N` = 0..3 |
| `make split-set-openocd MODE=N` | Set split via OpenOCD, `N` = 0..3 |

`MODE` values: `0` = 192K FLASH + 128K RAM, `1` = 224K + 96K, `2` = 256K + 64K,
`3` = 288K + 32K.  **This project requires `MODE=0`.**

```sh
# Default (recommended): mode 0 via minichlink
make split-set

# Mode 0 via OpenOCD
make split-set SPLIT_TOOL=openocd

# Power-cycle the board, then flash firmware
make flash
```

> After writing, power-cycle the board before flashing firmware.

### Setting the split with OpenOCD / WCH-LinkE on Linux (manual)

```sh
openocd -f interface/wch-riscv.cfg \
        -c "init; halt; \
            flash write_word 0x1FFFF802 0xC03F; \
            reset; exit"
```

The half-word `0xC03F` encodes USER byte `0x3F` (`SRAM_CODE_MODE = 00`,
all other bits at default) in the low byte and its complement `0xC0` in
the high byte, as required by the option byte format.

> After writing, power-cycle the board before flashing firmware.

