#ifndef __ZX_BUS_H
#define __ZX_BUS_H

#include "debug.h"

void Init_Cart (void);
void RunCart16k (void) __attribute__ ((interrupt ("WCH-Interrupt-fast")));
void RunCartWithRAM (void) __attribute__ ((interrupt ("WCH-Interrupt-fast")));


#endif
