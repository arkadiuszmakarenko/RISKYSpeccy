#include <string.h>
#include <rect.h>
#include <font/fzx.h>
#include "zx.h"

#pragma output CRT_ORG_CODE  = 0
#pragma output CRT_ORG_BSS   = 0x6000
#pragma output REGISTER_SP   = 0xFF00

extern struct fzx_font ff_ao_GenevaMonoRoman;
static struct fzx_state fs;
extern unsigned int in_Inkey(void);

#ifndef MONITOR_START
#define MONITOR_START 0x8000u
#endif

#define NMI_MAILBOX_CMD_ADDR 0x8100u
#define NMI_MAILBOX_LEN_ADDR 0x8101u
#define NMI_MAILBOX_DST_LO   0x8102u
#define NMI_MAILBOX_DST_HI   0x8103u
#define NMI_MAILBOX_SEQ_ADDR 0x8104u
#define NMI_MAILBOX_ERR_ADDR 0x8105u

#define NMI_CMD_NONE               0u
#define NMI_CMD_WRITE_FROM_SCRATCH 1u
#define NMI_CMD_READ_TO_SCRATCH    2u
#define NMI_SCRATCH_ADDR           0x8200u

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
volatile unsigned char nmi_copy_cmd @ NMI_MAILBOX_CMD_ADDR;
volatile unsigned char nmi_copy_len @ NMI_MAILBOX_LEN_ADDR;
volatile unsigned char nmi_copy_dst_lo @ NMI_MAILBOX_DST_LO;
volatile unsigned char nmi_copy_dst_hi @ NMI_MAILBOX_DST_HI;
volatile unsigned char nmi_copy_seq @ NMI_MAILBOX_SEQ_ADDR;
volatile unsigned char nmi_copy_err @ NMI_MAILBOX_ERR_ADDR;

// Static (rodata in ROM) — fzx_state_init copies it into fs.paper
static const struct r_Rect16 screen = {0, 256, 0, 192};

static char line_buf[48];
static unsigned int monitor_addr = (unsigned int)MONITOR_START;
static unsigned char edit_pending = 0;
static unsigned char edit_high = 0;
static unsigned char goto_mode = 0;
static unsigned char goto_digits = 0;
static unsigned int goto_addr = 0;
static const unsigned char line_pitch = 12u;

static char hex_digit(unsigned char nibble) {
    nibble &= 0x0Fu;
    return (nibble < 10u) ? (char)('0' + nibble) : (char)('A' + (nibble - 10u));
}

static int parse_hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'A' && c <= 'F') return (int)(10 + (c - 'A'));
    if (c >= 'a' && c <= 'f') return (int)(10 + (c - 'a'));
    return -1;
}

static void hex8_to_str(unsigned char v, char *out) {
    out[0] = hex_digit((unsigned char)(v >> 4));
    out[1] = hex_digit(v);
    out[2] = 0;
}

static void hex16_to_str(unsigned int v, char *out) {
    out[0] = hex_digit((unsigned char)(v >> 12));
    out[1] = hex_digit((unsigned char)(v >> 8));
    out[2] = hex_digit((unsigned char)(v >> 4));
    out[3] = hex_digit((unsigned char)v);
    out[4] = 0;
}

static void line_print(unsigned char row, const char *text) {
    fzx_at(&fs, 0, (unsigned int)(12u + ((unsigned int)row * (unsigned int)line_pitch)));
    fzx_puts(&fs, (char *)text);
}

static void line_clear(void) {
    unsigned char i;
    for (i = 0; i < sizeof(line_buf); ++i) line_buf[i] = 0;
}

static void draw_memory_row(unsigned int addr, unsigned char ui_row) {
    unsigned char i;
    unsigned int p = 0;
    char hx[3];
    char ax[5];

    hex16_to_str(addr, ax);
    line_clear();

    line_buf[p++] = ax[0];
    line_buf[p++] = ax[1];
    line_buf[p++] = ax[2];
    line_buf[p++] = ax[3];
    line_buf[p++] = ':';
    line_buf[p++] = ' ';

    for (i = 0; i < 8u; ++i) {
        unsigned int cur = (unsigned int)(addr + i);
        unsigned char val = *((volatile unsigned char *)cur);
        hex8_to_str(val, hx);

        if (cur == monitor_addr) {
            line_buf[p++] = '[';
            line_buf[p++] = hx[0];
            line_buf[p++] = hx[1];
            line_buf[p++] = ']';
        } else {
            line_buf[p++] = ' ';
            line_buf[p++] = hx[0];
            line_buf[p++] = hx[1];
            line_buf[p++] = ' ';
        }
    }

    line_buf[p] = 0;
    line_print(ui_row, line_buf);
}

static void draw_ui(void) {
    unsigned int block_base = monitor_addr & 0xFFF8u;
    unsigned char val = *((volatile unsigned char *)monitor_addr);
    unsigned char row;
    char hx8[3];
    char hx16[5];

    memset((void*)0x4000, 0x00, 6144);
    memset((void*)0x5800, 0x38, 768);

    line_print(0, "ZX RAM Monitor");

    hex16_to_str(monitor_addr, hx16);
    hex8_to_str(val, hx8);
    line_clear();
    line_buf[0] = 'A'; line_buf[1] = 'd'; line_buf[2] = 'd'; line_buf[3] = 'r'; line_buf[4] = ':'; line_buf[5] = ' ';
    line_buf[6] = hx16[0]; line_buf[7] = hx16[1]; line_buf[8] = hx16[2]; line_buf[9] = hx16[3];
    line_buf[10] = ' '; line_buf[11] = 'V'; line_buf[12] = 'a'; line_buf[13] = 'l'; line_buf[14] = ':'; line_buf[15] = ' ';
    line_buf[16] = hx8[0]; line_buf[17] = hx8[1];
    line_print(1, line_buf);

    if (goto_mode) {
        char ga[5];
        hex16_to_str(goto_addr, ga);
        line_clear();
        line_buf[0] = 'G'; line_buf[1] = 'o'; line_buf[2] = 't'; line_buf[3] = 'o'; line_buf[4] = ':'; line_buf[5] = ' ';
        line_buf[6] = ga[0]; line_buf[7] = ga[1]; line_buf[8] = ga[2]; line_buf[9] = ga[3];
        line_buf[10] = ' '; line_buf[11] = '('; line_buf[12] = '4'; line_buf[13] = ' '; line_buf[14] = 'h'; line_buf[15] = 'e'; line_buf[16] = 'x'; line_buf[17] = ')';
        line_print(2, line_buf);
    } else if (edit_pending) {
        line_print(2, "Edit: enter LOW nibble");
    } else {
        line_print(2, "Edit: 0-9 A-F (2 keys)");
    }

    line_print(3, "Move: Q/W +-1  J/M +-8");
    for (row = 0; row < 10u; ++row) {
        draw_memory_row((unsigned int)(block_base + ((unsigned int)row * 0x08u)), (unsigned char)(4u + row));
    }
    line_print(14, "O/P dec/inc  G goto  C cancel");
}

static unsigned char monitor_handle_key(unsigned char k) {
    int nib;

    if (goto_mode) {
        if (k == 'C' || k == 'c') {
            goto_mode = 0;
            goto_digits = 0;
            goto_addr = 0;
            return 1;
        }

        nib = parse_hex_digit(k);
        if (nib >= 0) {
            goto_addr = (unsigned int)(((goto_addr << 4) & 0xFFFFu) | (unsigned int)nib);
            ++goto_digits;
            if (goto_digits >= 4u) {
                monitor_addr = goto_addr;
                goto_mode = 0;
                goto_digits = 0;
                goto_addr = 0;
            }
            return 1;
        }
        return 0;
    }

    if (k == 'Q' || k == 'q') {
        monitor_addr = (unsigned int)(monitor_addr - 1u);
        return 1;
    }
    if (k == 'W' || k == 'w') {
        monitor_addr = (unsigned int)(monitor_addr + 1u);
        return 1;
    }
    if (k == 'J' || k == 'j') {
        monitor_addr = (unsigned int)(monitor_addr - 8u);
        return 1;
    }
    if (k == 'M' || k == 'm') {
        monitor_addr = (unsigned int)(monitor_addr + 8u);
        return 1;
    }
    if (k == 'O' || k == 'o') {
        --(*((volatile unsigned char *)monitor_addr));
        return 1;
    }
    if (k == 'P' || k == 'p') {
        ++(*((volatile unsigned char *)monitor_addr));
        return 1;
    }
    if (k == 'G' || k == 'g') {
        goto_mode = 1;
        goto_digits = 0;
        goto_addr = 0;
        edit_pending = 0;
        return 1;
    }
    if (k == 'C' || k == 'c') {
        edit_pending = 0;
        return 1;
    }

    nib = parse_hex_digit(k);
    if (nib < 0) return 0;

    if (!edit_pending) {
        edit_high = (unsigned char)nib;
        edit_pending = 1;
    } else {
        *((volatile unsigned char *)monitor_addr) = (unsigned char)((edit_high << 4) | (unsigned char)nib);
        edit_pending = 0;
        monitor_addr = (unsigned int)(monitor_addr + 1u);
    }
    return 1;
}

void nmi_handler_c(void) {
    unsigned int dst;
    unsigned int i;
    unsigned int length;

    if (nmi_copy_cmd != NMI_CMD_NONE) {
        dst = ((unsigned int)nmi_copy_dst_hi << 8) | (unsigned int)nmi_copy_dst_lo;
        length = (nmi_copy_len == 0u) ? 256u : (unsigned int)nmi_copy_len;
        nmi_copy_err = 0u;

        if (nmi_copy_cmd == NMI_CMD_WRITE_FROM_SCRATCH) {
            for (i = 0u; i < length; ++i) {
                *((volatile unsigned char *)(dst + i)) =
                    *((volatile unsigned char *)(NMI_SCRATCH_ADDR + i));
            }
        } else if (nmi_copy_cmd == NMI_CMD_READ_TO_SCRATCH) {
            for (i = 0u; i < length; ++i) {
                *((volatile unsigned char *)(NMI_SCRATCH_ADDR + i)) =
                    *((volatile unsigned char *)(dst + i));
            }
        } else {
            nmi_copy_err = 1u;
        }

        nmi_copy_cmd = NMI_CMD_NONE;
        ++nmi_copy_seq;
    }

    unsigned char color = (unsigned char)((nmi_border_color + 1u) & 0x07u);
    nmi_border_color = color;
    zx_border(color);
    ++nmi_irq_count;
}

void main(void) {
    unsigned char last_key = 0;
    unsigned char key;
    unsigned char needs_redraw = 1;

    monitor_addr = (unsigned int)MONITOR_START;
    edit_pending = 0;
    edit_high = 0;
    goto_mode = 0;
    goto_digits = 0;
    goto_addr = 0;

    nmi_border_color = BLUE;
    nmi_irq_count = 0;
    nmi_copy_cmd = NMI_CMD_NONE;
    nmi_copy_len = 0u;
    nmi_copy_dst_lo = 0u;
    nmi_copy_dst_hi = 0u;
    nmi_copy_seq = 0u;
    nmi_copy_err = 0u;

    fzx_state_init(&fs, &ff_ao_GenevaMonoRoman, (struct r_Rect16 *)&screen);
    fs.fgnd_attr = 0x38;
    fs.fgnd_mask = 0x00;

    while (1) {
        if (needs_redraw) {
            draw_ui();
            needs_redraw = 0;
        }

        key = (unsigned char)in_Inkey();
        if (key != 0 && last_key == 0) {
            if (monitor_handle_key(key)) {
                needs_redraw = 1;
            }
        }
        last_key = key;
    }
}
