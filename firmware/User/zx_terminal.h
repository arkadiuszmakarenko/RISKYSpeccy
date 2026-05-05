#ifndef __ZX_TERMINAL_H
#define __ZX_TERMINAL_H

#include <stdint.h>

void ZX_TerminalInit (void);

void ZX_TerminalCommandViewOff (void);
void ZX_TerminalCommandView (uint16_t address, uint8_t length);
void ZX_TerminalCommandBridgeText (char *firstToken);
void ZX_TerminalCommandTermInit (void);
void ZX_TerminalCommandTermWrite (char *firstToken);
void ZX_TerminalCommandZ80Select (const char *path);
void ZX_TerminalCommandTapSelect (const char *path);

const char *ZX_TerminalPendingZ80Selection (void);
void ZX_TerminalClearPendingZ80Selection (void);
const char *ZX_TerminalPendingTapSelection (void);
void ZX_TerminalClearPendingTapSelection (void);
void ZX_TerminalMarkBridgeDirty (void);

#endif
