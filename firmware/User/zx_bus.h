#ifndef __ZX_BUS_H
#define __ZX_BUS_H

#include "debug.h"

void Init_Cart (void);
void RunCart16k (void) __attribute__ ((interrupt ("WCH-Interrupt-fast")));
void RunCartWithRAM (void) __attribute__ ((interrupt ("WCH-Interrupt-fast")));
void RunCartWithM1Watch (void) __attribute__ ((interrupt ("WCH-Interrupt-fast")));
int ZX_BusReadBlock (uint16_t address, uint8_t *buffer, uint16_t length);
int ZX_BusWriteBlock (uint16_t address, const uint8_t *buffer, uint16_t length);
int ZX_CartRamReadBlock (uint16_t address, uint8_t *buffer, uint16_t length);
int ZX_CartRamWriteBlock (uint16_t address, const uint8_t *buffer, uint16_t length);
void ZX_TriggerNMI (void);
int ZX_BusAcquireDbg (uint32_t *cycles_out);
void ZX_BusReleaseDbg (void);
int ZX_BusWriteReadVerify (uint16_t address, uint8_t value, uint8_t *readback_out);
int ZX_NmiWriteBlock (uint16_t address, const uint8_t *buffer, uint16_t length, uint32_t timeout_ms);
int ZX_NmiReadBlock  (uint16_t address, uint8_t *buffer, uint8_t length, uint32_t timeout_ms);
void ZX_CartDrawSuspend (void);
void ZX_CartDrawResume (void);

/* ROMCS release: disable the cart ROM/RAM responses (ISR early-returns) and
   drive the ZX edge-connector ROMCS pin low so the internal Spectrum ROM
   takes over the 0x0000-0x3FFF range. ZX_RomcsAssert() restores cart-ROM
   mode. ZX_RomcsIsReleased() reports the current state. */
void ZX_RomcsRelease (void);
void ZX_RomcsAssert  (void);
int  ZX_RomcsIsReleased (void);

/* Hand control to a Z80 program at start_addr.  Installs a small trampoline
   in upper ZX RAM (0xFFC0..) that spin-waits for CH32 to release ROMCS, then
   triggers an NMI with the launch mailbox set so the cart-ROM NMI handler
   redirects Z80 PC into the trampoline.  Once the trampoline is alive (via
   poll over BUSREQ) CH32 sets the release flag and clears ROMCS.  Returns 1
   on success, 0 on timeout. */
int  ZX_LaunchZ80 (uint16_t start_addr);

/* Two-phase launch (used by the TAP loader so it can BUSREQ-write data into
   zxprog's BSS/stack region after zxprog has been replaced by the trampoline).
   Call ZX_LaunchPrepare first; on success the Z80 is in the spin-loop at
   0xFFC0 waiting for the go-flag.  Do whatever extra BUSREQ writes are
   needed, then call ZX_LaunchCommit() to release ROMCS and let the Z80 jump
   to the user program. */
int  ZX_LaunchPrepare (uint16_t start_addr);
int  ZX_LaunchCommit  (void);

/* Snapshot launch primitives (.z80 loader).
   ZX_SnapshotEnter installs a launch-mailbox sequence that makes the cart-ROM
   NMI handler redirect Z80 PC to tramp_addr; polls alive_addr for alive_value.
   ZX_SnapshotCommit arms M1 handover at handover_addr, BUSREQ-writes the
   go-byte to release the trampoline spin, waits for the ISR to drop ROMCS,
   then performs a full ZX_RomcsRelease() tristate. */
int  ZX_SnapshotEnter  (uint16_t tramp_addr, uint16_t alive_addr,
                        uint8_t alive_value, uint32_t timeout_ms);
int  ZX_SnapshotCommit (uint16_t handover_addr, uint16_t go_addr,
                        uint8_t go_value, uint32_t wait_ms);

/* Exposed so callers can size their fill buffers to match one NMI chunk */
#define ZX_NMI_WCMD_CHUNK_EXPOSED 0x0200u


#endif
