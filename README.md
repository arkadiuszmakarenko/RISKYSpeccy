# RISKYSpeccy

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
| [docs/zx_bus.md](docs/zx_bus.md) | Hardware-level CH32–ZX bus interface: GPIO wiring, `RunCartWithRAM` ISR, ROMCS/NMI/reset control, mailbox protocol |
| [docs/z80_launch_mechanism.md](docs/z80_launch_mechanism.md) | Full `.z80` snapshot launch sequence: parsing, PGCMD paging, register restore, ROMCS handover |
| [docs/tape_player.md](docs/tape_player.md) | `.tap`/`.tzx` playback: TZX→TAP conversion, timing tables, two-ISR state machine, EAR injection |
| [docs/zx_terminal.md](docs/zx_terminal.md) | On-screen UI: virtual terminal, file browser, bridge text, rendering pipeline, public API |
