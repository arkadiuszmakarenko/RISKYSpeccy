# CH32–ZX Spectrum Bus Interface

This document explains how the CH32V307 cartridge MCU connects to the ZX
Spectrum Z80 bus at the hardware level, how the `RunCartWithRAM` ISR serves
memory accesses, and what each higher-level bus function does.

---

## Hardware connections

The CH32V307 is wired directly to the ZX Spectrum edge connector.  All bus
signals are sampled or driven by standard GPIO registers without any glue
logic.

### Address bus — GPIOE[15:0]

All 16 address lines are wired to GPIOE pins 0–15.  The address is captured
as a single 16-bit word with `(uint16_t)GPIOE->INDR` at ISR entry.  All
GPIOE pins are configured as input-floating (CNF=01, MODE=00) and never
driven by the CH32.

### Data bus — GPIOD[7:0]

Data lines D0–D7 are wired to GPIOD pins 0–7.

- **Input** (default): `GPIOD->CFGLR = 0x44444444` — all 8 pins floating.
- **Output** (ROM/RAM read): `GPIOD->CFGLR = 0x33333333` — all 8 pins
  push-pull 50 MHz.

The CH32 drives the bus only for the duration of a confirmed read cycle and
tristates it immediately when `/RD` deasserts.

### Control signals — GPIOB

| Signal | Pin | Mode | Active |
|--------|-----|------|--------|
| ROMCS  | PB3 | PP output (idle HIGH) | HIGH = cart ROM selected |
| /WR    | PB4 | Floating input | LOW |
| /RD    | PB5 | Floating input | LOW |
| BUSACK | PB6 | Floating input | LOW |
| BUSREQ | PB7 | OD output (idle HIGH) | LOW |
| HALT   | PB8 | (not currently used) | — |
| M1     | PB9 | (not currently used) | — |
| /MREQ  | PB10 | IPU input → EXTI15_10 trigger | LOW |
| RESH   | PB11 | (not currently used) | — |
| /INT   | PB12 | OD output (idle HIGH) | LOW = NMI trigger |
| CK_INV | PB13 | Floating input | clock reference |

### Other control signals

| Signal | Pin | Mode | Notes |
|--------|-----|------|-------|
| /RESET | PC6 | IPU (pulse OD LOW to reset) | Z80 hardware reset |
| /WAIT  | PC7 | (not currently used) | — |
| /IORQ  | PC8 | Floating → EXTI9_5 (tape player only) | LOW = I/O cycle |
| BDIR   | PC9 | OD output | (not currently used) |

---

## ISR: `RunCartWithRAM`

This is the hot-path ISR that runs on every Z80 memory access.  It is
registered as a **VTF (Vector Table Free) fast interrupt** in NVIC slot 0
via `SetVTFIRQ` so that entry latency is minimised — there is no function
prologue and no stack frame.  The function carries
`__attribute__((interrupt("WCH-Interrupt-fast")))`.

EXTI line 10 (PB10 = /MREQ) is configured as a **falling-edge** trigger.
Every time the Z80 asserts /MREQ the ISR fires, regardless of whether the
access is a ROM read, RAM read, RAM write, or to an unselected address.

### ISR decision tree

```
/MREQ falls → EXTI15_10_IRQn fires
│
├─ /RD == LOW?  ──────── YES ──────────────────────────────────────────┐
│                                                                       │
│   address < RamBase (0x3000)?                                        │
│   ├─ YES → ROM read                                                  │
│   │        drive data = g_zx_image[address]                         │
│   │        wait /RD HIGH, tristate                                   │
│   │                                                                  │
│   └─ NO → address <= RomLast (0x3FFF)?                              │
│           ├─ YES → Cart RAM read                                     │
│           │        drive data = ram[address - RamBase]               │
│           │        wait /RD HIGH, tristate                           │
│           │        check handover_addr → tristate ROMCS if match    │
│           └─ NO  → ignore (not our address range)                   │
│                                                                       │
└─ /RD == HIGH ────────────────────────────────────────────────────────┘
   Wait for /MREQ+/WR both LOW  (write cycle detection)
   ├─ /WR asserts while /MREQ held?
   │   ├─ YES → address in cart RAM range?
   │   │        ├─ YES → capture data = GPIOD->INDR & 0xFF
   │   │        │        loop while /WR LOW (capture last stable value)
   │   │        │        write to ram[address - RamBase]
   │   │        └─ NO  → ignore
   │   └─ NO  → /MREQ de-asserted before /WR (e.g. opcode fetch M1
   │            where /WR never asserts) → ignore
   │
   Clear EXTI_Line10 pending flag; return
```

### Read path — ROM (0x0000–0x2FFF)

```c
GPIOD->CFGLR = sp->BusOn;          /* PP output */
GPIOD->OUTDR = (GPIOD->OUTDR & ~sp->DataMask) | g_zx_image[address];
while ((GPIOB->INDR & sp->PinRD) == 0u) {}   /* hold until /RD HIGH */
GPIOD->CFGLR = sp->BusOff;         /* floating input */
```

`g_zx_image[]` is the compiled `zxprog` binary embedded as a C array in
flash.  It is 16 KB (`0x0000–0x3FFF`) but only the lower 12 KB
(`0x0000–0x2FFF`) is pure ROM; `0x3000–0x3FFF` is cart RAM.

### Read path — Cart RAM (0x3000–0x3FFF)

Identical to the ROM path except the source is `sp->ram[address - sp->RamBase]`.
After the read completes, the ISR checks whether `address == sp->handover_addr`:
if so it tristates ROMCS and disables itself (snapshot launch handover — see
[z80_launch_mechanism.md](z80_launch_mechanism.md)).

### Write path — Cart RAM

```c
/* Wait until /WR asserts while /MREQ is still held */
while (((GPIOB->INDR & sp->PinMREQ) == 0u) &&
       ((GPIOB->INDR & sp->PinWR)   != 0u)) {}

if (((GPIOB->INDR & sp->PinMREQ) == 0u) &&
    ((GPIOB->INDR & sp->PinWR)   == 0u)) {
    if (address in cart RAM range) {
        GPIOD->CFGLR = sp->BusOff;   /* ensure data bus is input */
        do {
            sp->ram[address - sp->RamBase] = (uint8_t)(GPIOD->INDR & sp->DataMask);
        } while ((GPIOB->INDR & sp->PinWR) == 0u);
    }
}
```

The data is sampled repeatedly while /WR is held to capture the stable
value.  The data bus is kept floating throughout (the Z80 drives it).

---

## ROMCS control

ROMCS (PB3) selects whether the ZX Spectrum sees the cartridge ROM or its
own internal ROM at `0x0000–0x3FFF`.

| State | PB3 mode | Effect |
|-------|----------|--------|
| Cart ROM active | Push-pull HIGH | CH32 ISR serves all `0x0000–0x3FFF` reads |
| Cart ROM released | Input-floating | ZX ULA drives ROMCS normally; internal ROM visible |

**`ZX_RomcsAssert()`** — re-configures PB3 as push-pull HIGH, re-arms the
EXTI vector, and re-enables the NVIC line.  Used before `ZX_Z80Reset()` to
ensure zxprog boots.

**`ZX_RomcsRelease()`** — switches PB3 to input-floating and stops the ISR.
Used before tape playback so the Spectrum's own ROM is available for the
BASIC tape loader.

---

## Z80 reset — `ZX_Z80Reset()`

```
PC6 → OD output, pulled LOW    ← /RESET asserted (80 ms)
PC6 → OD output, released HIGH ← /RESET released
PC6 → IPU input                ← passive monitoring
Delay 1200 ms                  ← wait for zxprog to clear 48 KB RAM
```

The open-drain drive on PC6 allows the cartridge to pull /RESET LOW without
conflicting with the Spectrum's own RC pull-up.  The 1200 ms delay ensures
`zxprog`'s startup RAM clear is complete before any mailbox traffic begins.

---

## NMI trigger — `ZX_TriggerNMI()`

The CH32 triggers an NMI on the Z80 by pulsing `/INT` (PB12, open-drain)
LOW for 40 µs.  Before the pulse it waits for a ZX clock edge (GPIOB.13
inverted clock) to synchronise with the Z80 clock and avoid a pulse that
straddles a cycle boundary.

```c
ZX_WaitClockToggle(ZX_BUS_TIMEOUT);   /* sync to Z80 clock edge */
GPIO_ResetBits(GPIOB, ZX_PIN_INT);    /* pull /INT LOW */
Delay_Us(40u);
GPIO_SetBits(GPIOB, ZX_PIN_INT);      /* release */
```

Despite the signal being labelled `/INT`, the ZX Spectrum NMI hardware uses
this pin as its NMI source.

---

## Higher-level bus operations

### `ZX_BusWriteBlock` — NMI WCMD mailbox write

Writes an arbitrary range of ZX RAM (`0x4000–0xFFFF`) using the NMI mailbox
protocol.  Calls `ZX_NmiWriteBlockInternal` in 512-byte chunks.

For each chunk:
1. Copies payload into `state_pointer->ram[WCMD_DATA - 0x3000]` (direct cart
   RAM write — no bus cycle needed).
2. Writes `WCMD_DST`, `WCMD_LEN`, then increments `WCMD_SEQ`.
3. Calls `ZX_TriggerNMI()`.
4. Polls `WCMD_DONE == WCMD_SEQ` (with 500 µs retry, 200 ms timeout).

The Z80 `nmi_handler_c` → `wcmd_poll()` copies the data from cart RAM to the
target ZX RAM address and echoes the sequence number.

### `ZX_BusReadBlock` — NMI RCMD mailbox read

Reads an arbitrary range of ZX RAM back to the CH32 using the RCMD mailbox
in 64-byte chunks.  Same handshake as WCMD but reversed direction:
zxprog copies the requested bytes into `RCMD_BUF` in cart RAM.

### `ZX_CartRamWriteBlock` / `ZX_CartRamReadBlock` — direct cart RAM

Reads or writes the 4 KB cart RAM window (`0x3000–0x3FFF`) directly via
`state_pointer->ram[]` without triggering an NMI.  The NVIC is briefly
disabled during each transfer to prevent the ISR from concurrently modifying
the same bytes.

```c
NVIC_DisableIRQ(EXTI15_10_IRQn);
for (i = 0; i < length; ++i)
    ram[address - RamBase + i] = buffer[i];
NVIC_EnableIRQ(EXTI15_10_IRQn);
```

Used for all mailbox setup (WCMD, RCMD, PGCMD, LAUNCH_TRIGGER, REGBLOCK,
LAUNCHER_TAIL) where the data must be ready before the NMI fires.

### `ZX_KeyPoll` — read Z80 keyboard events

Reads `KEY_SEQ` (`0x3028`) from cart RAM via `ZX_CartRamReadBlock`.  If the
sequence number has changed since the last call, it also reads `KEY_CODE`
(`0x3029`) and returns the new key code.

---

## `ZXCartState` structure

A single static instance (`s_state`) is used for the lifetime of the firmware.
`state_pointer` is set to `&s_state` by `Init_Cart()` and must not be NULL
before any ISR or bus function runs.

```
struct ZXCartState {
    BusOn       — 0x33333333: CFGLR value to switch GPIOD to PP output
    BusOff      — 0x44444444: CFGLR value to switch GPIOD to floating input
    DataMask    — 0x000000FF: mask for reading GPIOD->INDR (8 data bits)
    RamBase     — 0x3000:  lowest cart RAM address
    RomLast     — 0x3FFF:  highest address served by the cart ISR
    PinRD       — GPIO_Pin_5  (PB5)
    PinWR       — GPIO_Pin_4  (PB4)
    PinMREQ     — GPIO_Pin_10 (PB10)
    IRQLine     — EXTI_Line10
    handover_addr — 0xFFFF (inactive) or 0x3FF5 (launcher armed)
    ram[0x1000] — 4 KB mirror of the ZX cart RAM window 0x3000–0x3FFF
}
```

The `ram[]` array is simultaneously:
- The **source** served to the Z80 for cart RAM reads.
- The **sink** written by the Z80 for cart RAM writes.
- The **mailbox** area accessed directly by the CH32 (`ZX_CartRamWriteBlock`).

Both the ISR and the CH32 main thread access `ram[]` concurrently.  The CH32
uses NVIC disable/enable guards around its own accesses; the ISR does not need
a guard because it runs at higher priority and is inherently atomic per access.

---

## Timing constraints

| Parameter | Value | Notes |
|-----------|-------|-------|
| EXTI→ISR latency (VTF fast) | ~3–5 ns | No stack frame, no prologue |
| Z80 clock (3.5 MHz) | 285 ns / T-state | One T-state = ~285 ns |
| Minimum /MREQ LOW window | 2–3 T-states | ISR must complete within this window |
| CH32 core clock | 144 MHz | ~7 ns/cycle |
| NMI pulse width | 40 µs | >> one Z80 machine cycle |

The ISR is compiled with `-Ofast` (`#pragma GCC optimize("Ofast")`) to ensure
the read path completes before the Z80 de-asserts /RD.  Any function called
from the hot path (data bus drive, tristate) is declared `static inline` or
placed in the same translation unit to avoid call overhead.

---

## Initialization — `Init_Cart()`

Called once from `main()` before `ZX_Z80Reset()`:

1. Sets `BusOn`/`BusOff`/`DataMask`/`RamBase`/`RomLast`/pin constants.
2. Sets `handover_addr = 0xFFFF` (handover inactive).
3. Configures BUSREQ and /INT as open-drain outputs (idle HIGH).
4. Configures ROMCS (PB3) as push-pull HIGH (cart ROM selected).
5. Configures address, data, and control pins as inputs.
6. Clears EXTI pending flag, registers `RunCartWithRAM` as VTF fast ISR on
   EXTI15_10, enables the NVIC line.

---

## Files involved

| File | Role |
|------|------|
| [User/zx_bus.c](../User/zx_bus.c) | `RunCartWithRAM` ISR, all bus functions, ROMCS/NMI/reset control |
| [User/zx_bus.h](../User/zx_bus.h) | Public API and `ZX_Z80State` struct |
| [User/gpio.c](../User/gpio.c) | Early GPIO clock/AFIO enable and EXTI line config (called before `Init_Cart`) |
| [User/zx_image.h](../User/zx_image.h) | `g_zx_image[]` — compiled zxprog binary served as cart ROM |
| [zx_src/crt0.s](../zx_src/crt0.s) / [zxprog.c](../zx_src/zxprog.c) | Z80-side mailbox handlers (WCMD, RCMD, PGCMD, NMI wrapper) |
