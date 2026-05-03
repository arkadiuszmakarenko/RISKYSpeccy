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
int ZX_BusAcquireDbg (uint32_t *cycles_out);
void ZX_BusReleaseDbg (void);

/* Poll next ZX key event published by zxprog (ASCII-ish code).
   Returns 1 when a new key is available, 0 when no new key, -1 on read error. */
int ZX_KeyPoll (uint8_t *keycode_out);
void ZX_CartDrawSuspend (void);
void ZX_CartDrawResume (void);

/* ROMCS release path is disabled; assert/reset keep cart ROM path active. */
void ZX_RomcsRelease (void);
void ZX_RomcsAssert  (void);
void ZX_Z80Reset     (void);  /* pulse /RESET LOW then wait 1200ms for zxprog init */


/* Launch/snapshot APIs are disabled and currently return 0. */
int  ZX_LaunchZ80 (uint16_t start_addr);
int  ZX_SnapshotEnter  (uint16_t tramp_addr, uint16_t alive_addr,
                        uint8_t alive_value, uint32_t timeout_ms);
int  ZX_SnapshotCommit (uint16_t handover_addr, uint16_t go_addr,
                        uint8_t go_value, uint32_t wait_ms);
int  ZX_SnapshotCommitDual (uint16_t handover_addr_a,
                            uint16_t handover_addr_b,
                            uint16_t go_addr,
                            uint8_t go_value,
                            uint32_t wait_ms);

/* Returns 0xFFFF when launch/handover is disabled. */
uint16_t ZX_SnapshotHandoverFiredAddr (void);

/* Passive bus tracer. Captures up to 128 MREQ-low events into a ring buffer.
   Use sequence:
       ZX_BusTraceArm();   Delay_Ms(N);   ZX_BusTraceStop();
       ZX_BusTraceDump();
   Reads address bus (GPIOE), data bus (GPIOD), M1/RD/WR (GPIOB). */
void ZX_BusTraceArm    (void);
void ZX_BusTraceSample (uint32_t loop_budget);
void ZX_BusTraceStop   (void);
void ZX_BusTraceDump   (void);


#endif
