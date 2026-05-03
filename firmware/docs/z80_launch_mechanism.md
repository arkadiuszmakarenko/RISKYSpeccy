# Z80 Snapshot Launch Mechanism

This document explains, step by step, how a `.z80` v1 snapshot is loaded and
launched on the ZX Spectrum by the CH32V307 cartridge MCU.

---

## Hardware overview

| Signal   | GPIO          | Direction (default) | Purpose                                  |
|----------|---------------|---------------------|------------------------------------------|
| ROMCS    | GPIOB Pin 3   | Output push-pull HIGH | Selects cartridge ROM over ZX internal ROM |
| /RESET   | GPIOC Pin 6   | Input pull-up (pulse OD LOW to reset) | Z80 hardware reset |
| /INT     | GPIOB Pin 12  | Output open-drain HIGH | Triggers NMI on Z80 (pulsed LOW) |
| Address  | GPIOE[15:0]   | Input floating      | Z80 address bus sampled in ISR           |
| Data     | GPIOD[7:0]    | Input / output PP   | Z80 data bus (driven only during reads)  |
| MREQ     | GPIOB Pin 10  | Input → EXTI trigger | EXTI15_10 fires on every memory access   |

**Key principle**: while ROMCS is driven HIGH by the CH32, the cartridge ROM
(`zxprog`) appears at addresses `0x0000–0x3FFF`.  The cartridge also has a 4 KB
RAM window at `0x3000–0x3FFF` that both the Z80 and the CH32 can access
simultaneously (Z80 via bus, CH32 via `state_pointer->ram[]`).

---

## Phase 1 — Boot: zxprog starts

1. CH32 initialises, configures ROMCS as **push-pull output HIGH**.
2. CH32 pulses `/RESET` LOW for 80 ms then releases it (`ZX_Z80Reset`).
3. Z80 fetches its first opcode from address `0x0000`.  Because ROMCS is HIGH,
   the CH32 cart ISR (`RunCartWithRAM`) serves the byte from `g_zx_image[]`
   (the compiled `zxprog` binary).
4. `zxprog` runs `_startup` (crt0.s):
   - Sets border YELLOW.
   - Enables interrupts (`EI`).
   - Jumps to `main()`.
5. `main()` clears all 48 KB of ZX RAM (`0x4000–0xFFFF`) and enters the
   **poll loop**.

After ~1200 ms the CH32 returns from `ZX_Z80Reset` and starts the monitor.

---

## Phase 2 — ROM body streaming via NMI mailbox

The CH32 cannot write directly to ZX RAM (it has no DMA to the Z80 bus).
Instead it uses an NMI-based mailbox protocol shared with `zxprog`.

**Write-command (WCMD) mailbox** — lives in cart RAM at `0x302E–0x3233`:

```
0x302E  WCMD_SEQ   — CH32 increments this to request a write
0x302F  WCMD_DONE  — zxprog echoes SEQ when the copy is done
0x3030  DST_LO/HI  — target address in ZX RAM
0x3032  LEN_LO/HI  — byte count (max 512 per chunk)
0x3034  DATA[512]  — bytes to copy
```

For each 512-byte chunk:
1. CH32 fills `DATA[]`, sets `DST`, `LEN`, increments `WCMD_SEQ`.
2. CH32 pulses the `/INT` line LOW for 40 µs → **NMI fires on Z80**.
3. `_nmi_wrapper` (crt0.s) saves all registers and calls `nmi_handler_c()`.
4. `nmi_handler_c()` calls `wcmd_poll()`: copies `DATA[]` to ZX RAM and sets
   `WCMD_DONE = SEQ`.
5. CH32 polls `WCMD_DONE` and moves to the next chunk.

The 49 152-byte snapshot body (`0x4000–0xFFFF`) is streamed this way chunk by
chunk (`ZX_BusWriteBlock` → `ZX_NmiWriteBlockInternal`).

---

## Phase 3 — Preparing the launcher

Before triggering the jump, the CH32 writes two data structures to the **cart
RAM** window (`0x3000–0x3FFF`) using `ZX_CartRamWriteBlock` (direct write to
`state_pointer->ram[]`, no NMI needed).

### 3a — Regblock (`0x3F90`, 26 bytes)

Contains all Z80 CPU registers in the exact order that `_zx_launcher` (crt0.s)
consumes them via a `LD SP, #0x3F90` / `POP` sequence:

| Offset | Bytes    | Loaded by                         |
|--------|----------|-----------------------------------|
| +0x00  | C', B'   | `POP BC` → `EXX` → BC'           |
| +0x02  | E', D'   | `POP DE` → `EXX` → DE'           |
| +0x04  | L', H'   | `POP HL` → `EXX` → HL'           |
| +0x06  | F', A'   | `POP AF` → `EX AF,AF'` → AF'     |
| +0x08  | IXl, IXh | `POP IX`                          |
| +0x0A  | IYl, IYh | `POP IY`                          |
| +0x0C  | 0x00, I  | `POP AF` → `LD I,A` (A = I value)|
| +0x0E  | border, R_comp | `POP BC` → `OUT(0xFE),C`; `LD R,B` |
| +0x10  | C, B     | `POP BC` → main BC                |
| +0x12  | E, D     | `POP DE` → main DE                |
| +0x14  | L, H     | `POP HL` → main HL                |
| +0x16  | F, A     | `POP AF` → main AF                |
| +0x18  | SPl, SPh | `LD SP,(0x3FA8)` → user SP        |

**R register compensation**: `_zx_launcher` executes exactly **12 M1 cycles**
between `LD R,A` and the user_pc opcode fetch.  Each M1 auto-increments R
(low 7 bits).  The CH32 pre-subtracts 12 so that R reads correctly in the
game:
```
R_comp = ((snapshot_r & 0x7F) - 12) & 0x7F  |  (snapshot_r & 0x80)
```

### 3b — Launcher tail (`0x3FF0`, 6 bytes)

```
0x3FF0  ED          — IM prefix
0x3FF1  46/56/5E    — IM 0 / IM 1 / IM 2 byte
0x3FF2  FB or 00    — EI (if IFF1 was set) or NOP
0x3FF3  C3          — JP
0x3FF4  pc_lo       — low byte of game entry PC
0x3FF5  pc_hi       — high byte of game entry PC
```

---

## Phase 4 — Arming the ROMCS handover

The CH32 sets `state_pointer->handover_addr = 0x3FF5` inside
`RunCartWithRAM`'s state struct.

This tells the cart ISR: *after you have served the read of address `0x3FF5`
(the JP pc_hi byte — the very last byte of the launcher tail), tristate
ROMCS and shut yourself down.*

```c
/* Inside RunCartWithRAM (fast ISR): */
if (address == sp->handover_addr) {
    sp->handover_addr = 0xFFFFu;
    /* ROMCS (PB3) → input-floating: ZX ULA now controls ROM selection */
    GPIOB->CFGLR = (GPIOB->CFGLR & ~(0xFu << 12)) | (0x4u << 12);
    EXTI->INTENR &= ~sp->IRQLine;
    NVIC_DisableIRQ(EXTI15_10_IRQn);
}
```

**Why 0x3FF5?**  The Z80 fetches the JP instruction in three memory reads:
`C3` (0x3FF3), `pc_lo` (0x3FF4), `pc_hi` (0x3FF5).  After the third read the
Z80 jumps to `user_pc`.  Releasing ROMCS immediately after 0x3FF5 is the
latest possible moment that is still guaranteed to be in cart space, and the
earliest moment at which the cartridge is no longer needed.

---

## Phase 5 — Firing the launcher

1. CH32 clears `LAUNCHER_ALIVE` (`0x3FAA = 0x00`).
2. CH32 writes `LAUNCH_TRIGGER = 0x55` to `0x3F10` (direct cart RAM write).
3. **On the Z80 side**: next time the `main()` poll loop runs it reads
   `LAUNCH_TRIGGER == 0x55` and calls `zx_launcher()` (→ `_zx_launcher` in
   crt0.s).
4. `_zx_launcher`:
   - `DI` — prevents IM1 from corrupting the stack during register restore.
   - Sets border **WHITE** (visual confirmation on screen).
   - Writes `0xAA` to `LAUNCHER_ALIVE` (`0x3FAA`) — CH32 polls this.
   - `LD SP, #0x3F90` and POPs all registers from the regblock (see Phase 3a).
   - `LD SP, (0x3FA8)` — restores user stack pointer.
   - `JP 0x3FF0` — enters the launcher tail.

---

## Phase 6 — Launcher tail and ROMCS tristate (the handover)

The Z80 executes the launcher tail at `0x3FF0` still served by the cart ISR:

```
0x3FF0  ED xx   — sets interrupt mode (IM 0/1/2)
0x3FF2  FB/00   — EI or NOP (restores IFF1)
0x3FF3  C3      — JP
0x3FF4  pc_lo   — \
0x3FF5  pc_hi   —  ← cart ISR fires handover HERE
```

When the ISR serves the `pc_hi` byte at `0x3FF5`:
- ROMCS is switched to **input-floating** (tristate) in the same ISR invocation,
  before the byte is even latched off the bus.
- The cart EXTI interrupt is disabled.
- The data bus is already back to input state (driven only during the read).

The Z80 now executes `JP user_pc`.  From this point:
- Addresses `0x4000–0xFFFF` are game RAM — no ROMCS involvement.
- Addresses `0x0000–0x3FFF` are served by the **ZX Spectrum's internal ROM**
  (the ULA drives ROMCS LOW for those accesses, as normal).
- The cart MCU is entirely invisible to the Z80.

---

## Phase 7 — CH32 confirms launch and returns

Back on the CH32, `ZX_LaunchZ80` polls:

```c
while (state_pointer->ram[0x0FAA] != 0xAAu) {
    Delay_Ms(1u);
    if (++waited >= 3000u) { /* timeout error */ }
}
printf("z80: launched (alive in %lu ms)\n", waited);
```

`state_pointer->ram[0x0FAA]` is the CH32's direct view of cart RAM address
`0x3FAA`.  The 0xAA byte is written by `_zx_launcher` before the register
restore (Phase 5, step 4), so the CH32 sees it within ~1 ms of the trigger.
The ROMCS handover happens a few microseconds later inside the ISR.

---

## Summary timeline

```
CH32                                  Z80 (zxprog / _zx_launcher)
─────────────────────────────────────────────────────────────────
ZX_Z80Reset()                →  Z80 reset, runs zxprog startup
ZX_BusWriteBlock (NMI loop)  →  NMI handler copies body chunks
ZX_CartRamWriteBlock(regblock)   (direct, no NMI)
ZX_CartRamWriteBlock(tail)       (direct, no NMI)
handover_addr = 0x3FF5
ZX_CartRamWriteBlock(ALIVE=0)    (direct, no NMI)
ZX_CartRamWriteBlock(TRIGGER=0x55) →  poll loop detects → calls _zx_launcher
                                       DI; ALIVE←0xAA; restore regs; JP 0x3FF0
poll ALIVE==0xAA ✓                     executing tail at 0x3FF0..0x3FF5
    ISR serves 0x3FF5 →
        ROMCS tristated ✓
        EXTI disabled ✓
                                       JP user_pc → GAME RUNNING
ZX_LaunchZ80 returns 1
```

---

## Files involved

| File | Role |
|------|------|
| [User/zx_bus.c](../User/zx_bus.c) | `RunCartWithRAM` ISR (handover), `ZX_LaunchZ80`, `ZX_RomcsRelease` |
| [User/zx_bus.h](../User/zx_bus.h) | `ZX_Z80State` struct, `ZX_LaunchZ80` declaration |
| [User/z80_loader.c](../User/z80_loader.c) | Parses `.z80` header, streams body, calls `ZX_LaunchZ80` |
| [zx_src/zxprog.c](../zx_src/zxprog.c) | Z80-side poll loop, LAUNCH_TRIGGER check |
| [zx_src/crt0.s](../zx_src/crt0.s) | `_zx_launcher`: register restore and jump to game |
