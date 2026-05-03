#include "zx_terminal.h"

#include "debug.h"
#include "ff.h"
#include "zx_bus.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define ZX_BRIDGE_SEQ_ADDR  0x3000u
#define ZX_BRIDGE_LEN_ADDR  0x3001u
#define ZX_BRIDGE_TEXT_ADDR 0x3002u
#define ZX_BRIDGE_MAX_TEXT  40u
#define ZX_VIEW_SEQ_ADDR    0x302Au
#define ZX_VIEW_ADDR_LO     0x302Bu
#define ZX_VIEW_ADDR_HI     0x302Cu
#define ZX_VIEW_LEN_ADDR    0x302Du
#define ZX_VIEW_MAX_BYTES   16u

#define ZX_SCREEN_PIXELS_ADDR 0x4000u
#define ZX_SCREEN_PIXELS_LEN  6144u
#define ZX_SCREEN_ATTRS_ADDR  0x5800u
#define ZX_SCREEN_ATTRS_LEN   768u
#define ZX_TERM_COLS          32u
#define ZX_TERM_ROWS          24u
#define ZX_TERM_NMI_TO        220u
#define ZX_SCREEN_WRITE_CHUNK 64u

#define ZX_BROWSER_MAX_FILES    64u
#define ZX_BROWSER_NAME_MAX     48u
#define ZX_BROWSER_PAGE_ROWS    18u

static uint8_t s_gfx_pixels[ZX_SCREEN_PIXELS_LEN];
static uint8_t s_gfx_attrs[ZX_SCREEN_ATTRS_LEN];
static uint8_t s_prev_pixels[ZX_SCREEN_PIXELS_LEN];
static uint8_t s_prev_attrs[ZX_SCREEN_ATTRS_LEN];
static uint8_t s_prev_valid = 0u;
static uint8_t s_bridge_seq = 0u;
static uint8_t s_view_seq = 0u;

static uint8_t s_term_chars[ZX_TERM_ROWS][ZX_TERM_COLS];
static uint8_t s_term_attrs[ZX_TERM_ROWS][ZX_TERM_COLS];
static uint8_t s_term_row = 0u;
static uint8_t s_term_col = 0u;
static uint8_t s_term_fg = 0u;  /* black */
static uint8_t s_term_bg = 7u;  /* white */
static uint8_t s_term_bright = 0u;
static uint8_t s_term_esc_state = 0u; /* 0=normal,1=ESC,2=CSI */
static uint8_t s_term_csi_param[4];
static uint8_t s_term_csi_count = 0u;
static uint8_t s_term_csi_building = 0u;
static uint8_t s_term_csi_value = 0u;
static char s_browser_files[ZX_BROWSER_MAX_FILES][ZX_BROWSER_NAME_MAX];
static uint8_t s_browser_count = 0u;
static char s_z80select_pending[128];

static const uint8_t s_font4x7_chars[] =
    " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-.:/_";

static const uint8_t s_font4x7[][7] = {
    {0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0x0u}, /* space */
    {0x6u,0x9u,0x9u,0x9u,0x9u,0x9u,0x6u}, /* 0 */
    {0x2u,0x6u,0x2u,0x2u,0x2u,0x2u,0x7u}, /* 1 */
    {0x6u,0x9u,0x1u,0x2u,0x4u,0x8u,0xFu}, /* 2 */
    {0xEu,0x1u,0x1u,0x6u,0x1u,0x1u,0xEu}, /* 3 */
    {0x1u,0x3u,0x5u,0x9u,0xFu,0x1u,0x1u}, /* 4 */
    {0xFu,0x8u,0x8u,0xEu,0x1u,0x1u,0xEu}, /* 5 */
    {0x6u,0x8u,0x8u,0xEu,0x9u,0x9u,0x6u}, /* 6 */
    {0xFu,0x1u,0x2u,0x2u,0x4u,0x4u,0x4u}, /* 7 */
    {0x6u,0x9u,0x9u,0x6u,0x9u,0x9u,0x6u}, /* 8 */
    {0x6u,0x9u,0x9u,0x7u,0x1u,0x1u,0x6u}, /* 9 */
    {0x6u,0x9u,0x9u,0xFu,0x9u,0x9u,0x9u}, /* A */
    {0xEu,0x9u,0x9u,0xEu,0x9u,0x9u,0xEu}, /* B */
    {0x6u,0x9u,0x8u,0x8u,0x8u,0x9u,0x6u}, /* C */
    {0xEu,0x9u,0x9u,0x9u,0x9u,0x9u,0xEu}, /* D */
    {0xFu,0x8u,0x8u,0xEu,0x8u,0x8u,0xFu}, /* E */
    {0xFu,0x8u,0x8u,0xEu,0x8u,0x8u,0x8u}, /* F */
    {0x6u,0x9u,0x8u,0xBu,0x9u,0x9u,0x7u}, /* G */
    {0x9u,0x9u,0x9u,0xFu,0x9u,0x9u,0x9u}, /* H */
    {0x7u,0x2u,0x2u,0x2u,0x2u,0x2u,0x7u}, /* I */
    {0x1u,0x1u,0x1u,0x1u,0x9u,0x9u,0x6u}, /* J */
    {0x9u,0xAu,0xCu,0x8u,0xCu,0xAu,0x9u}, /* K */
    {0x8u,0x8u,0x8u,0x8u,0x8u,0x8u,0xFu}, /* L */
    {0x9u,0xFu,0xFu,0x9u,0x9u,0x9u,0x9u}, /* M */
    {0x9u,0xDu,0xDu,0xBu,0xBu,0x9u,0x9u}, /* N */
    {0x6u,0x9u,0x9u,0x9u,0x9u,0x9u,0x6u}, /* O */
    {0xEu,0x9u,0x9u,0xEu,0x8u,0x8u,0x8u}, /* P */
    {0x6u,0x9u,0x9u,0x9u,0xBu,0xAu,0x5u}, /* Q */
    {0xEu,0x9u,0x9u,0xEu,0xCu,0xAu,0x9u}, /* R */
    {0x7u,0x8u,0x8u,0x6u,0x1u,0x1u,0xEu}, /* S */
    {0xFu,0x2u,0x2u,0x2u,0x2u,0x2u,0x2u}, /* T */
    {0x9u,0x9u,0x9u,0x9u,0x9u,0x9u,0x6u}, /* U */
    {0x9u,0x9u,0x9u,0x9u,0x9u,0x6u,0x6u}, /* V */
    {0x9u,0x9u,0x9u,0x9u,0xFu,0xFu,0x9u}, /* W */
    {0x9u,0x9u,0x6u,0x6u,0x6u,0x9u,0x9u}, /* X */
    {0x9u,0x9u,0x6u,0x2u,0x2u,0x2u,0x2u}, /* Y */
    {0xFu,0x1u,0x2u,0x4u,0x8u,0x8u,0xFu}, /* Z */
    {0x0u,0x0u,0x0u,0xFu,0x0u,0x0u,0x0u}, /* - */
    {0x0u,0x0u,0x0u,0x0u,0x0u,0x6u,0x6u}, /* . */
    {0x0u,0x6u,0x6u,0x0u,0x6u,0x6u,0x0u}, /* : */
    {0x1u,0x1u,0x2u,0x2u,0x4u,0x8u,0x8u}, /* / */
    {0x0u,0x0u,0x0u,0x0u,0x0u,0x0u,0xFu}  /* _ */
};

static uint8_t ZX_Font4x7Row (uint8_t ch, uint8_t row) {
    uint8_t i;

    if (row >= 7u) {
        return 0u;
    }
    if ((ch >= 'a') && (ch <= 'z')) {
        ch = (uint8_t)(ch - ('a' - 'A'));
    }
    for (i = 0u; s_font4x7_chars[i] != '\0'; ++i) {
        if ((uint8_t)s_font4x7_chars[i] == ch) {
            return s_font4x7[i][row];
        }
    }
    return 0u;
}

static uint16_t ZX_PixelAddr (uint8_t y, uint8_t x_byte) {
    uint16_t addr = ZX_SCREEN_PIXELS_ADDR;
    addr = (uint16_t)(addr + (uint16_t)((uint16_t)(y & 0xC0u) << 5));
    addr = (uint16_t)(addr + (uint16_t)((uint16_t)(y & 0x07u) << 8));
    addr = (uint16_t)(addr + (uint16_t)((uint16_t)(y & 0x38u) << 2));
    addr = (uint16_t)(addr + x_byte);
    return addr;
}

static uint8_t ZX_Expand4To8 (uint8_t bits4) {
    uint8_t out = 0u;
    if ((bits4 & 0x8u) != 0u) { out |= 0xC0u; }
    if ((bits4 & 0x4u) != 0u) { out |= 0x30u; }
    if ((bits4 & 0x2u) != 0u) { out |= 0x0Cu; }
    if ((bits4 & 0x1u) != 0u) { out |= 0x03u; }
    return out;
}

static uint8_t ZX_TermCurrentAttr (void) {
    return (uint8_t)(((s_term_bright & 1u) << 6) | ((s_term_bg & 7u) << 3) | (s_term_fg & 7u));
}

static void ZX_TermRenderCellToBuffers (uint8_t col, uint8_t row) {
    uint8_t r;
    uint8_t attr = s_term_attrs[row][col];
    uint8_t ch = s_term_chars[row][col];

    for (r = 0u; r < 8u; ++r) {
        uint8_t pix = (r < 7u) ? ZX_Expand4To8 (ZX_Font4x7Row (ch, r)) : 0u;
        uint16_t addr = (uint16_t)(ZX_PixelAddr ((uint8_t)(row * 8u + r), col) - ZX_SCREEN_PIXELS_ADDR);
        s_gfx_pixels[addr] = pix;
    }

    s_gfx_attrs[(uint16_t)row * ZX_TERM_COLS + col] = attr;
}

static void ZX_TermRenderAllToBuffers (void) {
    uint8_t y;
    uint8_t x;

    memset (s_gfx_pixels, 0, sizeof (s_gfx_pixels));
    for (y = 0u; y < ZX_TERM_ROWS; ++y) {
        for (x = 0u; x < ZX_TERM_COLS; ++x) {
            ZX_TermRenderCellToBuffers (x, y);
        }
    }
}

static int ZX_BusWriteChunked (uint16_t base_addr, const uint8_t *buffer, uint16_t length) {
    uint16_t offset = 0u;

    while (offset < length) {
        uint16_t chunk = (uint16_t)(length - offset);
        if (chunk > ZX_SCREEN_WRITE_CHUNK) {
            chunk = ZX_SCREEN_WRITE_CHUNK;
        }
        /* Keep chunk size modest to avoid long mailbox service latency. */
        if (!ZX_BusWriteBlock ((uint16_t)(base_addr + offset), &buffer[offset], chunk)) {
            return 0;
        }
        offset = (uint16_t)(offset + chunk);
    }

    return 1;
}

static int ZX_BusWriteDiff (uint16_t base_addr,
                            const uint8_t *current,
                            uint8_t *previous,
                            uint16_t length) {
    uint16_t i = 0u;

    while (i < length) {
        uint16_t start;
        uint16_t run;

        while ((i < length) && (current[i] == previous[i])) {
            ++i;
        }
        if (i >= length) {
            break;
        }

        start = i;
        while ((i < length) && (current[i] != previous[i])) {
            ++i;
        }
        run = (uint16_t)(i - start);

        if (!ZX_BusWriteChunked ((uint16_t)(base_addr + start), &current[start], run)) {
            return 0;
        }
        memcpy (&previous[start], &current[start], run);
    }

    return 1;
}

static int ZX_WaitNmiMailboxReady (uint32_t timeout_ms) {
    uint8_t probe;
    uint32_t waited = 0u;

    while (waited < timeout_ms) {
        if (ZX_BusReadBlock (0x0000u, &probe, 1u)) {
            return 1;
        }
        Delay_Ms (20u);
        waited += 20u;
    }

    return 0;
}

static int ZX_TermFlushBuffers (void) {
    if (!s_prev_valid) {
        if (!ZX_BusWriteChunked (ZX_SCREEN_PIXELS_ADDR,
                                 s_gfx_pixels,
                                 ZX_SCREEN_PIXELS_LEN)) {
            return 0;
        }
        if (!ZX_BusWriteChunked (ZX_SCREEN_ATTRS_ADDR,
                                 s_gfx_attrs,
                                 ZX_SCREEN_ATTRS_LEN)) {
            return 0;
        }
        memcpy (s_prev_pixels, s_gfx_pixels, sizeof (s_prev_pixels));
        memcpy (s_prev_attrs, s_gfx_attrs, sizeof (s_prev_attrs));
        s_prev_valid = 1u;
        return 1;
    }

    if (!ZX_BusWriteDiff (ZX_SCREEN_PIXELS_ADDR,
                          s_gfx_pixels,
                          s_prev_pixels,
                          ZX_SCREEN_PIXELS_LEN)) {
        return 0;
    }
    if (!ZX_BusWriteDiff (ZX_SCREEN_ATTRS_ADDR,
                          s_gfx_attrs,
                          s_prev_attrs,
                          ZX_SCREEN_ATTRS_LEN)) {
        return 0;
    }
    return 1;
}

static int ZX_TermCommit (void) {
    ZX_TermRenderAllToBuffers();
    if (!ZX_TermFlushBuffers()) {
        return 0;
    }
    return 1;
}

static uint8_t ZX_TermSanitizeChar (uint8_t ch) {
    if ((ch >= 'a') && (ch <= 'z')) {
        ch = (uint8_t)(ch - ('a' - 'A'));
    }
    if ((ch < 0x20u) || (ch > 0x7Eu)) {
        ch = ' ';
    }
    if (!strchr ((const char *)s_font4x7_chars, (int)ch)) {
        ch = ' ';
    }
    return ch;
}

static void ZX_TermModelWriteAt (uint8_t row, uint8_t col, const char *text, uint8_t attr) {
    while ((row < ZX_TERM_ROWS) && (col < ZX_TERM_COLS) && (text != NULL) && (*text != '\0')) {
        s_term_chars[row][col] = ZX_TermSanitizeChar ((uint8_t)*text++);
        s_term_attrs[row][col] = attr;
        ++col;
    }
}

static void ZX_TermModelFillRow (uint8_t row, uint8_t attr) {
    uint8_t col;
    if (row >= ZX_TERM_ROWS) {
        return;
    }
    for (col = 0u; col < ZX_TERM_COLS; ++col) {
        s_term_chars[row][col] = ' ';
        s_term_attrs[row][col] = attr;
    }
}

static void ZX_TermModelClear (void) {
    uint8_t y;
    uint8_t x;
    uint8_t attr = ZX_TermCurrentAttr();

    for (y = 0u; y < ZX_TERM_ROWS; ++y) {
        for (x = 0u; x < ZX_TERM_COLS; ++x) {
            s_term_chars[y][x] = ' ';
            s_term_attrs[y][x] = attr;
        }
    }
}

static int ZX_TermClear (void) {
    s_term_row = 0u;
    s_term_col = 0u;
    ZX_TermModelClear();
    return 1;
}

static int ZX_TermScrollUp (void) {
    uint8_t x;
    uint8_t attr = ZX_TermCurrentAttr();

    memmove (&s_term_chars[0][0], &s_term_chars[1][0], (ZX_TERM_ROWS - 1u) * ZX_TERM_COLS);
    memmove (&s_term_attrs[0][0], &s_term_attrs[1][0], (ZX_TERM_ROWS - 1u) * ZX_TERM_COLS);

    for (x = 0u; x < ZX_TERM_COLS; ++x) {
        s_term_chars[ZX_TERM_ROWS - 1u][x] = ' ';
        s_term_attrs[ZX_TERM_ROWS - 1u][x] = attr;
    }

    return 1;
}

static int ZX_TermPutVisible (uint8_t ch) {
    s_term_chars[s_term_row][s_term_col] = ch;
    s_term_attrs[s_term_row][s_term_col] = ZX_TermCurrentAttr();

    ++s_term_col;
    if (s_term_col >= ZX_TERM_COLS) {
        s_term_col = 0u;
        ++s_term_row;
        if (s_term_row >= ZX_TERM_ROWS) {
            s_term_row = (uint8_t)(ZX_TERM_ROWS - 1u);
            if (!ZX_TermScrollUp()) {
                return 0;
            }
        }
    }
    return 1;
}

static int ZX_TermHandleBasic (uint8_t ch) {
    if (ch == '\r') {
        s_term_col = 0u;
        return 1;
    }
    if (ch == '\n') {
        s_term_col = 0u;
        ++s_term_row;
        if (s_term_row >= ZX_TERM_ROWS) {
            s_term_row = (uint8_t)(ZX_TERM_ROWS - 1u);
            return ZX_TermScrollUp();
        }
        return 1;
    }
    if (ch == '\b') {
        if (s_term_col > 0u) {
            --s_term_col;
        }
        s_term_chars[s_term_row][s_term_col] = ' ';
        s_term_attrs[s_term_row][s_term_col] = ZX_TermCurrentAttr();
        return 1;
    }
    if (ch == '\t') {
        uint8_t next_tab = (uint8_t)((s_term_col + 4u) & (uint8_t)~3u);
        while (s_term_col < next_tab) {
            if (!ZX_TermPutVisible (' ')) {
                return 0;
            }
        }
        return 1;
    }

    if (ch < 0x20u) {
        return 1;
    }

    if (ch > 0x7Eu) {
        ch = '?';
    }

    if ((ch >= 'a') && (ch <= 'z')) {
        ch = (uint8_t)(ch - ('a' - 'A'));
    }

    if (!strchr ((const char *)s_font4x7_chars, (int)ch)) {
        ch = ' ';
    }

    return ZX_TermPutVisible (ch);
}

static uint8_t ZX_TermCsiParam (uint8_t index, uint8_t default_value) {
    if (index >= s_term_csi_count) {
        return default_value;
    }
    return (s_term_csi_param[index] == 0u) ? default_value : s_term_csi_param[index];
}

static void ZX_TermApplySgrOne (uint8_t p) {
    if (p == 0u) {
        s_term_fg = 0u;
        s_term_bg = 7u;
        s_term_bright = 0u;
        return;
    }
    if (p == 1u) {
        s_term_bright = 1u;
        return;
    }
    if ((p >= 30u) && (p <= 37u)) {
        s_term_fg = (uint8_t)(p - 30u);
        return;
    }
    if ((p >= 40u) && (p <= 47u)) {
        s_term_bg = (uint8_t)(p - 40u);
        return;
    }
}

static int ZX_TermHandleCsiFinal (uint8_t final_ch) {
    uint8_t n;
    uint8_t y;
    uint8_t x;

    if (final_ch == 'H' || final_ch == 'f') {
        uint8_t row = ZX_TermCsiParam (0u, 1u);
        uint8_t col = ZX_TermCsiParam (1u, 1u);
        if (row > ZX_TERM_ROWS) { row = ZX_TERM_ROWS; }
        if (col > ZX_TERM_COLS) { col = ZX_TERM_COLS; }
        s_term_row = (uint8_t)(row - 1u);
        s_term_col = (uint8_t)(col - 1u);
        return 1;
    }

    if (final_ch == 'A') {
        n = ZX_TermCsiParam (0u, 1u);
        s_term_row = (s_term_row > n) ? (uint8_t)(s_term_row - n) : 0u;
        return 1;
    }
    if (final_ch == 'B') {
        n = ZX_TermCsiParam (0u, 1u);
        s_term_row = (uint8_t)((s_term_row + n < ZX_TERM_ROWS) ? (s_term_row + n) : (ZX_TERM_ROWS - 1u));
        return 1;
    }
    if (final_ch == 'C') {
        n = ZX_TermCsiParam (0u, 1u);
        s_term_col = (uint8_t)((s_term_col + n < ZX_TERM_COLS) ? (s_term_col + n) : (ZX_TERM_COLS - 1u));
        return 1;
    }
    if (final_ch == 'D') {
        n = ZX_TermCsiParam (0u, 1u);
        s_term_col = (s_term_col > n) ? (uint8_t)(s_term_col - n) : 0u;
        return 1;
    }

    if (final_ch == 'J') {
        return ZX_TermClear();
    }

    if (final_ch == 'K') {
        for (x = s_term_col; x < ZX_TERM_COLS; ++x) {
            s_term_chars[s_term_row][x] = ' ';
            s_term_attrs[s_term_row][x] = ZX_TermCurrentAttr();
        }
        return 1;
    }

    if (final_ch == 'm') {
        if (s_term_csi_count == 0u) {
            ZX_TermApplySgrOne (0u);
            return 1;
        }
        for (y = 0u; y < s_term_csi_count; ++y) {
            ZX_TermApplySgrOne (s_term_csi_param[y]);
        }
        return 1;
    }

    return 1;
}

static void ZX_TermCsiReset (void) {
    s_term_csi_count = 0u;
    s_term_csi_building = 0u;
    s_term_csi_value = 0u;
}

static int ZX_TermWriteByte (uint8_t ch) {
    if (s_term_esc_state == 0u) {
        if (ch == 0x1Bu) {
            s_term_esc_state = 1u;
            return 1;
        }
        return ZX_TermHandleBasic (ch);
    }

    if (s_term_esc_state == 1u) {
        if (ch == '[') {
            s_term_esc_state = 2u;
            ZX_TermCsiReset();
            return 1;
        }
        s_term_esc_state = 0u;
        return 1;
    }

    if ((ch >= '0') && (ch <= '9')) {
        s_term_csi_value = (uint8_t)(s_term_csi_value * 10u + (ch - '0'));
        s_term_csi_building = 1u;
        return 1;
    }
    if (ch == ';') {
        if (s_term_csi_count < 4u) {
            s_term_csi_param[s_term_csi_count++] = s_term_csi_building ? s_term_csi_value : 0u;
        }
        s_term_csi_value = 0u;
        s_term_csi_building = 0u;
        return 1;
    }

    if (s_term_csi_count < 4u) {
        s_term_csi_param[s_term_csi_count++] = s_term_csi_building ? s_term_csi_value : 0u;
    }
    s_term_esc_state = 0u;
    return ZX_TermHandleCsiFinal (ch);
}

static int ZX_TermWriteBuffer (const uint8_t *buf, uint16_t len) {
    uint16_t i;
    if ((buf == NULL) && (len != 0u)) {
        return 0;
    }
    for (i = 0u; i < len; ++i) {
        if (!ZX_TermWriteByte (buf[i])) {
            return 0;
        }
    }
    return 1;
}

static int ZX_ParseEscapedText (const char *in, uint8_t *out, uint16_t out_max, uint16_t *out_len) {
    uint16_t w = 0u;

    while ((in != NULL) && (*in != '\0')) {
        uint8_t ch = (uint8_t)*in++;
        if ((ch == '\\') && (*in != '\0')) {
            char n = *in++;
            if (n == 'n') { ch = '\n'; }
            else if (n == 'r') { ch = '\r'; }
            else if (n == 't') { ch = '\t'; }
            else if (n == 'e') { ch = 0x1Bu; }
            else if (n == '\\') { ch = '\\'; }
            else { ch = (uint8_t)n; }
        }

        if (w >= out_max) {
            return 0;
        }
        out[w++] = ch;
    }

    *out_len = w;
    return 1;
}

static int ZX_HasZ80Extension (const char *name) {
    size_t len;

    if (name == NULL) {
        return 0;
    }
    len = strlen (name);
    if (len < 4u) {
        return 0;
    }
    name += (len - 4u);
    return (tolower ((unsigned char)name[0]) == '.') &&
           (tolower ((unsigned char)name[1]) == 'z') &&
           (tolower ((unsigned char)name[2]) == '8') &&
           (tolower ((unsigned char)name[3]) == '0');
}

static int ZX_BrowserLoadFiles (const char *path) {
    DIR dir;
    FILINFO fno;
    FRESULT fr;
    const char *target = ((path != NULL) && (path[0] != '\0')) ? path : "/";

    s_browser_count = 0u;
    fr = f_opendir (&dir, target);
    if (fr != FR_OK) {
        printf("z80select: cannot open '%s' (fr=%d)\r\n", target, (int)fr);
        return 0;
    }

    while (1) {
        fr = f_readdir (&dir, &fno);
        if (fr != FR_OK) {
            printf("z80select: readdir failed (fr=%d)\r\n", (int)fr);
            f_closedir (&dir);
            return 0;
        }
        if (fno.fname[0] == '\0') {
            break;
        }
        if ((fno.fattrib & AM_DIR) != 0u) {
            continue;
        }
        if (!ZX_HasZ80Extension ((const char *)fno.fname)) {
            continue;
        }
        if (s_browser_count >= ZX_BROWSER_MAX_FILES) {
            break;
        }
        strncpy (s_browser_files[s_browser_count],
                 (const char *)fno.fname,
                 (size_t)(ZX_BROWSER_NAME_MAX - 1u));
        s_browser_files[s_browser_count][ZX_BROWSER_NAME_MAX - 1u] = '\0';
        ++s_browser_count;
    }

    f_closedir (&dir);
    return 1;
}

static void ZX_BrowserBuildPath (char *out, size_t out_size, const char *dir, const char *name) {
    const char *base = ((dir != NULL) && (dir[0] != '\0')) ? dir : "/";
    size_t len = strlen (base);

    if ((len > 0u) && (base[len - 1u] == '/')) {
        (void)snprintf (out, out_size, "%s%s", base, name);
    } else if ((len == 1u) && (base[0] == '/')) {
        (void)snprintf (out, out_size, "/%s", name);
    } else {
        (void)snprintf (out, out_size, "%s/%s", base, name);
    }
}

static int ZX_BrowserRender (const char *path, uint8_t selected) {
    uint8_t page_start;
    uint8_t row;
    uint8_t i;
    uint8_t normal_attr = (uint8_t)((7u << 3) | 0u);
    uint8_t select_attr = (uint8_t)((0u << 3) | 7u);
    char line[ZX_TERM_COLS + 1u];

    ZX_TermModelClear();
    ZX_TermModelWriteAt (0u, 0u, "Z80 FILE SELECTOR", normal_attr);
    ZX_TermModelWriteAt (1u, 0u, ((path != NULL) && (path[0] != '\0')) ? path : "/", normal_attr);
    ZX_TermModelWriteAt (2u, 0u, "Q/A MOVE  O/P PAGE", normal_attr);
    ZX_TermModelWriteAt (3u, 0u, "ENTER RUN  SPACE EXIT", normal_attr);

    if (s_browser_count == 0u) {
        ZX_TermModelWriteAt (6u, 0u, "NO Z80 FILES FOUND", normal_attr);
        return ZX_TermCommit();
    }

    page_start = (uint8_t)((selected / ZX_BROWSER_PAGE_ROWS) * ZX_BROWSER_PAGE_ROWS);
    for (row = 0u; row < ZX_BROWSER_PAGE_ROWS; ++row) {
        uint8_t index = (uint8_t)(page_start + row);
        uint8_t attr = (index == selected) ? select_attr : normal_attr;
        ZX_TermModelFillRow ((uint8_t)(5u + row), attr);
        if (index >= s_browser_count) {
            continue;
        }

        memset (line, ' ', sizeof (line));
        line[ZX_TERM_COLS] = '\0';
        line[0] = (index == selected) ? '>' : ' ';
        for (i = 0u; (i < (uint8_t)(ZX_TERM_COLS - 2u)) && (s_browser_files[index][i] != '\0'); ++i) {
            line[1u + i] = s_browser_files[index][i];
        }
        ZX_TermModelWriteAt ((uint8_t)(5u + row), 0u, line, attr);
    }

    memset (line, ' ', sizeof (line));
    line[ZX_TERM_COLS] = '\0';
    (void)snprintf (line, sizeof (line), "%u/%u FILES",
                    (unsigned)(selected + 1u),
                    (unsigned)s_browser_count);
    ZX_TermModelWriteAt (23u, 0u, line, normal_attr);
    return ZX_TermCommit();
}

void ZX_TerminalInit (void) {
    s_bridge_seq = 0u;
    s_view_seq = 0u;
    s_term_row = 0u;
    s_term_col = 0u;
    s_term_fg = 0u;
    s_term_bg = 7u;
    s_term_bright = 0u;
    s_term_esc_state = 0u;
    s_prev_valid = 0u;
    ZX_TermCsiReset();
    ZX_TermModelClear();
    s_z80select_pending[0] = '\0';
}

void ZX_TerminalMarkBridgeDirty (void) {
    ++s_bridge_seq;
    (void)ZX_CartRamWriteBlock (ZX_BRIDGE_SEQ_ADDR, &s_bridge_seq, 1u);
}

void ZX_TerminalCommandViewOff (void) {
    uint8_t value = 0u;

    if (!ZX_CartRamWriteBlock (ZX_VIEW_LEN_ADDR, &value, 1u)) {
        printf("ERR: zxview disable failed\r\n");
        return;
    }

    ++s_view_seq;
    if (!ZX_CartRamWriteBlock (ZX_VIEW_SEQ_ADDR, &s_view_seq, 1u)) {
        printf("ERR: zxview seq write failed\r\n");
        return;
    }

    ZX_TriggerNMI();
    printf("ZX RAM viewer disabled\r\n");
}

void ZX_TerminalCommandView (uint16_t address, uint8_t length) {
    uint8_t config[3];

    config[0] = (uint8_t)(address & 0x00FFu);
    config[1] = (uint8_t)(address >> 8);
    config[2] = length;

    if (!ZX_CartRamWriteBlock (ZX_VIEW_ADDR_LO, config, 3u)) {
        printf("ERR: zxview config write failed\r\n");
        return;
    }

    ++s_view_seq;
    if (!ZX_CartRamWriteBlock (ZX_VIEW_SEQ_ADDR, &s_view_seq, 1u)) {
        printf("ERR: zxview seq write failed\r\n");
        return;
    }

    ZX_TriggerNMI();
    printf("ZX RAM viewer set to 0x%04X (%u byte%s)\r\n",
           (unsigned)address,
           (unsigned)length,
           (length == 1u) ? "" : "s");
}

void ZX_TerminalCommandBridgeText (char *firstToken) {
    uint8_t text[ZX_BRIDGE_MAX_TEXT];
    uint16_t len = 0u;
    char *tok = firstToken;

    if (tok == NULL) {
        printf("Usage: zxmsg <text>\r\n");
        return;
    }

    while ((tok != NULL) && (len < ZX_BRIDGE_MAX_TEXT)) {
        while ((*tok != '\0') && (len < ZX_BRIDGE_MAX_TEXT)) {
            text[len++] = (uint8_t)*tok++;
        }

        tok = strtok (NULL, " \t");
        if ((tok != NULL) && (len < ZX_BRIDGE_MAX_TEXT)) {
            text[len++] = (uint8_t)' ';
        }
    }

    if (!ZX_CartRamWriteBlock (ZX_BRIDGE_TEXT_ADDR, text, len)) {
        printf("ERR: bridge text write failed\r\n");
        return;
    }

    {
        uint8_t l = (uint8_t)len;
        if (!ZX_CartRamWriteBlock (ZX_BRIDGE_LEN_ADDR, &l, 1u)) {
            printf("ERR: bridge len write failed\r\n");
            return;
        }
    }

    ZX_TerminalMarkBridgeDirty();
    ZX_TriggerNMI();
    printf("ZX message sent (%u chars)\r\n", (unsigned)len);
}

void ZX_TerminalCommandTermInit (void) {
    s_term_fg = 0u;
    s_term_bg = 7u;
    s_term_bright = 0u;
    s_term_esc_state = 0u;
    ZX_TermCsiReset();

    if (!ZX_TermClear() || !ZX_TermCommit()) {
        printf("ERR: terminal init draw failed\r\n");
        return;
    }
    printf("ZX terminal ready (MPU-rendered 32x24, white bg/black text)\r\n");
}

void ZX_TerminalCommandTermWrite (char *firstToken) {
    uint8_t buf[192];
    uint16_t len = 0u;
    char joined[192];
    uint16_t used = 0u;
    char *tok = firstToken;

    if (tok == NULL) {
        printf("Usage: zxtty <text with \\n \\r \\t \\e escapes>\r\n");
        return;
    }

    joined[0] = '\0';
    while ((tok != NULL) && (used < (uint16_t)(sizeof (joined) - 2u))) {
        uint16_t l = (uint16_t)strlen (tok);
        if (used != 0u) {
            joined[used++] = ' ';
            joined[used] = '\0';
        }
        if (used + l >= (uint16_t)sizeof (joined)) {
            l = (uint16_t)(sizeof (joined) - used - 1u);
        }
        memcpy (&joined[used], tok, l);
        used = (uint16_t)(used + l);
        joined[used] = '\0';
        tok = strtok (NULL, " \t");
    }

    if (!ZX_ParseEscapedText (joined, buf, (uint16_t)sizeof (buf), &len)) {
        printf("ERR: zxtty input too long\r\n");
        return;
    }

    if (!ZX_TermWriteBuffer (buf, len)) {
        printf("ERR: zxtty write failed\r\n");
        return;
    }
    if (!ZX_TermCommit()) {
        printf("ERR: zxtty flush failed\r\n");
        return;
    }
}

void ZX_TerminalCommandZ80Select (const char *path) {
    uint8_t selected = 0u;
    char full_path[128];
    int draw_suspended = 0;
    uint8_t render_attempt;

    s_z80select_pending[0] = '\0';

    if (!ZX_WaitNmiMailboxReady (3000u)) {
        printf("ERR: z80select mailbox not ready\r\n");
        goto done;
    }

    ZX_CartDrawSuspend();
    Delay_Ms (50u);
    draw_suspended = 1;

    if (!ZX_BrowserLoadFiles (path)) {
        goto done;
    }
    if (!ZX_BrowserRender (path, selected)) {
        /* Startup can race zxprog mailbox readiness right after reset.
           Keep retrying briefly before giving up. */
        for (render_attempt = 0u; render_attempt < 20u; ++render_attempt) {
            Delay_Ms (80u);
            if (ZX_BrowserRender (path, selected)) {
                break;
            }
        }
        if (render_attempt == 20u) {
            printf("ERR: z80select draw failed\r\n");
            goto done;
        }
    }
    if (s_browser_count == 0u) {
        printf("z80select: no .z80 files\r\n");
        goto done;
    }

    printf("z80select: Q/A move, O/P page, ENTER run, SPACE exit\r\n");

    for (;;) {
        uint8_t key = 0u;
        int rc = ZX_KeyPoll (&key);

        if (rc < 0) {
            printf("ERR: key mailbox read failed\r\n");
            goto done;
        }
        if (rc == 0) {
            Delay_Ms (20u);
            continue;
        }

        if ((key == 'q') || (key == 'Q')) {
            if (selected > 0u) {
                --selected;
                if (!ZX_BrowserRender (path, selected)) {
                    printf("ERR: z80select redraw failed\r\n");
                    goto done;
                }
            }
            continue;
        }
        if ((key == 'a') || (key == 'A')) {
            if ((uint16_t)selected + 1u < s_browser_count) {
                ++selected;
                if (!ZX_BrowserRender (path, selected)) {
                    printf("ERR: z80select redraw failed\r\n");
                    goto done;
                }
            }
            continue;
        }
        if ((key == 'o') || (key == 'O')) {
            if (selected >= ZX_BROWSER_PAGE_ROWS) {
                selected = (uint8_t)(selected - ZX_BROWSER_PAGE_ROWS);
            } else {
                selected = 0u;
            }
            if (!ZX_BrowserRender (path, selected)) {
                printf("ERR: z80select redraw failed\r\n");
                goto done;
            }
            continue;
        }
        if ((key == 'p') || (key == 'P')) {
            uint16_t next = (uint16_t)selected + ZX_BROWSER_PAGE_ROWS;
            if (next >= s_browser_count) {
                next = (s_browser_count == 0u) ? 0u : (uint16_t)(s_browser_count - 1u);
            }
            selected = (uint8_t)next;
            if (!ZX_BrowserRender (path, selected)) {
                printf("ERR: z80select redraw failed\r\n");
                goto done;
            }
            continue;
        }
        if (key == ' ') {
            printf("z80select: cancelled\r\n");
            goto done;
        }
        if (key == '\n') {
            ZX_BrowserBuildPath (full_path, sizeof (full_path), path, s_browser_files[selected]);
            printf("z80select: selected %s\r\n", full_path);
            strncpy (s_z80select_pending, full_path, sizeof (s_z80select_pending) - 1u);
            s_z80select_pending[sizeof (s_z80select_pending) - 1u] = '\0';
            goto done;
        }
    }

done:
    if (draw_suspended) {
        ZX_TerminalMarkBridgeDirty();
        ZX_CartDrawResume();
    }
}

const char *ZX_TerminalPendingZ80Selection (void) {
    return s_z80select_pending;
}

void ZX_TerminalClearPendingZ80Selection (void) {
    s_z80select_pending[0] = '\0';
}


