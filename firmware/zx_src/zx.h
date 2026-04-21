#ifndef ZX_H
#define ZX_H

#include <stdint.h>

/* ZX Spectrum memory map */
#define PIXEL_BASE   0x4000u   /* Pixel RAM: 0x4000..0x57FF (6144 bytes) */
#define ATTR_BASE    0x5800u   /* Attribute RAM: 0x5800..0x5AFF (768 bytes) */
#define PIXEL_SIZE   0x1800u
#define ATTR_SIZE    0x0300u

/* Attribute byte: FBPPPIII (Flash, Bright, Paper[2:0], Ink[2:0]) */
#define ATTR(bright, paper, ink)  (((bright) << 6) | (((paper) & 7) << 3) | ((ink) & 7))

/* Standard colors */
#define BLACK    0
#define BLUE     1
#define RED      2
#define MAGENTA  3
#define GREEN    4
#define CYAN     5
#define YELLOW   6
#define WHITE    7

/* ULA port: bits [2:0] = border color, bit 4 = MIC, bit 3 = EAR */
__sfr __at(0xFE) ULA_PORT;

#define zx_border(color)  (ULA_PORT = (color) & 0x07)

#endif /* ZX_H */
