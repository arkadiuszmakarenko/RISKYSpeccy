#ifndef __ZX_BUS_H
#define __ZX_BUS_H

#include "debug.h"

void Init_Cart (void);
void RunCart16k (void) __attribute__ ((interrupt ("WCH-Interrupt-fast")));
void RunCartWithRAM (void) __attribute__ ((interrupt ("WCH-Interrupt-fast")));
int ZX_BusReadBlock (uint16_t address, uint8_t *buffer, uint16_t length);
int ZX_BusWriteBlock (uint16_t address, const uint8_t *buffer, uint16_t length);
int ZX_CartRamReadBlock (uint16_t address, uint8_t *buffer, uint16_t length);
int ZX_CartRamWriteBlock (uint16_t address, const uint8_t *buffer, uint16_t length);
void ZX_TriggerNMI (void);
int ZX_BusAcquireDbg (uint32_t *cycles_out);
void ZX_BusReleaseDbg (void);
int ZX_BusWriteReadVerify (uint16_t address, uint8_t value, uint8_t *readback_out);
int ZX_NmiWriteBlock (uint16_t address, const uint8_t *buffer, uint16_t length, uint32_t timeout_ms);
void ZX_CartDrawSuspend (void);
void ZX_CartDrawResume (void);

/* Exposed so callers can size their fill buffers to match one NMI chunk */
#define ZX_NMI_WCMD_CHUNK_EXPOSED 0x0200u


#endif
