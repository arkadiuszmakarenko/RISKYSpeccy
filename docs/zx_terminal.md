# ZX Terminal — MPU-Rendered Screen and UI

This document explains the architecture of `zx_terminal.c`, what each
subsystem does, and how to use the public API.

---

## Overview

`zx_terminal` is the CH32-side UI layer that renders content onto the ZX
Spectrum screen from the cartridge MCU.  It does **not** run on the Z80 —
the CH32 builds a complete 6144-byte pixel buffer and a 768-byte attribute
buffer in internal SRAM, then pushes the data to ZX RAM (`0x4000–0x5AFF`)
via the NMI WCMD mailbox.  The Spectrum's ULA reads those buffers and
displays them on the screen without any cooperation from the Z80.

Three independent subsystems share the same pair of screen buffers:

| Subsystem | Purpose |
|-----------|---------|
| **Virtual terminal** (`zxtty`) | Software 32×24 VT100-compatible text terminal rendered in the custom 4×7 font |
| **File browser** (`z80select` / `tapselect`) | Interactive full-screen file selector driven by ZX keyboard input |
| **Bridge text** (`zxmsg`) | Single-line status message written at a fixed cart RAM address and rendered by `zxprog` itself |

---

## Screen rendering pipeline

```
CH32 builds:        s_gfx_pixels[6144]   +   s_gfx_attrs[768]
                           │
              ZX_TermFlushBuffers()
                           │
              ┌────────────┴────────────────┐
              │ First call (s_prev_valid=0) │ Subsequent calls (diff mode)
              │ Full write: two chunked NMI │ ZX_BusWriteDiff: only changed
              │ WCMD calls (512 B/chunk)    │ regions (merge gap ≤ 16 B)
              └────────────────────────────┘
                           │
          ZX RAM 0x4000–0x57FF  (pixel data)
          ZX RAM 0x5800–0x5AFF  (attribute data)
                           │
                    ZX ULA displays
```

Differential writes (`ZX_BusWriteDiff`) skip unchanged bytes and merge runs
of unchanged bytes that are ≤ 16 bytes wide.  This keeps NMI traffic low
when only a small part of the screen changes (e.g. a cursor movement or a
single row update in the browser).

---

## Font

All text is rendered using a **4×7 pixel** proportional bitmap font stored
in `s_font4x7[]`.  Each character occupies one full 8×8 attribute cell
(one byte per row, upper 4 bits unused, lower 4 bits expanded 2× to fill
8 pixels).  In selector/browser mode a 1-pixel right-edge gutter is applied
to prevent adjacent characters from visually merging.

Supported characters: `0–9`, `A–Z`, `space`, `"  - . : / _`

Lower-case input is silently up-cased; unsupported characters are replaced
with a space.

---

## Attribute encoding

ZX Spectrum attribute bytes:

```
Bit 7   FLASH  (not used by the terminal)
Bit 6   BRIGHT
Bits 5-3  PAPER colour (0=black … 7=white)
Bits 2-0  INK colour   (0=black … 7=white)
```

Standard colour codes match ANSI SGR 30–37 (foreground) and 40–47
(background).  The default state is **black ink on white paper** (no bright).

---

## Virtual terminal (`zxtty`)

A 32-column × 24-row character terminal with cursor tracking and scroll-up.
Accepts a stream of bytes including:

| Input | Action |
|-------|--------|
| `\r`  | Carriage return (column → 0) |
| `\n`  | Newline (next row; scroll if at bottom) |
| `\b`  | Backspace (erase previous cell) |
| `\t`  | Tab (advance to next 4-column tab stop) |
| `ESC [` … | ANSI CSI sequence |

**Supported ANSI CSI sequences** (subset):

| Sequence | Action |
|----------|--------|
| `ESC [ n A` | Cursor up n rows |
| `ESC [ n B` | Cursor down n rows |
| `ESC [ n C` | Cursor right n columns |
| `ESC [ n D` | Cursor left n columns |
| `ESC [ row ; col H` | Set cursor position (1-based) |
| `ESC [ row ; col f` | Same as `H` |
| `ESC [ J`   | Clear screen |
| `ESC [ K`   | Clear to end of line |
| `ESC [ … m` | SGR — set colours / reset (see below) |

**SGR parameters understood**:

| Code | Effect |
|------|--------|
| `0` or none | Reset (black ink, white paper, no bright) |
| `1` | Bold / bright on |
| `30`–`37` | Set foreground (ink) colour |
| `40`–`47` | Set background (paper) colour |

### Using the terminal from the UART command line

```
zxtty init               — clear screen and reset colours
zxtty hello world        — write "HELLO WORLD" at current cursor
zxtty \e[2JHELLO\n       — clear screen, write "HELLO", newline
zxtty \e[1;32mOK\e[0m    — bright green "OK", then reset
```

Escape sequences in the argument use C-style backslash notation:
`\n` = newline, `\r` = CR, `\t` = tab, `\e` = ESC (`0x1B`), `\\` = `\`.

> The terminal is purely output-only — there is no input path from the Z80
> keyboard to `zxtty`.  Use `z80select` or `tapselect` for interactive UIs.

---

## File browser (`z80select` / `tapselect`)

A full-screen interactive file browser rendered on the ZX screen.  The user
navigates with the ZX Spectrum keyboard; key events are read back to the CH32
via the **KEY mailbox** (`0x3028`/`0x3029`) polled by `ZX_KeyPoll()`.

### Browser layout (32×24)

```
Row 0   "RISKY SPECCY"
Row 1   Current path
Row 2   "Q/A MOVE  O/P PAGE"
Row 3   "ENTER LOAD/OPEN" or "ENTER QUEUE/OPEN" or "ENTER OPEN  0 BACK"
Row 4   (blank)
Rows 5-22  File list (18 entries per page)
Row 23  "N/M FILES" counter
```

Directories are shown as `[NAME]`; the selected entry is highlighted with
inverted colours and prefixed `>`.

### Keyboard controls

| Key | Action |
|-----|--------|
| `Q` | Move selection up |
| `A` | Move selection down |
| `O` | Page up (−18) |
| `P` | Page down (+18) |
| `ENTER` | Open directory / load file / queue tape |
| `0` | Go back to parent directory |

### z80select flow

1. Browser scans the USB filesystem for `.z80`, `.tap`, and `.tzx` files.
2. Selecting a `.z80` file shows a **snapshot info page** (version, PC, SP,
   hardware mode, compression, file size).  Pressing `ENTER` confirms load;
   pressing `0` cancels.
3. After confirmation `Z80_LoadAndRun()` is called directly.  On success the
   game is running and `ZX_TerminalCommandZ80Select` does not return to the
   browser.  On failure an error screen is shown and the browser resets.
4. Selecting a `.tap` or `.tzx` file loads it into the tape player buffer,
   shows a **TAPE READY** screen, then releases ROMCS and resets the Z80 to
   BASIC.  The pending path is stored; the caller checks
   `ZX_TerminalPendingTapSelection()` and can start playback via a button
   press.

### tapselect flow

Like `z80select` but only `.tap` and `.tzx` files are listed.  Selecting a
file loads it into the tape player buffer and resets the Z80 to BASIC.
The pending path is stored for the caller to retrieve via
`ZX_TerminalPendingTapSelection()`.

### Pending selections (used by `zx_monitor`)

Both browsers can end with a pending selection that the caller must act on
**after** the function returns:

```c
ZX_TerminalCommandZ80Select("/");
const char *path = ZX_TerminalPendingZ80Selection();
if (path && path[0]) {
    Z80_LoadAndRun(path);
    ZX_TerminalClearPendingZ80Selection();
}

ZX_TerminalCommandTapSelect("/");
const char *tap = ZX_TerminalPendingTapSelection();
if (tap && tap[0]) {
    /* TAP_Player_Start() triggered externally (e.g. button press) */
    ZX_TerminalClearPendingTapSelection();
}
```

> `zx_monitor.c` uses exactly this pattern for the `z80select`, `tapselect`,
> and auto-start flows.

---

## RAM viewer (`zxview`)

Sends a config to `zxprog` via the VIEW mailbox (`0x302A`–`0x302D`) so the
Z80-side code displays a hex dump of ZX RAM on the border/screen.

```c
ZX_TerminalCommandView(0x4000u, 16u);  /* dump 16 bytes at 0x4000 */
ZX_TerminalCommandViewOff();           /* disable viewer */
```

The VIEW mailbox:

```
0x302A  VIEW_SEQ    — incremented to trigger update
0x302B  VIEW_ADDR_LO — low byte of start address
0x302C  VIEW_ADDR_HI — high byte of start address
0x302D  VIEW_LEN    — byte count (max 16)
```

---

## Bridge text (`zxmsg`)

Writes up to 40 characters to the **BRIDGE mailbox** (`0x3000`–`0x3029`):

```c
ZX_TerminalCommandBridgeText("hello world");
```

The BRIDGE mailbox:

```
0x3000  BRIDGE_SEQ  — incremented to signal new text
0x3001  BRIDGE_LEN  — byte count
0x3002  BRIDGE_TEXT[40] — text payload
```

`zxprog` reads `BRIDGE_SEQ` each NMI cycle; when it changes it copies
`BRIDGE_TEXT[BRIDGE_LEN]` to a fixed screen position and redraws it.

`ZX_TerminalMarkBridgeDirty()` increments `BRIDGE_SEQ` and writes it to cart
RAM without changing the text — used to force a redraw after the screen has
been overwritten by another subsystem.

---

## Initialization

```c
ZX_TerminalInit();   /* call once, before any other terminal function */
```

Resets all internal state: cursor position, colours, ESC parser, diff
buffers, pending selections.  Called by `ZX_Monitor_Init()` at startup.

---

## Draw suspension

While the browser or terminal is actively writing to ZX RAM, it suspends the
`zxprog` main loop's own screen-draw routine to prevent races:

```c
ZX_CartDrawSuspend();   /* set CTRL_FLAGS bit 0; zxprog stops drawing */
/* ... write screen ... */
ZX_CartDrawResume();    /* clear bit 0; zxprog resumes */
ZX_TerminalMarkBridgeDirty();  /* force bridge redraw after resume */
```

This is handled automatically inside `ZX_TerminalCommandZ80Select` and
`ZX_TerminalCommandTapSelect`.

---

## Public API summary

| Function | Description |
|----------|-------------|
| `ZX_TerminalInit()` | Reset all state; call once at startup |
| `ZX_TerminalCommandTermInit()` | Clear virtual terminal and reset colours |
| `ZX_TerminalCommandTermWrite(firstToken)` | Write text (with `\n\r\t\e` escapes) to the virtual terminal |
| `ZX_TerminalCommandView(address, length)` | Enable ZX RAM hex viewer |
| `ZX_TerminalCommandViewOff()` | Disable ZX RAM hex viewer |
| `ZX_TerminalCommandBridgeText(firstToken)` | Send a text message to `zxprog` bridge display |
| `ZX_TerminalMarkBridgeDirty()` | Force bridge text redraw |
| `ZX_TerminalCommandZ80Select(path)` | Open interactive .z80/.tap/.tzx browser |
| `ZX_TerminalCommandTapSelect(path)` | Open interactive .tap/.tzx browser |
| `ZX_TerminalPendingZ80Selection()` | Returns pending .z80 path after `Z80Select` |
| `ZX_TerminalClearPendingZ80Selection()` | Clear pending path |
| `ZX_TerminalPendingTapSelection()` | Returns pending tape path after `TapSelect` |
| `ZX_TerminalClearPendingTapSelection()` | Clear pending path |

---

## Files involved

| File | Role |
|------|------|
| [User/zx_terminal.c](../User/zx_terminal.c) | Full implementation |
| [User/zx_terminal.h](../User/zx_terminal.h) | Public API declarations |
| [User/zx_monitor.c](../User/zx_monitor.c) | Caller: exposes `zxtty`, `zxmsg`, `zxview`, `z80select`, `tapselect` as UART commands |
| [User/zx_bus.c](../User/zx_bus.c) | `ZX_BusWriteBlock`, `ZX_CartRamWriteBlock`, `ZX_KeyPoll` |
| [User/tape_player.c](../User/tape_player.c) | `TAP_Player_Load`, `TAP_Player_Start` called from browser |
| [User/z80_loader.c](../User/z80_loader.c) | `Z80_LoadAndRun`, `Z80_GetFileInfo` called from browser |
