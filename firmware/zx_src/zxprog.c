#include <string.h>
#include <rect.h>
#include <font/fzx.h>
#include "zx.h"

#pragma output CRT_ORG_CODE  = 0
#pragma output CRT_ORG_BSS   = 0x6000
#pragma output REGISTER_SP   = 0xFF00

extern struct fzx_font ff_ao_Sinclair;
static struct fzx_state fs;

#asm
nmi_irq_wrapper:
    push af
    push bc
    push de
    push hl
    push ix
    push iy

    exx
    ex af, af'
    push af
    push bc
    push de
    push hl

    call _nmi_handler_c

    pop hl
    pop de
    pop bc
    pop af
    ex af, af'
    exx

    pop iy
    pop ix
    pop hl
    pop de
    pop bc
    pop af
    retn
#endasm

// Fixed RAM locations that can be inspected by monitor tools.
volatile unsigned char nmi_border_color @ 0x5B00;
volatile unsigned char nmi_irq_count @ 0x5B01;

// Static (rodata in ROM) — fzx_state_init copies it into fs.paper
static const struct r_Rect16 screen = {0, 256, 0, 192};

void nmi_handler_c(void) {
    unsigned char color = (unsigned char)((nmi_border_color + 1u) & 0x07u);
    nmi_border_color = color;
    zx_border(color);
    ++nmi_irq_count;
}

void main(void) {
    nmi_border_color = BLUE;
    nmi_irq_count = 0;

    // Clear screen
    memset((void*)0x4000, 0x00, 6144);
    memset((void*)0x5800, 0x38, 768);   // white paper, black ink

    // Diagnostic: 8 black pixels top-left proves pixel writes work
    //ZX_PIXELS[0] = 0xFF;

    // FZX text — fgnd_attr must be set explicitly (BSS is zero after init)
    fzx_state_init(&fs, &ff_ao_Sinclair, (struct r_Rect16 *)&screen);
    fs.fgnd_attr = 0x38;  // white paper, black ink (ink=pixels, visible on white)
    fs.fgnd_mask = 0x00;  // replace attr fully (don't keep background bits)
    fzx_at(&fs, 0, 4);  // y = baseline; Cayeux is 19px tall so baseline at 18
    fzx_puts(&fs, "Hello World!");

    while (1);
}
