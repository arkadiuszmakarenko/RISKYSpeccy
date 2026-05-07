#ifndef __TAPE_PLAYER_H
#define __TAPE_PLAYER_H

#include "debug.h"

/*
 * Experimental tape player (.tap + basic .tzx) — feeds EAR bit (data bus bit 6) directly in
 * response to IN A,(#FE) (IORQ + /RD with A0=0), driven by two ISRs:
 *
 *   TAP_TimerISR  — TIM2 one-shot (VTF slot 2), advances tape state machine
 *                   and toggles the current EAR bit at ZX tape timings.
 *
 *   TAP_IorqISR   — EXTI9_5 on PC8 (/IORQ falling edge, VTF slot 1),
 *                   drives data bus bits with current EAR value whenever
 *                   a ULA-port IN (A0=0, /RD active) is detected.
 *
 * Usage from zx_monitor:
 *   1. TAP_Player_Load(path)   — load .tap/.tzx file from USB into internal buffer
 *   2. TAP_Player_Start()      — configure TIM2 + EXTI9_5 and begin playback
 *      (call ZX_RomcsRelease + ZX_Z80Reset before this so Spectrum ROM runs)
 *   3. TAP_Player_IsRunning()  — poll until 0 to detect completion
 *   4. TAP_Player_Stop()       — abort early
 *
 * NOTE: While the IORQ ISR is active it drives the full data bus byte for
 * every IN A,(#FE), overriding the ULA's output.  Keyboard bits 0-4 are
 * returned as 1 (all keys up).  This is intentional: the Spectrum ROM tape
 * loader is in a tight IN loop and does not scan the keyboard while loading.
 *
 * NOTE: Bus contention between the CH32 push-pull output and the ULA driver
 * exists for the duration of each IN A,(#FE) cycle.  This is an experimental
 * implementation; use on hardware known to tolerate brief contention.
 *
 * NOTE: .tzx support currently handles standard-speed data streams and common
 * metadata blocks; turbo/pure/direct/CSW blocks are not yet supported.
 */

/* Load a .tap/.tzx file from the USB filesystem into the player's internal buffer.
   Returns 1 on success, 0 on error. */
int TAP_Player_Load (const char *path);

/* Start tape playback.  Call TAP_Player_Load first.
   Configures TIM2 (VTF slot 2) and EXTI9_5/IORQ (VTF slot 1).
   Begins the pilot tone immediately. */
void TAP_Player_Start (void);

/* Stop playback and release TIM2 + IORQ EXTI. */
void TAP_Player_Stop (void);

/* Returns 1 while tape is playing, 0 when done or stopped. */
int TAP_Player_IsRunning (void);

/* Returns 1 when a tape image is buffered and ready to start. */
int TAP_Player_HasTapeLoaded (void);

void TAP_IorqISR  (void) __attribute__ ((interrupt ("WCH-Interrupt-fast")));
void TAP_TimerISR (void) __attribute__ ((interrupt ("WCH-Interrupt-fast")));

#endif /* __TAPE_PLAYER_H */
