#ifndef __CART_H
#define __CART_H

#include "debug.h"

void Init_Cart(void);
void RunCart16k(void) __attribute__((interrupt("WCH-Interrupt-fast")));

#endif
