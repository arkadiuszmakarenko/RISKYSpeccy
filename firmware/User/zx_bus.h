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

/* ROMCS release path is disabled; assert/reset keep cart ROM path active. */
void ZX_RomcsRelease (void);
void ZX_RomcsAssert  (void);
void ZX_Z80Reset     (void);  /* pulse /RESET LOW then wait 1200ms for zxprog init */
int  ZX_RomcsIsReleased (void);



#endif
