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
/home/<user>/RISC-V_Embedded_GCC12
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

The build outputs land in `build/`:

| File | Description |
|------|-------------|
| `build/RISKYSpeccy.elf` | Linked ELF (for debugging / flashing via OpenOCD) |
| `build/RISKYSpeccy.bin` | Raw binary for flashing |
| `build/RISKYSpeccy.hex` | Intel HEX for flashing |

### Clean

```sh
make clean
```

### Flashing

Flash `build/RISKYSpeccy.bin` to the CH32V307 using WCH-LinkE and the
[WCH Flash Tool](https://www.wch.cn/downloads/WCHISPTool_Setup_exe.html)
or OpenOCD with the WCH patch.

---

## Documentation

| Document | Description |
|----------|-------------|
| [docs/bootloader.md](docs/bootloader.md) | Custom IAP bootloader: memory layout, USB firmware update flow, FAT filesystem integration, pre-inserted drive detection, recovery mechanisms |
| [docs/zx_bus.md](docs/zx_bus.md) | Hardware-level CH32–ZX bus interface: GPIO wiring, `RunCartWithRAM` ISR, ROMCS/NMI/reset control, mailbox protocol |
| [docs/z80_launch_mechanism.md](docs/z80_launch_mechanism.md) | Full `.z80` snapshot launch sequence: parsing, PGCMD paging, register restore, ROMCS handover |
| [docs/tape_player.md](docs/tape_player.md) | `.tap`/`.tzx` playback: TZX→TAP conversion, timing tables, two-ISR state machine, EAR injection |
| [docs/zx_terminal.md](docs/zx_terminal.md) | On-screen UI: virtual terminal, file browser, bridge text, rendering pipeline, public API |

---

## Bootloader

### Overview

The RISKY Speccy includes a custom **in-application programming (IAP) bootloader** that:

- **Resides at flash offset 0x00000000** in a reserved 16 KB region
- **Provides USB-based firmware updates** via Petit FatFS and USB MSC (Mass Storage Class)
- **Allows zero-downtime firmware upgrades** by programming the application at offset 0x00004000
- **Detects pre-inserted USB drives** at boot without requiring unplug/replug
- **Falls back to IAP mode** automatically if application is invalid after reboot

### Memory Layout

```
Flash Memory (288 KB total):
  0x00000000 - 0x00003FFF: Bootloader (16 KB, reserved)
  0x00004000 - 0x0046BFFF: Application (272 KB, max 244 KB usable)
```

### Flashing Options

The unified build system supports three deployment modes:

| Mode | Command | Output File | When to Use |
|------|---------|------------|-------------|
| **Combined** (default) | `make flash` | `build/RISKYZXS_COMBINED.bin` | Fresh cartridge setup or bootloader update + app in one shot |
| **Bootloader only** | `make flash-bootloader` | `build/bootloader/Bootloader.bin` | Bootloader maintenance/recovery |
| **App update via USB** | _(not make)_ | `build/RISKYZXS.UPD` | After bootloader is installed; copy `.UPD` file to USB drive |
| **Standalone (no BL)** | `make flash-standalone` | `build/RISKYSpeccy.bin` | Development or dedicated single-firmware deployment |

### Building All Variants

```sh
# Build bootloader, both app variants, and combined image:
make build-all-versions

# View all output artifact paths:
make versions

# Full help:
make help
```

---

## USB Firmware Upgrades

### Entering IAP (Bootloader) Mode

The bootloader checks a jumper on the **SWDIO / SWCLK** pins at power-on to decide whether to enter IAP mode or jump straight to the application:

| Pin | Role | Signal |
|-----|------|--------|
| **PA13 (SWDIO)** | Output | Driven **low** by bootloader |
| **PA14 (SWCLK)** | Input | Pull-up; reads low when jumpered to PA13 |

**To enter IAP mode:** place a jumper (or short) across the **SWDIO** and **SWCLK** header pins before powering on.

```
SWDIO (PA13) ──┐
               ├── [jumper] ──> PA14 reads LOW  →  IAP mode
SWCLK (PA14) ──┘

No jumper: PA14 pull-up keeps HIGH  →  jump to application
```

**Step-by-step:**

1. **Fit the jumper** across SWDIO / SWCLK on the cartridge header.
2. **Insert your USB drive** with `RISKYZXS.UPD` in the root directory.
3. **Power on** (or reset) the cartridge. The bootloader enters IAP mode automatically.
4. Follow the USB upgrade procedure below.
5. **Remove the jumper** before the next power cycle so the updated firmware boots normally.

> **Note:** Fitting this jumper disables SWD debugging for the duration of boot. Remove it before attaching a WCH-Link for debugging.

---

### Quick Start

Once the bootloader is installed, you can update firmware via USB without a programmer:

1. **Create a USB drive with the firmware update:**
   ```sh
   # Copy the update file to USB drive root:
   cp build/RISKYZXS.UPD /mnt/usb/
   ```

2. **Insert USB drive into cartridge slot** (if not already inserted).

3. **Power on the cartridge:**
   - Bootloader detects the USB drive automatically.
   - It displays: "INSERT USB DRIVE, PRESS ENTER TO CONTINUE"
   - Press **Enter** on ZX keyboard.
   - Bootloader reads `RISKYZXS.UPD` and programs it.
   - Bootloader verifies the image.
   - If successful, bootloader jumps to the new firmware.

### Detailed Process

#### Hardware Requirements

- **USB drive** (FAT12/FAT16/FAT32 formatted)
- **Firmware file** named `RISKYZXS.UPD` in drive root directory


#### Bootloader State Machine

1. **Initialization**
   - Bootloader GPIO PA8 LED blinks slowly (ready state).
   - USB host subsystem initializes.
   - Bootloader polls for USB drive attachment or enumeration events.

2. **USB Detection**
   - If USB is already inserted at power-on, bootloader synthesizes a connect event (no replug needed).
   - Performs device enumeration (get descriptor, set address, retrieve configuration).
   - Mounts Petit FatFS filesystem.

3. **File Lookup**
   - Opens `/RISKYZXS.UPD` from drive root.
   - If file not found, displays error on ZX screen and waits for next retry.

4. **Image Programming**
   - Reads file in 256-byte chunks (USB MSC resilience).
   - Programs into flash at `0x08004000` with adaptive retry on transient read errors.
   - Small delays inserted between reads to stabilize USB state machine.

5. **Verification**
   - Re-opens file and re-reads all bytes.
   - Compares each byte with programmed flash.
   - If mismatch, reports failure and waits.

6. **Boot**
   - If verification passes, bootloader checks if application is valid (non-erased vector word).
   - Jumps to application at `0x08004000`.
   - If application is invalid, stays in IAP mode and shows prompt again.

#### Troubleshooting

> All diagnostic messages below are printed over **USART1 at 115200 8N1**. Connect a serial adapter to the cartridge's UART TX pin to see them.

| Symptom | Cause | Solution |
|---------|-------|----------|
| **"USB not ready (ret=255), re-prompting"** | USB state machine stuck or device not enumerated. | Press Enter again; bootloader will poll and retry. |
| **"File not found"** | `RISKYZXS.UPD` missing from USB root or wrong filename. | Verify file exists: `ls /mnt/usb/RISKYZXS.UPD` |
| **"Read err"** | USB read failure mid-transfer. | Try different USB drive or cable; reduce read chunk size. |
| **"Flash err @0x..."** | Flash write protection or hardware failure. | Verify bootloader is not overwriting itself; check chip erased state. |
| **"Vfy FAIL"** | Verification mismatch (corruption during program). | Retry the upgrade; check USB drive integrity. |

### Bootloader Debug Output

When the bootloader initializes with debug printf enabled, it prints on **USART1 at 115200 8N1**:

```
Initializing GPIO for IAP mode detection...
Initializing IAP subsystem...
Waiting for USB device...
USB dev In.
USB Port 00 Device Enumeration Succeed
...
File not found
```

If using a USB drive, successful upgrade looks like:

```
IAP: 71592 bytes
Disk err, retry 1 @8192, req=256
Disk err, retry 2 @8192, req=128
Read err: 0 @8192/71592
(no error; retry logic converged)
...
Verifying...
Vfy OK
```

### Bootloader vs. Direct Flash

| Aspect | Bootloader Path | Direct Flash (Programmer) |
|--------|-----------------|--------------------------|
| **Setup** | USB drive, ZX keyboard | WCH-Link USB + Flash Tool or OpenOCD |
| **Time** | ~30–60 seconds | ~10–20 seconds |
| **Risk** | Safe; stays in IAP if verification fails. | Higher; incorrect command = bricked chip. |
| **Portability** | USB drive = any computer (Linux/macOS/Windows). | Requires toolchain + programmer. |
| **Use Case** | End-user updates, field deployment. | Development, bootloader recovery. |

---
