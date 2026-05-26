#ifndef __ZX_BUS_H
#define __ZX_BUS_H

#include "debug.h"

void Init_Cart (void);

void RunCartWithRAM (void) __attribute__ ((interrupt ("WCH-Interrupt-fast")));

int ZX_BusReadBlock (uint16_t address, uint8_t *buffer, uint16_t length);
int ZX_BusWriteBlock (uint16_t address, const uint8_t *buffer, uint16_t length);
int ZX_CartRamReadBlock (uint16_t address, uint8_t *buffer, uint16_t length);
int ZX_CartRamWriteBlock (uint16_t address, const uint8_t *buffer, uint16_t length);
void ZX_TriggerNMI (void);

/* Poll next ZX key event published by zxprog (ASCII-ish code).
   Returns 1 when a new key is available, 0 when no new key, -1 on read error. */
int ZX_KeyPoll (uint8_t *keycode_out);
void ZX_CartDrawSuspend (void);
void ZX_CartDrawResume (void);

/* Full Z80 CPU state used to restore a .z80 v1 snapshot. */
typedef struct {
    uint8_t  a,  f;
    uint8_t  a_alt, f_alt;
    uint16_t bc,     de,     hl;
    uint16_t bc_alt, de_alt, hl_alt;
    uint16_t ix, iy;
    uint16_t sp, pc;
    uint8_t  i, r;
    uint8_t  iff1;
    uint8_t  im;      /* interrupt mode: 0, 1, or 2 */
    uint8_t  border;
} ZX_Z80State;

/* Release ROMCS to input-floating (tristate) and disable the cart ISR.
   Call only in an emergency / abort path; normal launch uses the automatic
   ISR handover armed by ZX_LaunchZ80. */
void ZX_RomcsRelease (void);
void ZX_RomcsAssert  (void);
void ZX_Z80Reset     (void);  /* pulse /RESET LOW then wait 1200ms for zxprog init */

/* Switch the cart engine into Interface 2 pure-ROM mode.
 * rom_16k must point to a 16 KB image (zero-padded if the original cart was
 * smaller).  After return the cart ISR serves the full 0x0000..0x3FFF window
 * from this buffer; the launcher mailbox shadow is disabled.  Call
 * ZX_Z80Reset() right after to boot the Z80 into the cartridge.  Returning
 * to launcher mode requires a hardware reset / power cycle.
 */
void ZX_BecomeInterface2 (const uint8_t *rom_16k);

/* Send a byte to port 0x7FFD via the PGCMD NMI mailbox.
 * Used to control 128K ROM/RAM paging from the CH32 side.
 * On 48K hardware the Z80 OUT is a no-op; on 128K it selects the ROM
 * and/or the RAM bank paged in at 0xC000.
 * Returns 1 on success, 0 on timeout. */
int ZX_128kPage (uint8_t val);

/* Restore the full Z80 CPU state from a snapshot and launch the game.
 *
 * Sequence:
 *   1. Writes regblock (26 bytes) to cart RAM at 0x3F90 for _zx_launcher.
 *   2. Writes launcher tail (6 bytes) to cart RAM at 0x3FF0:
 *      [ED, IM_byte, EI/NOP, C3, pc_lo, pc_hi].
 *   3. Arms ROMCS handover: after the cart ISR serves address 0x3FF5
 *      (the JP pc_hi byte — the last byte before the Z80 jumps to game space),
 *      ROMCS is switched to input-floating (tristate) and the cart ISR is
 *      disabled, handing control cleanly to the ZX Spectrum hardware.
 *   4. Writes LAUNCH_TRIGGER = 0x55 to 0x3F10; zxprog detects this and calls
 *      _zx_launcher, which restores all registers and jumps to the game.
 *   5. Polls LAUNCHER_ALIVE (0x3FAA) until 0xAA appears (launcher fired) or
 *      timeout.
 *
 * Returns 1 on success, 0 on error or timeout.
 */
int ZX_LaunchZ80 (const ZX_Z80State *state);

#endif
