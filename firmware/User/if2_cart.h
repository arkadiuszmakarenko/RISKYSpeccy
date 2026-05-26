#ifndef __IF2_CART_H
#define __IF2_CART_H

#include "debug.h"

/* ZX Spectrum Interface 2 ROM cartridge emulation.
 *
 * Reads a .ROM file (any size up to 16 KB) from the mounted FatFs volume
 * into an internal 16 KB buffer (zero-padded), then hands the buffer to the
 * cart engine via ZX_BecomeInterface2() and resets the Z80 so it boots
 * straight into the cartridge.
 *
 * The board acts as a pure 16 KB ROM cart until a hardware reset.
 * Return value: 1 on success, 0 on any error (open / read / size). */
int IF2_LoadRomFromFile (const char *path);

#endif
