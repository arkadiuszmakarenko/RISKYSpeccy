#ifndef __ZX_TERMINAL_H
#define __ZX_TERMINAL_H

#include <stdint.h>

void ZX_TerminalInit (void);
void ZX_TerminalWaitUsbDriveReady (void);

void ZX_TerminalCommandViewOff (void);
void ZX_TerminalCommandView (uint16_t address, uint8_t length);
void ZX_TerminalCommandBridgeText (char *firstToken);
void ZX_TerminalCommandTermInit (void);
void ZX_TerminalCommandTermWrite (char *firstToken);
int  ZX_TerminalCommandZ80Select (const char *path);  /* returns 1 if a z80 game was launched */
void ZX_TerminalCommandTapSelect (const char *path);

const char *ZX_TerminalPendingZ80Selection (void);
void ZX_TerminalClearPendingZ80Selection (void);
const char *ZX_TerminalPendingTapSelection (void);
void ZX_TerminalClearPendingTapSelection (void);
void ZX_TerminalMarkBridgeDirty (void);

/* USB-lost signalling: the blocking browser loops call
 * ZX_TerminalPollUsb() each iteration; if the host stack reports the
 * drive gone, ZX_TerminalUsbLost() returns 1 and the browser returns
 * to the main loop.  ZX_TerminalClearUsbLost() resets the latch. */
uint8_t ZX_TerminalUsbLost (void);
void ZX_TerminalClearUsbLost (void);
void ZX_TerminalPollUsb (void);

#endif
