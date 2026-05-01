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
void ZX_Z80Reset     (void);  /* pulse /RESET LOW then wait 200ms for zxprog init */
int  ZX_RomcsIsReleased (void);

/* Hand control to a Z80 program at start_addr using the cart-RAM launcher.
   Builds a clean Spectrum register state (regblock at 0x3F80) and launcher
   tail (at 0x3FF0), then triggers LAUNCH_TRIGGER so zxprog's main loop calls
   zx_launcher, which restores registers and executes the tail.
   Arms M1 watch at start_addr (and 0x0038 for IM1) via ZX_SnapshotCommitDual.
   Returns 1 on success, 0 on timeout. */
int  ZX_LaunchZ80 (uint16_t start_addr);

/* Snapshot launch primitives.
   ZX_SnapshotEnter redirects Z80 PC to tramp_addr (trampoline already written
   to ZX RAM by caller) using the cart-RAM launcher; polls alive_addr for
   alive_value.  Uses LAUNCH_TRIGGER mechanism — no NMI mailbox seq/target.
   ZX_SnapshotCommit arms M1 handover at handover_addr, writes the go-byte to
   release the trampoline spin, waits for the ISR to drop ROMCS, then performs
   a full ZX_RomcsRelease() tristate. */
int  ZX_SnapshotEnter  (uint16_t tramp_addr, uint16_t alive_addr,
                        uint8_t alive_value, uint32_t timeout_ms);
int  ZX_SnapshotCommit (uint16_t handover_addr, uint16_t go_addr,
                        uint8_t go_value, uint32_t wait_ms);
int  ZX_SnapshotCommitDual (uint16_t handover_addr_a,
                            uint16_t handover_addr_b,
                            uint16_t go_addr,
                            uint8_t go_value,
                            uint32_t wait_ms);

/* After SnapshotCommit*, returns the M1 address that actually triggered the
   ROMCS drop (0xFFFF if handover never fired). */
uint16_t ZX_SnapshotHandoverFiredAddr (void);

/* Post-handover passive bus tracer.  ROMCS is floated so the cart edge
   sees every Z80 cycle without driving anything.  Captures up to 128
   MREQ-low events into a ring buffer.  Use sequence:
       ZX_BusTraceArm();   Delay_Ms(N);   ZX_BusTraceStop();
       ZX_BusTraceDump();
   Reads address bus (GPIOE), data bus (GPIOD), M1/RD/WR (GPIOB). */
void ZX_BusTraceArm    (void);
void ZX_BusTraceSample (uint32_t loop_budget);
void ZX_BusTraceStop   (void);
void ZX_BusTraceDump   (void);

/* Exposed so callers can size their fill buffers to match one NMI chunk */
#define ZX_NMI_WCMD_CHUNK_EXPOSED 0x0200u


#endif
