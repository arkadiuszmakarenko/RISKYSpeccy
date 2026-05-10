# Bootloader Architecture

## Overview

The RISKY Speccy bootloader is a specialized in-application programming (IAP) monitor that:

1. **Boots first** from flash offset `0x00000000`
2. **Manages application lifetime** — validates, jumps to, and recovers from invalid application states
3. **Provides USB-based firmware updates** via Petit FatFS + USB MSC
4. **Implements robust USB polling** with automatic attach detection and fallback enumeration
5. **Protects itself** — hard-wired 16 KB memory boundary ensures app writes never corrupt bootloader load image

---

## Memory Map

### Bootloader Regions

```
FLASH Memory (CH32V30x with 288 KB):
┌─────────────────────────┬─────────────────────────────────────────┐
│ Bootloader              │ Application + IAP Update Slot           │
│ 0x00000000..0x00003FFF  │ 0x00004000..0x0046BFFF                 │
│ (16 KB reserved)        │ (272 KB available; ~244 KB typical max) │
└─────────────────────────┴─────────────────────────────────────────┘

RAM Memory (32 KB):
┌────────────────────────────────────────────────────────────────┐
│ Stack, data, BSS for bootloader + USB host + FAT subsystems   │
│ 0x20000000..0x20007FFF                                         │
└────────────────────────────────────────────────────────────────┘
```

### Bootloader Linker Script

**File:** `Bootloader/Ld/Link.ld`

Key settings:

- **BOOT_FLASH:** `ORIGIN=0x00000000, LENGTH=16K` — hard boundary on loadable sections
- **All `.text`, `.data` LMA mapped to BOOT_FLASH** — linker enforces overflow error if bootloader > 16 KB
- **PROVIDE(_bootloader_limit = 0x00004000)** — marks app start boundary

### Application Linker Script (Bootloader-linked variant)

**File:** `firmware/Ld/Link.ld`

- **FLASH:** `ORIGIN=0x00004000, LENGTH=192K` — app linked with 0x4000 base
- App is exactly 192 KB (0x00004000..0x0046BFFF)
- Final `.data` LMA = bootloader limit for proper ROM LMA placement

### Application Linker Script (Standalone variant)

**File:** `firmware/Ld/Link_standalone.ld`

- **FLASH:** `ORIGIN=0x00000000, LENGTH=192K` — full chip, no bootloader
- Used for development or direct flash without IAP

---

## Boot Flow

### Startup Sequence (main.c)

```
1. Delay_Init()          -- Setup system timer
2. USART_Printf_Init()   -- Configure debug UART
3. Enable GPIOA + AFIO clocks
4. GPIO_PinRemapConfig(SWJ_Disable) -- Release PA13/PA14 from SWD alternate function
5. PA13 → push-pull output, driven LOW
   PA14 → input pull-up
6. Check PA14 level
   └─ PA14 LOW (jumpered to PA13): Enter IAP mode
      ├─ IAP_Initialization()  -- USB host, RCC, global state
      ├─ PA8 → open-drain output (LED)
      ├─ blinkLed()
      └─ IAP_Main_Deal() loop (USB update flow)
   └─ PA14 HIGH (no jumper): Jump to application
      └─ IAP_Jump_APP() 
```

### State Machine (IAP_Main_Deal in usb_host_iap.c)

**Goal:** Read file from USB FAT drive, program to flash, verify, and jump to app.

```
┌─ IAP_USBH_PreDeal()
│  ├─ Check root hub port status
│  ├─ If already attached (hardware flag set) but no connect event fired
│  │  └─ Synthesize connect event (handles pre-inserted drives)
│  ├─ Enumerate device
│  │  ├─ Get descriptor
│  │  ├─ Set device address
│  │  ├─ Get config descriptor
│  │  ├─ Parse MSC interface + bulk endpoints
│  │  └─ Return DEF_SUCCESS or error
│  └─ State → ROOT_DEV_SUCCESS = ready
│
├─ PFF_Check_And_Mount()
│  ├─ Initialize disk subsystem (SCSI READ CAPACITY)
│  ├─ pf_mount() — mount FAT filesystem
│  └─ Return 0 if mounted, else error
│
├─ pf_open("RISKYZXS.UPD")
│  └─ If file not found → retry or wait
│
├─ Read file in chunks
│  ├─ pf_read() with adaptive retries
│  ├─ On FR_DISK_ERR:
│  │  ├─ Reduce chunk size (512 → 256 → 128 → 64 bytes)
│  │  ├─ Reset disk state machine
│  │  ├─ Re-mount filesystem
│  │  ├─ Re-seek to offset
│  │  └─ Retry pf_read()
│  └─ Load into RAM buffer
│
├─ IAP_Flash_Program()
│  ├─ Erase pages as needed
│  ├─ Write aligned 256-byte blocks
│  ├─ Verify keys (flash runaway protection)
│  └─ Return 0 if success
│
├─ Verify by re-opening file
│  ├─ pf_read() entire file again
│  ├─ Compare each byte with flash
│  └─ If match → Vfy OK; else Vfy FAIL
│
└─ If success: IAP_Jump_APP()
   ├─ Set software interrupt (Software_IRQn)
   ├─ Trigger it
   ├─ CPU jumps to app @ 0x08004000
```

---

## USB Host & FAT Integration

### USB Host Stack

**File:** `Bootloader/User/USB_Host/ch32v30x_usbfs_host.c`

- **USBFSH_CheckRootHubPortStatus()** — polls device attachment state
- **USBFSH_EnableRootHubPort()** — waits for post-reset device detection
- **USBFSH_GetDeviceDescr()** — retrieves device descriptor with retries
- **USBFSH_CtrlTransfer()** — sends control requests (SET ADDR, GET CONFIG)
- **USBFSH_SendEndpData() / GetEndpData()** — bulk data transfer with NAK/STALL handling

**Key Tuning:**
- Bulk endpoint timeout: `200 ms` (allows NAK retry without aborting)
- Reset recovery: `DEF_RE_ATTACH_TIMEOUT = 100 ms`

### Petit FatFS Integration

**File:** `Bootloader/User/Pff/diskio.c`

- **disk_initialize()** — SCSI READ CAPACITY to detect drive presence
- **disk_readp()** — sector read with:
  - CBW (Command Block Wrapper) send
  - Data phase
  - CSW (Command Status Wrapper) receive
  - Multi-stage retry on error (TUR, reset, re-init, final fail print with sense data)

**Key Features:**
- Adaptive chunk size: starts at 512 bytes, halves on error
- Sense data logging: prints ASC/ASCQ for SCSI diagnostics
- Geometry detection: logs last LBA and block size on failure

---

## Recovery & Fallback Mechanisms

### Pre-inserted USB Drive Detection

**Problem:** If USB is already in the slot at power-on, the attach interrupt may not fire, leaving the device stuck in idle state.

**Solution (firmware/User/FATFS/usb_disk.c and Bootloader):**
```c
hw_attached = ((USBFSH->MIS_ST & USBFS_UMS_DEV_ATTACH) != 0) ? 1 : 0;
if ((ret == ROOT_DEV_FAILED) && hw_attached &&
    (RootHubDev[usb_port].bStatus != ROOT_DEV_SUCCESS)) {
    ret = ROOT_DEV_CONNECTED;  // Force enum
}
```

### Invalid Application / No Jumper

If the jumper is absent at power-on, the bootloader jumps unconditionally to the application address (`0x08004000`). If no valid firmware is present at that address, the behaviour is undefined — the chip will fault or spin. In this case, refit the jumper and power-cycle to re-enter IAP mode and re-flash.

### Persistent USB Retry Loop

**Problem:** Firmware terminal used to spin `ret=255` infinitely if USB never achieved ready state.

**Solution (firmware/User/zx_terminal.c):**
```c
/* Poll with timeout + adaptive re-init on default state */
for (tries = 0; tries < 100; ++tries) {
    usb_ret = USBH_PreDeal();
    if (usb_ret == DEF_SUCCESS) break;
    Delay_Ms(20u);
}
if (usb_ret == DEF_DEFAULT) {
    ClearUSB();  // Reset state
    USB_Initialization();
    Delay_Ms(50u);
    /* Retry briefly */
    for (tries = 0; tries < 50; ++tries) {
        usb_ret = USBH_PreDeal();
        if (usb_ret == DEF_SUCCESS) break;
        Delay_Ms(20u);
    }
}
```

---

## Build Targets

### Root Makefile (Makefile)

**Key rules:**

| Target | Effect |
|--------|--------|
| `make bootloader` | Compiles and links Bootloader/.* → build/bootloader/Bootloader.elf/.bin |
| `make app-bootloader` | Links app at 0x4000 → build/RISKYZXS.UPD |
| `make app-standalone` | Links app at 0x0000 → build/RISKYSpeccy.bin |
| `make combine` | Pads to 16 KB + cat bootloader.bin + app.bin → build/RISKYZXS_COMBINED.bin |
| `make all` or `make build-all-versions` | Build all four above |
| `make flash` | Flash combined image to 0x0000 |
| `make flash-bootloader` | Flash bootloader only |
| `make flash-app-upd` | Flash app update image to 0x4000 |
| `make flash-standalone` | Flash standalone app to 0x0000 |

### Bootloader Compile Settings

- **Optimization:** `-Os` (size) to fit 16 KB boundary
- **Debug logs:** Controlled by `DEF_DEBUG_PRINTF` in `Bootloader/User/USB_Host/usb_host_config.h`
- **Compiler flags:** Same RISC-V ABI as app, but no `-Ofast`

---

## Testing & Validation

### Manual Testing Checklist

1. **Boot to IAP with PA13/PA14 jumpered**
   - Fit jumper across SWDIO (PA13) and SWCLK (PA14) header pins
   - `Initializing GPIO for IAP mode detection...` then `Initializing IAP subsystem...` prints on USART1
   - LED blinks
   - Prompt appears on ZX screen

2. **USB enumeration**
   - Insert USB drive with test file
   - Press Enter
   - `USB dev In.` and `USB Port 00 Device Enumeration Succeed` print

3. **File programming**
   - Check `IAP: XXXX bytes` matches file size
   - Verify no read errors or `Len err` prints

4. **Reboot behavior**
   - After successful upgrade, reboot without USB
   - App starts directly (no IAP prompt)
   - LED stops blinking

5. **Recovery from missing/corrupt firmware**
   - Fit the jumper across SWDIO/SWCLK
   - Power-cycle the cartridge
   - Bootloader enters IAP mode (jumper present)
   - Re-flash via USB drive

### Automated Build Validation

```sh
make clean
make build-all-versions
make versions  # Print all artifact paths
# Inspect build/bootloader/Bootloader.siz — should be < 16 KB
# Inspect build/RISKYZXS.UPD — should match firmware size
```

---

## Known Limitations & Future Work

1. **Single USB device** — Only one MSC drive supported at a time
2. **FAT cluster chain** — Petit FatFS may struggle with highly fragmented drives
3. **No rollback** — If verification fails, no automatic revert to previous version
4. **LED blink pattern** — Currently just slow blink; could encode state more clearly (e.g., rapid on error)
5. **Timeout configurable** — USB timeouts are hardcoded; could be made tunable via config sector

---

## References

- CH32V30x Datasheet: `http://www.wch.cn/downloads/CH32V30xRM_PDF.html`
- Petit FatFS: `http://elm-chan.org/fsw/ff/00index_p.html`
- USB Mass Storage Class: USB-IF BOT Specification
- WCH OpenOCD Setup: `http://www.mounriver.com/`
