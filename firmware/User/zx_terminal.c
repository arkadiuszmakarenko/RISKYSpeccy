#include "zx_terminal.h"

#include "debug.h"
#include "version.h"
#include "ff.h"
#include "tape_player.h"
#include "z80_loader.h"
#include "zx_bus.h"
#include "usb_disk.h"
#include "if2_cart.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define ZX_BRIDGE_SEQ_ADDR 0x3000u
#define ZX_BRIDGE_LEN_ADDR 0x3001u
#define ZX_BRIDGE_TEXT_ADDR 0x3002u
#define ZX_BRIDGE_MAX_TEXT 40u
#define ZX_VIEW_SEQ_ADDR 0x302Au
#define ZX_VIEW_ADDR_LO 0x302Bu
#define ZX_VIEW_ADDR_HI 0x302Cu
#define ZX_VIEW_LEN_ADDR 0x302Du
#define ZX_VIEW_MAX_BYTES 16u

#define ZX_SCREEN_PIXELS_ADDR 0x4000u
#define ZX_SCREEN_PIXELS_LEN 6144u
#define ZX_SCREEN_ATTRS_ADDR 0x5800u
#define ZX_SCREEN_ATTRS_LEN 768u
#define ZX_TERM_COLS 32u
#define ZX_TERM_ROWS 24u
#define ZX_TERM_NMI_TO 220u
#define ZX_SCREEN_WRITE_CHUNK 512u
#define ZX_SCREEN_DIFF_MERGE_GAP 16u

#define ZX_BROWSER_NAME_MAX (FF_MAX_LFN + 1u)
#define ZX_BROWSER_PATH_MAX 320u
#define ZX_BROWSER_PAGE_ROWS 18u
#define ZX_BROWSER_JUMP_STEP 90u
#define ZX_BROWSER_SCAN_RETRIES 8u
#define ZX_BROWSER_RENDER_RETRIES 20u

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
static uint8_t s_term_fg = 0u;        /* black */
static uint8_t s_term_bg = 7u;        /* white */
static uint8_t s_term_bright = 0u;
static uint8_t s_term_esc_state = 0u; /* 0=normal,1=ESC,2=CSI */
static uint8_t s_term_csi_param[4];
static uint8_t s_term_csi_count = 0u;
static uint8_t s_term_csi_building = 0u;
static uint8_t s_term_csi_value = 0u;
static uint8_t s_selector_font_mode = 0u;
static uint8_t s_browser_mode = 0u;

/* Streaming directory browser: we never store the full directory listing.
   Instead, an open DIR handle and a small ring buffer of the most-recently
   displayed names let us render any page on demand by f_readdir()'ing forward
   from the start of the directory.  This makes the on-screen file count
   effectively unlimited and keeps RAM usage flat (~5 KB regardless of
   directory size) at the cost of one full re-scan on each page jump.
   The page cache stores one slot per visible row, indexed by the
   *directory-entry* index it corresponds to. */
static DIR     s_browser_dir;
static uint8_t s_browser_dir_open = 0u;
static char    s_browser_dir_path[ZX_BROWSER_PATH_MAX];

/* Per-page name cache.  s_browser_cache_idx[i] is the directory-entry index
   of the name stored in s_browser_cache_name[i].  A "miss" is resolved by
   re-opening the directory and f_readdir()'ing to that index. */
static uint16_t s_browser_cache_idx[ZX_BROWSER_PAGE_ROWS];
static char     s_browser_cache_name[ZX_BROWSER_PAGE_ROWS][ZX_BROWSER_NAME_MAX];
static uint8_t  s_browser_cache_is_dir[ZX_BROWSER_PAGE_ROWS];
static uint8_t  s_browser_cache_valid[ZX_BROWSER_PAGE_ROWS];

static uint16_t s_browser_count = 0u;
static char s_z80select_pending[ZX_BROWSER_PATH_MAX];
static char s_tapselect_pending[ZX_BROWSER_PATH_MAX];
static char s_browser_path_stack[3][ZX_BROWSER_PATH_MAX];
static uint16_t s_browser_selected_stack[3];
static uint8_t s_browser_stack_depth = 0u;

/* Set to 1 by ZX_TerminalCommand*Select when the USB host stack
 * reports the drive has gone away during a blocking keyboard wait.
 * The main loop checks this after the browser returns and re-runs
 * ZX_TerminalWaitUsbDriveReady if it is set. */
static volatile uint8_t s_usb_lost = 0u;

#define ZX_BROWSER_MODE_Z80 0u
#define ZX_BROWSER_MODE_TAP 1u

static const uint8_t s_font4x7_chars[] =
    " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ\"-.:/_";

static const uint8_t s_font4x7[][7] = {
    {0x0u, 0x0u, 0x0u, 0x0u, 0x0u, 0x0u, 0x0u}, /* space */
    {0x6u, 0x9u, 0x9u, 0x9u, 0x9u, 0x9u, 0x6u}, /* 0 */
    {0x2u, 0x6u, 0x2u, 0x2u, 0x2u, 0x2u, 0x7u}, /* 1 */
    {0x6u, 0x9u, 0x1u, 0x2u, 0x4u, 0x8u, 0xFu}, /* 2 */
    {0xEu, 0x1u, 0x1u, 0x6u, 0x1u, 0x1u, 0xEu}, /* 3 */
    {0x1u, 0x3u, 0x5u, 0x9u, 0xFu, 0x1u, 0x1u}, /* 4 */
    {0xFu, 0x8u, 0x8u, 0xEu, 0x1u, 0x1u, 0xEu}, /* 5 */
    {0x6u, 0x8u, 0x8u, 0xEu, 0x9u, 0x9u, 0x6u}, /* 6 */
    {0xFu, 0x1u, 0x2u, 0x2u, 0x4u, 0x4u, 0x4u}, /* 7 */
    {0x6u, 0x9u, 0x9u, 0x6u, 0x9u, 0x9u, 0x6u}, /* 8 */
    {0x6u, 0x9u, 0x9u, 0x7u, 0x1u, 0x1u, 0x6u}, /* 9 */
    {0x6u, 0x9u, 0x9u, 0xFu, 0x9u, 0x9u, 0x9u}, /* A */
    {0xEu, 0x9u, 0x9u, 0xEu, 0x9u, 0x9u, 0xEu}, /* B */
    {0x6u, 0x9u, 0x8u, 0x8u, 0x8u, 0x9u, 0x6u}, /* C */
    {0xEu, 0x9u, 0x9u, 0x9u, 0x9u, 0x9u, 0xEu}, /* D */
    {0xFu, 0x8u, 0x8u, 0xEu, 0x8u, 0x8u, 0xFu}, /* E */
    {0xFu, 0x8u, 0x8u, 0xEu, 0x8u, 0x8u, 0x8u}, /* F */
    {0x6u, 0x9u, 0x8u, 0xBu, 0x9u, 0x9u, 0x7u}, /* G */
    {0x9u, 0x9u, 0x9u, 0xFu, 0x9u, 0x9u, 0x9u}, /* H */
    {0x7u, 0x2u, 0x2u, 0x2u, 0x2u, 0x2u, 0x7u}, /* I */
    {0x1u, 0x1u, 0x1u, 0x1u, 0x9u, 0x9u, 0x6u}, /* J */
    {0x9u, 0xAu, 0xCu, 0x8u, 0xCu, 0xAu, 0x9u}, /* K */
    {0x8u, 0x8u, 0x8u, 0x8u, 0x8u, 0x8u, 0xFu}, /* L */
    {0x9u, 0xFu, 0xFu, 0x9u, 0x9u, 0x9u, 0x9u}, /* M */
    {0x9u, 0xDu, 0xDu, 0xBu, 0xBu, 0x9u, 0x9u}, /* N */
    {0x6u, 0x9u, 0x9u, 0x9u, 0x9u, 0x9u, 0x6u}, /* O */
    {0xEu, 0x9u, 0x9u, 0xEu, 0x8u, 0x8u, 0x8u}, /* P */
    {0x6u, 0x9u, 0x9u, 0x9u, 0xBu, 0xAu, 0x5u}, /* Q */
    {0xEu, 0x9u, 0x9u, 0xEu, 0xCu, 0xAu, 0x9u}, /* R */
    {0x7u, 0x8u, 0x8u, 0x6u, 0x1u, 0x1u, 0xEu}, /* S */
    {0xFu, 0x2u, 0x2u, 0x2u, 0x2u, 0x2u, 0x2u}, /* T */
    {0x9u, 0x9u, 0x9u, 0x9u, 0x9u, 0x9u, 0x6u}, /* U */
    {0x9u, 0x9u, 0x9u, 0x9u, 0x9u, 0x6u, 0x6u}, /* V */
    {0x9u, 0x9u, 0x9u, 0x9u, 0xFu, 0xFu, 0x9u}, /* W */
    {0x9u, 0x9u, 0x6u, 0x6u, 0x6u, 0x9u, 0x9u}, /* X */
    {0x9u, 0x9u, 0x6u, 0x2u, 0x2u, 0x2u, 0x2u}, /* Y */
    {0xFu, 0x1u, 0x2u, 0x4u, 0x8u, 0x8u, 0xFu}, /* Z */
    {0x5u, 0x5u, 0x0u, 0x0u, 0x0u, 0x0u, 0x0u}, /* " */
    {0x0u, 0x0u, 0x0u, 0xFu, 0x0u, 0x0u, 0x0u}, /* - */
    {0x0u, 0x0u, 0x0u, 0x0u, 0x0u, 0x6u, 0x6u}, /* . */
    {0x0u, 0x6u, 0x6u, 0x0u, 0x6u, 0x6u, 0x0u}, /* : */
    {0x1u, 0x1u, 0x2u, 0x2u, 0x4u, 0x8u, 0x8u}, /* / */
    {0x0u, 0x0u, 0x0u, 0x0u, 0x0u, 0x0u, 0xFu}  /* _ */
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

    if ((bits4 & 0x8u) != 0u) {
        out |= 0xC0u;
    }
    if ((bits4 & 0x4u) != 0u) {
        out |= 0x30u;
    }
    if ((bits4 & 0x2u) != 0u) {
        out |= 0x0Cu;
    }
    if ((bits4 & 0x1u) != 0u) {
        out |= 0x03u;
    }
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
        if (s_selector_font_mode != 0u) {
            /* Keep selector text bold but add a 1-pixel gutter between
               adjacent cells so letters do not visually merge. */
            pix &= 0xFEu;
        }
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
        uint16_t end;
        uint16_t gap;

        /* skip leading unchanged bytes */
        while ((i < length) && (current[i] == previous[i])) {
            ++i;
        }
        if (i >= length) {
            break;
        }

        start = i;
        end   = i;

        /* extend the run forward, merging across small unchanged gaps */
        while (end < length) {
            if (current[end] != previous[end]) {
                ++end;
            } else {
                /* measure the gap of unchanged bytes */
                gap = 0u;
                while (((end + gap) < length) &&
                       (current[end + gap] == previous[end + gap]) &&
                       (gap < ZX_SCREEN_DIFF_MERGE_GAP)) {
                    ++gap;
                }
                if (gap >= ZX_SCREEN_DIFF_MERGE_GAP) {
                    break; /* gap too wide — stop the run here */
                }
                end = (uint16_t)(end + gap);
            }
        }

        if (!ZX_BusWriteChunked ((uint16_t)(base_addr + start), &current[start], (uint16_t)(end - start))) {
            return 0;
        }
        memcpy (&previous[start], &current[start], (size_t)(end - start));
        i = end;
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
        if (row > ZX_TERM_ROWS) {
            row = ZX_TERM_ROWS;
        }
        if (col > ZX_TERM_COLS) {
            col = ZX_TERM_COLS;
        }
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
            if (n == 'n') {
                ch = '\n';
            } else if (n == 'r') {
                ch = '\r';
            } else if (n == 't') {
                ch = '\t';
            } else if (n == 'e') {
                ch = 0x1Bu;
            } else if (n == '\\') {
                ch = '\\';
            } else {
                ch = (uint8_t)n;
            }
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

static int ZX_HasTapExtension (const char *name) {
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
           (tolower ((unsigned char)name[1]) == 't') &&
           (tolower ((unsigned char)name[2]) == 'a') &&
           (tolower ((unsigned char)name[3]) == 'p');
}

static int ZX_HasTzxExtension (const char *name) {
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
           (tolower ((unsigned char)name[1]) == 't') &&
           (tolower ((unsigned char)name[2]) == 'z') &&
           (tolower ((unsigned char)name[3]) == 'x');
}

static int ZX_HasRomExtension (const char *name) {
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
           (tolower ((unsigned char)name[1]) == 'r') &&
           (tolower ((unsigned char)name[2]) == 'o') &&
           (tolower ((unsigned char)name[3]) == 'm');
}

static int ZX_IsEnterKey (uint8_t key) {
    return (key == '\n') || (key == '\r');
}

/* Streaming browser: open `path` and count how many *visible* entries it
   contains (directories first, then mode-filtered files).  No names are
   stored -- the per-page cache is populated lazily by ZX_BrowserGetEntry().
   Returns 1 on success (s_browser_count is valid), 0 on failure. */
static int ZX_BrowserLoadFiles (const char *path) {
    DIR dir;
    FILINFO fno;
    FRESULT fr;
    const char *target = ((path != NULL) && (path[0] != '\0')) ? path : "/";
    uint8_t attempt;

    /* Always invalidate the page cache when (re)loading a directory: the
       cached names are indexed by absolute directory position, so they are
       only valid while we stay in the same directory. */
    for (attempt = 0u; attempt < ZX_BROWSER_PAGE_ROWS; ++attempt) {
        s_browser_cache_valid[attempt] = 0u;
        s_browser_cache_name[attempt][0] = '\0';
        s_browser_cache_idx[attempt] = 0xFFFFu;
        s_browser_cache_is_dir[attempt] = 0u;
    }

    for (attempt = 0u; attempt < ZX_BROWSER_SCAN_RETRIES; ++attempt) {
        uint16_t total = 0u;

        /* Pass 1: directories first (skip hidden/dot entries) */
        fr = f_opendir (&dir, target);
        if (fr != FR_OK) {
            Delay_Ms (80u);
            continue;
        }
        for (;;) {
            fr = f_readdir (&dir, &fno);
            if ((fr != FR_OK) || (fno.fname[0] == '\0')) {
                break;
            }
            if (fno.fname[0] == '.') {
                continue;
            }
            if ((fno.fattrib & AM_DIR) == 0u) {
                continue;
            }
            if (total < 0xFFFEu) {
                ++total;
            }
        }
        f_closedir (&dir);
        if (fr != FR_OK) {
            Delay_Ms (80u);
            continue;
        }

        /* Pass 2: files for the active selector mode */
        fr = f_opendir (&dir, target);
        if (fr != FR_OK) {
            Delay_Ms (80u);
            continue;
        }
        for (;;) {
            fr = f_readdir (&dir, &fno);
            if ((fr != FR_OK) || (fno.fname[0] == '\0')) {
                break;
            }
            if ((fno.fattrib & AM_DIR) != 0u) {
                continue;
            }
            if (s_browser_mode == ZX_BROWSER_MODE_TAP) {
                if (!ZX_HasTapExtension ((const char *)fno.fname) &&
                    !ZX_HasTzxExtension ((const char *)fno.fname)) {
                    continue;
                }
            } else {
                if (!ZX_HasZ80Extension ((const char *)fno.fname) &&
                    !ZX_HasTapExtension ((const char *)fno.fname) &&
                    !ZX_HasTzxExtension ((const char *)fno.fname) &&
                    !ZX_HasRomExtension ((const char *)fno.fname)) {
                    continue;
                }
            }
            if (total < 0xFFFEu) {
                ++total;
            }
        }
        f_closedir (&dir);
        if (fr == FR_OK) {
            s_browser_count = total;
            /* Remember the directory we're listing so ZX_BrowserGetEntry()
               can re-open it. */
            strncpy (s_browser_dir_path, target,
                     (size_t)(ZX_BROWSER_PATH_MAX - 1u));
            s_browser_dir_path[ZX_BROWSER_PATH_MAX - 1u] = '\0';
            s_browser_dir_open = 0u;
            return 1;
        }
        Delay_Ms (80u);
    }

    s_browser_count = 0u;
    s_browser_dir_path[0] = '\0';
    s_browser_dir_open = 0u;
    printf ("%s: scan failed for '%s' (fr=%d)\r\n",
            (s_browser_mode == ZX_BROWSER_MODE_TAP) ? "tapselect" : "z80select",
            target,
            (int)fr);
    return 0;
}

/* Resolve a directory-entry index (0..s_browser_count-1) to (name, is_dir)
   by reading the directory forward from the start until we reach that index.
   Results are cached in s_browser_cache_* so repeated lookups for the
   current page are O(1) after the first miss.  Returns 1 on success, 0 on
   I/O failure. */
static int ZX_BrowserGetEntry (uint16_t index, char *out_name,
                               uint8_t *out_is_dir) {
    FILINFO fno;
    FRESULT fr;
    uint16_t skip;
    uint8_t cache_slot;

    if ((out_name == NULL) || (out_is_dir == NULL)) {
        return 0;
    }
    if (index >= s_browser_count) {
        out_name[0] = '\0';
        *out_is_dir = 0u;
        return 0;
    }

    /* Look in the per-page cache first. */
    for (cache_slot = 0u; cache_slot < ZX_BROWSER_PAGE_ROWS; ++cache_slot) {
        if ((s_browser_cache_valid[cache_slot] != 0u) &&
            (s_browser_cache_idx[cache_slot] == index)) {
            size_t n;
            for (n = 0u; n < (size_t)(ZX_BROWSER_NAME_MAX - 1u); ++n) {
                out_name[n] = s_browser_cache_name[cache_slot][n];
                if (out_name[n] == '\0') {
                    break;
                }
            }
            out_name[n] = '\0';
            *out_is_dir = s_browser_cache_is_dir[cache_slot];
            return 1;
        }
    }

    /* Cache miss.  Pick a slot to fill:
       1. Prefer an empty (invalid) slot so we don't kick out a useful entry.
       2. Otherwise, evict the slot holding the smallest index.  When paging
          forward the smallest indices are the least recently used; when the
          user pages backward the same strategy evicts rows we just left,
          which is also the right behavior since the user's new focus is the
          just-loaded page. */
    {
        uint8_t found_empty = 0u;
        uint8_t victim = 0u;
        uint16_t victim_idx = 0xFFFFu;
        for (cache_slot = 0u; cache_slot < ZX_BROWSER_PAGE_ROWS; ++cache_slot) {
            if (s_browser_cache_valid[cache_slot] == 0u) {
                victim = cache_slot;
                found_empty = 1u;
                break;
            }
            if (s_browser_cache_idx[cache_slot] < victim_idx) {
                victim_idx = s_browser_cache_idx[cache_slot];
                victim = cache_slot;
            }
        }
        cache_slot = victim;
        (void)found_empty;
    }

    /* Open (or re-open) the directory and walk to `index`. */
    fr = f_opendir (&s_browser_dir, s_browser_dir_path);
    if (fr != FR_OK) {
        out_name[0] = '\0';
        *out_is_dir = 0u;
        return 0;
    }
    s_browser_dir_open = 1u;

    for (skip = 0u; skip <= index; ++skip) {
        uint8_t is_dir_entry;
        fr = f_readdir (&s_browser_dir, &fno);
        if ((fr != FR_OK) || (fno.fname[0] == '\0')) {
            s_browser_dir_open = 0u;
            f_closedir (&s_browser_dir);
            out_name[0] = '\0';
            *out_is_dir = 0u;
            return 0;
        }
        /* Skip entries the loader would have skipped. */
        if (fno.fname[0] == '.') {
            --skip;
            continue;
        }
        is_dir_entry = (uint8_t)((fno.fattrib & AM_DIR) != 0u);
        if (skip < index) {
            if (is_dir_entry) {
                /* Directories appear before files.  All directories come
                   first in the listing, so a non-dir here at skip<index
                   means we need to keep scanning for the matching-file
                   position.  No correction needed. */
            } else {
                /* File: count only if it matches the active mode filter. */
                if (s_browser_mode == ZX_BROWSER_MODE_TAP) {
                    if (!ZX_HasTapExtension ((const char *)fno.fname) &&
                        !ZX_HasTzxExtension ((const char *)fno.fname)) {
                        --skip;
                    }
                } else {
                    if (!ZX_HasZ80Extension ((const char *)fno.fname) &&
                        !ZX_HasTapExtension ((const char *)fno.fname) &&
                        !ZX_HasTzxExtension ((const char *)fno.fname) &&
                        !ZX_HasRomExtension ((const char *)fno.fname)) {
                        --skip;
                    }
                }
            }
        }
        if (skip == index) {
            size_t n;
            for (n = 0u; n < (size_t)(ZX_BROWSER_NAME_MAX - 1u); ++n) {
                out_name[n] = fno.fname[n];
                if (out_name[n] == '\0') {
                    break;
                }
            }
            out_name[n] = '\0';
            *out_is_dir = is_dir_entry;
            /* Populate cache. */
            s_browser_cache_idx[cache_slot] = index;
            s_browser_cache_is_dir[cache_slot] = is_dir_entry;
            s_browser_cache_valid[cache_slot] = 1u;
            for (n = 0u; n < (size_t)(ZX_BROWSER_NAME_MAX - 1u); ++n) {
                s_browser_cache_name[cache_slot][n] = out_name[n];
                if (out_name[n] == '\0') {
                    break;
                }
            }
            s_browser_cache_name[cache_slot][n] = '\0';
            return 1;
        }
    }

    s_browser_dir_open = 0u;
    f_closedir (&s_browser_dir);
    out_name[0] = '\0';
    *out_is_dir = 0u;
    return 0;
}

static int ZX_BrowserRender (const char *path, uint16_t selected);

static const char *ZX_Z80LoadErrorText (int rc) {
    switch (rc) {
    case Z80L_ERR_OPEN: return "OPEN FAILED";
    case Z80L_ERR_READ: return "READ FAILED";
    case Z80L_ERR_FORMAT: return "BAD FORMAT";
    case Z80L_ERR_VERSION: return "UNSUPPORTED VERSION";
    case Z80L_ERR_LOAD: return "LOAD FAILED";
    case Z80L_ERR_LAUNCH: return "LAUNCH FAILED";
    default: return "UNKNOWN ERROR";
    }
}

static void ZX_WaitAnyKey (void) {
    for (;;) {
        uint8_t key = 0u;
        int rc = ZX_KeyPoll (&key);

        if (rc < 0) {
            return;
        }
        if (rc > 0) {
            return;
        }
        ZX_TerminalPollUsb ();
        if (s_usb_lost) {
            return;
        }
        /* ZX hardware reset cleared the screen.  Invalidate the diff cache
         * and re-commit whatever screen was last rendered so the user sees
         * the prompt again instead of a blank green border. */
        if (ZX_HandleExternalResetIfAny ()) {
            s_prev_valid = 0u;
            (void)ZX_TermCommit();
        }
        Delay_Ms (20u);
    }
}

/* Wait for any key and treat '0' as cancel on tape-ready screens.
   Returns: 1 = proceed to BASIC, 0 = cancel, -1 = read error. */
static int ZX_WaitAnyKeyOrCancel0 (void) {
    for (;;) {
        uint8_t key = 0u;
        int rc = ZX_KeyPoll (&key);

        if (rc < 0) {
            return -1;
        }
        if (rc > 0) {
            return (key == '0') ? 0 : 1;
        }
        ZX_TerminalPollUsb ();
        if (s_usb_lost) {
            return -1;
        }
        /* ZX hardware reset cleared the screen.  Invalidate the diff cache
         * and re-commit whatever screen was last rendered (typically the
         * TAPE READY prompt) so the user sees the prompt again. */
        if (ZX_HandleExternalResetIfAny ()) {
            s_prev_valid = 0u;
            (void)ZX_TermCommit();
        }
        Delay_Ms (20u);
    }
}

static const char *ZX_Z80HwModeText (uint8_t version, uint8_t hw_mode) {
    if (version <= 1u) { return "48K"; }
    if (version == 2u) {
        switch (hw_mode) {
            case 0u: return "48K";
            case 1u: return "48K+IF1";
            case 2u: return "SAMRAM";
            case 3u: return "128K";
            case 4u: return "128K+IF1";
            default: return "UNKNOWN";
        }
    }
    /* v3 */
    switch (hw_mode) {
        case 0u: return "48K";
        case 1u: return "48K+IF1";
        case 2u: return "48K+MGT";
        case 3u: return "SAMRAM";
        case 4u: return "128K";
        case 5u: return "128K+IF1";
        case 6u: return "+3";
        case 7u: return "+2A";
        default: return "UNKNOWN";
    }
}

static int ZX_BrowserShowZ80Info (const char *path, const Z80FileInfo *info) {
    uint8_t normal_attr = (uint8_t)((7u << 3) | 0u);
    uint8_t title_attr  = (uint8_t)((1u << 3) | 7u);
    uint8_t warn_attr   = (uint8_t)((6u << 3) | 0u);
    char line[ZX_TERM_COLS + 1u];
    const char *target;
    uint8_t hw_attr;

    target = ZX_Z80HwModeText (info->version, info->hw_mode);

    ZX_TermModelClear();
    ZX_TermModelWriteAt (0u, 0u, "RISKY SPECCY", normal_attr);
    ZX_TermModelWriteAt (0u, 14u, FIRMWARE_VERSION_STRING, normal_attr);
    ZX_TermModelWriteAt (2u, 0u, "Z80 SNAPSHOT INFO", title_attr);

    ZX_TermModelWriteAt (4u, 0u, (path != NULL) ? path : "(unknown)", normal_attr);

    memset (line, ' ', sizeof (line));
    line[ZX_TERM_COLS] = '\0';
    (void)snprintf (line, sizeof (line), "VERSION: %u", (unsigned)info->version);
    ZX_TermModelWriteAt (6u, 0u, line, normal_attr);

    hw_attr = (info->is_48k || info->is_128k) ? normal_attr : warn_attr;
    memset (line, ' ', sizeof (line));
    line[ZX_TERM_COLS] = '\0';
    (void)snprintf (line, sizeof (line), "TARGET:  %s", target);
    ZX_TermModelWriteAt (7u, 0u, line, hw_attr);

    if (info->version == 1u) {
        memset (line, ' ', sizeof (line));
        line[ZX_TERM_COLS] = '\0';
        (void)snprintf (line, sizeof (line), "BODY:    %s",
                        info->compressed ? "COMPRESSED" : "UNCOMPRESSED");
        ZX_TermModelWriteAt (8u, 0u, line, normal_attr);
    } else if (info->is_128k) {
        memset (line, ' ', sizeof (line));
        line[ZX_TERM_COLS] = '\0';
        (void)snprintf (line, sizeof (line), "7FFD:    %02X", (unsigned)info->page_7ffd);
        ZX_TermModelWriteAt (8u, 0u, line, normal_attr);
    }

    memset (line, ' ', sizeof (line));
    line[ZX_TERM_COLS] = '\0';
    (void)snprintf (line, sizeof (line), "SIZE:    %lu BYTES", (unsigned long)info->file_size);
    ZX_TermModelWriteAt (9u, 0u, line, normal_attr);

    memset (line, ' ', sizeof (line));
    line[ZX_TERM_COLS] = '\0';
    (void)snprintf (line, sizeof (line), "PC:%04X  SP:%04X",
                    (unsigned)info->pc, (unsigned)info->sp);
    ZX_TermModelWriteAt (10u, 0u, line, normal_attr);

    if (!info->is_48k && !info->is_128k) {
        ZX_TermModelWriteAt (12u, 0u, "WARNING: NOT SUPPORTED!", warn_attr);
        ZX_TermModelWriteAt (13u, 0u, "LOAD WILL FAIL", warn_attr);
    }

    ZX_TermModelWriteAt (22u, 0u, "ENTER LOAD  0 CANCEL", normal_attr);

    return ZX_TermCommit();
}

static int ZX_BrowserShowLoadError (const char *path, int rc) {
    uint8_t normal_attr = (uint8_t)((7u << 3) | 0u);
    uint8_t alert_attr = (uint8_t)((2u << 3) | 7u);
    uint8_t red_attr = (uint8_t)((7u << 3) | 2u);

    char line[ZX_TERM_COLS + 1u];
    const char *msg = ZX_Z80LoadErrorText (rc);

    ZX_TermModelClear();
    ZX_TermModelWriteAt (0u, 0u, "RISKY SPECCY", normal_attr);
    ZX_TermModelWriteAt (0u, 14u, FIRMWARE_VERSION_STRING, normal_attr);
    ZX_TermModelWriteAt (2u, 0u, "Z80 LOAD ERROR", alert_attr);
    ZX_TermModelWriteAt (4u, 0u, msg, red_attr);
    ZX_TermModelWriteAt (6u, 0u, "FILE:", normal_attr);
    ZX_TermModelWriteAt (7u, 0u, (path != NULL) ? path : "(unknown)", normal_attr);

    memset (line, ' ', sizeof (line));
    line[ZX_TERM_COLS] = '\0';
    (void)snprintf (line, sizeof (line), "RC=%d", rc);
    ZX_TermModelWriteAt (9u, 0u, line, normal_attr);

    ZX_TermModelWriteAt (12u, 0u, "PRESS ANY KEY TO RESET MENU", red_attr);

    return ZX_TermCommit();
}

static int ZX_BrowserShowTapReady (const char *path) {
    uint8_t normal_attr = (uint8_t)((7u << 3) | 0u);
    uint8_t alert_attr = (uint8_t)((1u << 3) | 7u);
    uint8_t red_attr = (uint8_t)((7u << 3) | 2u);

    ZX_TermModelClear();
    ZX_TermModelWriteAt (0u, 0u, "RISKY SPECCY", normal_attr);
    ZX_TermModelWriteAt (0u, 14u, FIRMWARE_VERSION_STRING, normal_attr);
    ZX_TermModelWriteAt (2u, 0u, "TAPE READY", alert_attr);
    ZX_TermModelWriteAt (3u, 0u, "FILE:", normal_attr);
    ZX_TermModelWriteAt (4u, 0u, (path != NULL) ? path : "(unknown)", normal_attr);
    ZX_TermModelWriteAt (6u, 0u, "TYPE LOAD \"\" AND PRESS ENTER", normal_attr);
    ZX_TermModelWriteAt (8u, 0u, "SHORT-PRESS PLAY/RESET BUTTON", normal_attr);
    ZX_TermModelWriteAt (9u, 0u, "TO START PLAYBACK", normal_attr);
    ZX_TermModelWriteAt (11u, 0u, "ANY KEY LOAD BASIC 0 CANCEL", red_attr);


    return ZX_TermCommit();
}

static int ZX_BrowserShowRomInfo (const char *path, uint32_t size) {
    uint8_t normal_attr = (uint8_t)((7u << 3) | 0u);
    uint8_t title_attr  = (uint8_t)((1u << 3) | 7u);
    uint8_t warn_attr   = (uint8_t)((6u << 3) | 0u);
    char line[ZX_TERM_COLS + 1u];

    ZX_TermModelClear();
    ZX_TermModelWriteAt (0u, 0u, "RISKY SPECCY", normal_attr);
    ZX_TermModelWriteAt (0u, 14u, FIRMWARE_VERSION_STRING, normal_attr);
    ZX_TermModelWriteAt (2u, 0u, "INTERFACE 2 CARTRIDGE", title_attr);

    ZX_TermModelWriteAt (4u, 0u, (path != NULL) ? path : "(unknown)", normal_attr);

    memset (line, ' ', sizeof (line));
    line[ZX_TERM_COLS] = '\0';
    (void)snprintf (line, sizeof (line), "SIZE:    %lu BYTES", (unsigned long)size);
    ZX_TermModelWriteAt (6u, 0u, line, normal_attr);

    memset (line, ' ', sizeof (line));
    line[ZX_TERM_COLS] = '\0';
    (void)snprintf (line, sizeof (line), "MAPPED:  0000-3FFF (16K)");
    ZX_TermModelWriteAt (7u, 0u, line, normal_attr);

    if (size == 0u) {
        ZX_TermModelWriteAt (10u, 0u, "WARNING: EMPTY FILE", warn_attr);
        ZX_TermModelWriteAt (11u, 0u, "LOAD WILL FAIL", warn_attr);
    } else if (size > 0x4000u) {
        ZX_TermModelWriteAt (10u, 0u, "WARNING: > 16K, WILL TRUNCATE", warn_attr);
    } else if (size < 0x4000u) {
        ZX_TermModelWriteAt (10u, 0u, "NOTE: PADDED TO 16K (0xFF)", normal_attr);
    }

    ZX_TermModelWriteAt (14u, 0u, "AFTER LOAD: PURE-ROM MODE", normal_attr);
    ZX_TermModelWriteAt (15u, 0u, "USE HW RESET TO EXIT", normal_attr);

    ZX_TermModelWriteAt (22u, 0u, "ENTER LOAD  0 CANCEL", normal_attr);

    return ZX_TermCommit();
}

static int ZX_BrowserShowRomLoading (const char *path) {
    uint8_t normal_attr = (uint8_t)((7u << 3) | 0u);
    uint8_t alert_attr  = (uint8_t)((1u << 3) | 7u);

    ZX_TermModelClear();
    ZX_TermModelWriteAt (0u, 0u, "RISKY SPECCY", normal_attr);
    ZX_TermModelWriteAt (0u, 14u, FIRMWARE_VERSION_STRING, normal_attr);
    ZX_TermModelWriteAt (2u, 0u, "LOADING CARTRIDGE...", alert_attr);
    ZX_TermModelWriteAt (4u, 0u, (path != NULL) ? path : "(unknown)", normal_attr);
    ZX_TermModelWriteAt (10u, 0u, "PLEASE WAIT", normal_attr);
    return ZX_TermCommit();
}

static int ZX_BrowserRenderRetry (const char *path, uint16_t selected) {
    uint8_t retry;

    for (retry = 0u; retry < ZX_BROWSER_RENDER_RETRIES; ++retry) {
        if (ZX_BrowserRender (path, selected)) {
            return 1;
        }
        Delay_Ms (80u);
    }
    return 0;
}

static int ZX_BrowserBuildPath (char *out, size_t out_size, const char *dir, const char *name) {
    const char *base = ((dir != NULL) && (dir[0] != '\0')) ? dir : "/";
    size_t len = strlen (base);
    int written;

    if ((len > 0u) && (base[len - 1u] == '/')) {
        written = snprintf (out, out_size, "%s%s", base, name);
    } else if ((len == 1u) && (base[0] == '/')) {
        written = snprintf (out, out_size, "/%s", name);
    } else {
        written = snprintf (out, out_size, "%s/%s", base, name);
    }

    if ((written < 0) || ((size_t)written >= out_size)) {
        if (out_size != 0u) {
            out[out_size - 1u] = '\0';
        }
        return 0;
    }
    return 1;
}

static int ZX_BrowserRender (const char *path, uint16_t selected) {
    uint16_t page_start;
    uint8_t row;
    uint8_t normal_attr = (uint8_t)((7u << 3) | 0u);
    uint8_t select_attr = (uint8_t)((0u << 3) | 7u);
    char line[ZX_TERM_COLS + 1u];

    ZX_TermModelClear();
    ZX_TermModelWriteAt (0u, 0u, "RISKY SPECCY", normal_attr);
    ZX_TermModelWriteAt (0u, 14u, FIRMWARE_VERSION_STRING, normal_attr);
    ZX_TermModelWriteAt (1u, 0u, ((path != NULL) && (path[0] != '\0')) ? path : "/", normal_attr);
    ZX_TermModelWriteAt (2u, 0u, "Q/A MOVE  O/P PAGE  W/E 90", normal_attr);
    if (s_browser_stack_depth > 0u) {
        ZX_TermModelWriteAt (3u, 0u, "ENTER OPEN  0 BACK", normal_attr);
    } else {
        ZX_TermModelWriteAt (3u, 0u,
                             (s_browser_mode == ZX_BROWSER_MODE_TAP) ? "ENTER QUEUE/OPEN" : "ENTER LOAD/OPEN",
                             normal_attr);
    }

    if (s_browser_count == 0u) {
        ZX_TermModelWriteAt (6u, 0u, "NO FILES FOUND", normal_attr);
        return ZX_TermCommit();
    }

    page_start = (uint16_t)((selected / ZX_BROWSER_PAGE_ROWS) * ZX_BROWSER_PAGE_ROWS);
    for (row = 0u; row < ZX_BROWSER_PAGE_ROWS; ++row) {
        uint16_t index = (uint16_t)page_start + (uint16_t)row;
        uint8_t attr = (index == selected) ? select_attr : normal_attr;
        ZX_TermModelFillRow ((uint8_t)(5u + row), attr);
        if (index >= s_browser_count) {
            continue;
        }

        memset (line, ' ', sizeof (line));
        line[ZX_TERM_COLS] = '\0';
        line[0] = (index == selected) ? '>' : ' ';

        {
            char entry_name[ZX_BROWSER_NAME_MAX];
            uint8_t entry_is_dir;
            if (ZX_BrowserGetEntry (index, entry_name, &entry_is_dir)) {
                uint8_t i;
                if (entry_is_dir != 0u) {
                    /* Show directories as [NAME] */
                    line[1u] = '[';
                    for (i = 0u; (i < (uint8_t)(ZX_TERM_COLS - 4u)) && (entry_name[i] != '\0'); ++i) {
                        line[2u + i] = entry_name[i];
                    }
                    line[2u + i] = ']';
                } else {
                    for (i = 0u; (i < (uint8_t)(ZX_TERM_COLS - 2u)) && (entry_name[i] != '\0'); ++i) {
                        line[1u + i] = entry_name[i];
                    }
                }
            }
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

/* Show the "Insert USB Drive" prompt screen.  had_error != 0 adds a
   second line indicating the previous attempt failed. */
static void ZX_ShowUsbPromptScreen (int had_error) {
    uint8_t normal_attr = (uint8_t)((7u << 3) | 0u);  /* white bg, black fg */
    uint8_t warn_attr   = (uint8_t)((6u << 3) | 0u);  /* yellow bg, black fg */
    uint8_t err_attr    = (uint8_t)((1u << 3) | 7u);  /* red bg, white fg */

    ZX_TermModelClear();
    ZX_TermModelWriteAt (0u, 0u, "RISKY SPECCY", normal_attr);
    ZX_TermModelWriteAt (0u, 14u, FIRMWARE_VERSION_STRING, normal_attr);
    ZX_TermModelWriteAt (2u, 0u, "INSERT USB DRIVE", warn_attr);
    ZX_TermModelWriteAt (4u, 0u, "PRESS ENTER TO CONTINUE", normal_attr);
    if (had_error) {
        ZX_TermModelWriteAt (6u, 0u, "USB DRIVE NOT DETECTED", err_attr);
        ZX_TermModelWriteAt (7u, 0u, "TRY AGAIN", err_attr);
    }
    if (!ZX_TermCommit()) {
        printf ("WARN: USB prompt commit failed\r\n");
    }
}

/* Block until a USB MSC drive is detected.  Displays a ZX-screen prompt asking
   the user to insert the drive and press Enter; retries with an error notice if
   the drive is still absent when Enter is pressed. */
void ZX_TerminalWaitUsbDriveReady (void) {
    int draw_suspended = 0;
    int had_error = 0;

    /* Fast path: if USB is already enumerated, go straight to the menu. */
    {
        uint8_t usb_ret = USBH_PreDeal();
        if (usb_ret == DEF_SUCCESS) {
            printf ("USB drive already ready\r\n");
            return;
        }
    }

    if (!ZX_WaitNmiMailboxReady (5000u)) {
        printf ("ERR: USB prompt mailbox not ready\r\n");
        return;
    }

    s_selector_font_mode = 1u;
    ZX_CartDrawSuspend();
    Delay_Ms (50u);
    draw_suspended = 1;

    for (;;) {
        ZX_ShowUsbPromptScreen (had_error);

        /* Wait for Enter on the ZX keyboard. */
        for (;;) {
            uint8_t key = 0u;
            int rc = ZX_KeyPoll (&key);
            if (rc < 0) { break; }
            if ((rc > 0) && ZX_IsEnterKey (key)) { break; }
            /* ZX hardware reset cleared the prompt screen.  Invalidate the
             * diff cache and redraw it so the user sees the prompt again. */
            if (ZX_HandleExternalResetIfAny ()) {
                s_prev_valid = 0u;
                if (!ZX_TermCommit()) {
                    printf ("WARN: USB prompt redraw after ZX reset timeout\r\n");
                }
            }
            Delay_Ms (20u);
        }

        /* Check USB readiness. */
        {
            uint8_t usb_ret = DEF_DEFAULT;
            uint8_t tries;

            /* Give the host stack time to observe attach/enumeration transitions. */
            for (tries = 0; tries < 100; ++tries) {
                usb_ret = USBH_PreDeal();
                if (usb_ret == DEF_SUCCESS) {
                    break;
                }
                Delay_Ms (20u);
            }

            /* If still in default state, reset host state once and retry briefly. */
            if (usb_ret == DEF_DEFAULT) {
                ClearUSB();
                USB_Initialization();
                Delay_Ms (50u);

                for (tries = 0; tries < 50; ++tries) {
                    usb_ret = USBH_PreDeal();
                    if (usb_ret == DEF_SUCCESS) {
                        break;
                    }
                    Delay_Ms (20u);
                }
            }

            if (usb_ret == DEF_SUCCESS) {
                printf ("USB drive ready\r\n");
                break;
            }
            printf ("USB not ready (ret=%d), re-prompting\r\n", (int)usb_ret);
            had_error = 1;
        }
    }

    s_selector_font_mode = 0u;
    if (draw_suspended) {
        ZX_TerminalMarkBridgeDirty();
        ZX_CartDrawResume();
    }
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
    s_tapselect_pending[0] = '\0';
}

void ZX_TerminalMarkBridgeDirty (void) {
    ++s_bridge_seq;
    (void)ZX_CartRamWriteBlock (ZX_BRIDGE_SEQ_ADDR, &s_bridge_seq, 1u);
}

void ZX_TerminalCommandViewOff (void) {
    uint8_t value = 0u;

    if (!ZX_CartRamWriteBlock (ZX_VIEW_LEN_ADDR, &value, 1u)) {
        printf ("ERR: zxview disable failed\r\n");
        return;
    }

    ++s_view_seq;
    if (!ZX_CartRamWriteBlock (ZX_VIEW_SEQ_ADDR, &s_view_seq, 1u)) {
        printf ("ERR: zxview seq write failed\r\n");
        return;
    }

    ZX_TriggerNMI();
    printf ("ZX RAM viewer disabled\r\n");
}

void ZX_TerminalCommandView (uint16_t address, uint8_t length) {
    uint8_t config[3];

    config[0] = (uint8_t)(address & 0x00FFu);
    config[1] = (uint8_t)(address >> 8);
    config[2] = length;

    if (!ZX_CartRamWriteBlock (ZX_VIEW_ADDR_LO, config, 3u)) {
        printf ("ERR: zxview config write failed\r\n");
        return;
    }

    ++s_view_seq;
    if (!ZX_CartRamWriteBlock (ZX_VIEW_SEQ_ADDR, &s_view_seq, 1u)) {
        printf ("ERR: zxview seq write failed\r\n");
        return;
    }

    ZX_TriggerNMI();
    printf ("ZX RAM viewer set to 0x%04X (%u byte%s)\r\n",
            (unsigned)address,
            (unsigned)length,
            (length == 1u) ? "" : "s");
}

void ZX_TerminalCommandBridgeText (char *firstToken) {
    uint8_t text[ZX_BRIDGE_MAX_TEXT];
    uint16_t len = 0u;
    char *tok = firstToken;

    if (tok == NULL) {
        printf ("Usage: zxmsg <text>\r\n");
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
        printf ("ERR: bridge text write failed\r\n");
        return;
    }

    {
        uint8_t l = (uint8_t)len;
        if (!ZX_CartRamWriteBlock (ZX_BRIDGE_LEN_ADDR, &l, 1u)) {
            printf ("ERR: bridge len write failed\r\n");
            return;
        }
    }

    ZX_TerminalMarkBridgeDirty();
    ZX_TriggerNMI();
    printf ("ZX message sent (%u chars)\r\n", (unsigned)len);
}

void ZX_TerminalCommandTermInit (void) {
    s_term_fg = 0u;
    s_term_bg = 7u;
    s_term_bright = 0u;
    s_term_esc_state = 0u;
    ZX_TermCsiReset();

    if (!ZX_TermClear() || !ZX_TermCommit()) {
        printf ("ERR: terminal init draw failed\r\n");
        return;
    }
    printf ("ZX terminal ready (MPU-rendered 32x24, white bg/black text)\r\n");
}

void ZX_TerminalCommandTermWrite (char *firstToken) {
    uint8_t buf[192];
    uint16_t len = 0u;
    char joined[192];
    uint16_t used = 0u;
    char *tok = firstToken;

    if (tok == NULL) {
        printf ("Usage: zxtty <text with \\n \\r \\t \\e escapes>\r\n");
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
        printf ("ERR: zxtty input too long\r\n");
        return;
    }

    if (!ZX_TermWriteBuffer (buf, len)) {
        printf ("ERR: zxtty write failed\r\n");
        return;
    }
    if (!ZX_TermCommit()) {
        printf ("ERR: zxtty flush failed\r\n");
        return;
    }
}

int ZX_TerminalCommandZ80Select (const char *path) {
    uint16_t selected = 0u;
    uint8_t suppress_enter_loops = 0u;
    char full_path[ZX_BROWSER_PATH_MAX];
    char cur_path[ZX_BROWSER_PATH_MAX];
    int draw_suspended = 0;
    int launched = 0;

    s_browser_mode = ZX_BROWSER_MODE_Z80;
    s_z80select_pending[0] = '\0';
    s_browser_stack_depth = 0u;
    strncpy (cur_path, ((path != NULL) && (path[0] != '\0')) ? path : "/",
             (size_t)(ZX_BROWSER_PATH_MAX - 1u));
    cur_path[ZX_BROWSER_PATH_MAX - 1u] = '\0';

    if (!ZX_WaitNmiMailboxReady (3000u)) {
        printf ("ERR: z80select mailbox not ready\r\n");
        goto done;
    }

    s_selector_font_mode = 1u;
    ZX_CartDrawSuspend();
    Delay_Ms (50u);
    draw_suspended = 1;

    if (!ZX_BrowserLoadFiles (cur_path)) {
        goto done;
    }
    if (!ZX_BrowserRenderRetry (cur_path, selected)) {
        printf ("ERR: z80select draw failed\r\n");
        goto done;
    }

    printf ("z80select: Q/A move, O/P page, ENTER load/open, 0 back\r\n");

    for (;;) {
        uint8_t key = 0u;
        int rc = ZX_KeyPoll (&key);

        if (rc < 0) {
            printf ("ERR: key mailbox read failed\r\n");
            goto done;
        }
        if (rc == 0) {
            /* While the user isn't pressing keys, poll USB so a
             * disconnect is detected within one keyboard poll period
             * instead of after the user eventually presses something. */
            ZX_TerminalPollUsb ();
            if (s_usb_lost) {
                printf ("z80select: USB drive gone, aborting browser\r\n");
                launched = 0;
                goto done;
            }
            if (suppress_enter_loops > 0u) {
                --suppress_enter_loops;
            }
            /* Detect a ZX hardware reset (PC6 falling edge).  zxprog clears
             * 0x4000-0xFFFF on startup, which wipes the screen we pushed;
             * invalidate the diff cache so the next render pushes the full
             * 6144 + 768 bytes, then redraw the current page so the browser
             * comes back instead of staying blank until the user types. */
            if (ZX_HandleExternalResetIfAny ()) {
                s_prev_valid = 0u;
                if (!ZX_BrowserRenderRetry (cur_path, selected)) {
                    printf ("WARN: z80select redraw after ZX reset timeout\r\n");
                }
            }
            Delay_Ms (20u);
            continue;
        }

        if ((key == 'q') || (key == 'Q')) {
            if (selected > 0u) {
                --selected;
                if (!ZX_BrowserRenderRetry (cur_path, selected)) {
                    printf ("WARN: z80select redraw timeout\r\n");
                }
            }
            suppress_enter_loops = 0u;
            continue;
        }
        if ((key == 'a') || (key == 'A')) {
            if ((uint16_t)selected + 1u < s_browser_count) {
                ++selected;
                if (!ZX_BrowserRenderRetry (cur_path, selected)) {
                    printf ("WARN: z80select redraw timeout\r\n");
                }
            }
            suppress_enter_loops = 0u;
            continue;
        }
        if ((key == 'o') || (key == 'O')) {
            if (selected >= ZX_BROWSER_PAGE_ROWS) {
                selected = (uint16_t)(selected - ZX_BROWSER_PAGE_ROWS);
            } else {
                selected = 0u;
            }
            if (!ZX_BrowserRenderRetry (cur_path, selected)) {
                printf ("WARN: z80select redraw timeout\r\n");
            }
            suppress_enter_loops = 20u;
            continue;
        }
        if ((key == 'p') || (key == 'P')) {
            uint16_t next = (uint16_t)selected + ZX_BROWSER_PAGE_ROWS;
            if (next >= s_browser_count) {
                next = (s_browser_count == 0u) ? 0u : (uint16_t)(s_browser_count - 1u);
            }
            selected = next;
            if (!ZX_BrowserRenderRetry (cur_path, selected)) {
                printf ("WARN: z80select redraw timeout\r\n");
            }
            suppress_enter_loops = 20u;
            continue;
        }
        if ((key == 'w') || (key == 'W')) {
            /* Jump back by ZX_BROWSER_JUMP_STEP (5 pages). */
            if (selected >= ZX_BROWSER_JUMP_STEP) {
                selected = (uint16_t)(selected - ZX_BROWSER_JUMP_STEP);
            } else {
                selected = 0u;
            }
            if (!ZX_BrowserRenderRetry (cur_path, selected)) {
                printf ("WARN: z80select redraw timeout\r\n");
            }
            suppress_enter_loops = 20u;
            continue;
        }
        if ((key == 'e') || (key == 'E')) {
            /* Jump forward by ZX_BROWSER_JUMP_STEP (5 pages). */
            uint16_t next = (uint16_t)selected + ZX_BROWSER_JUMP_STEP;
            if (next >= s_browser_count) {
                next = (s_browser_count == 0u) ? 0u : (uint16_t)(s_browser_count - 1u);
            }
            selected = next;
            if (!ZX_BrowserRenderRetry (cur_path, selected)) {
                printf ("WARN: z80select redraw timeout\r\n");
            }
            suppress_enter_loops = 20u;
            continue;
        }
        if (key == '0') {
            if (s_browser_stack_depth > 0u) {
                --s_browser_stack_depth;
                strncpy (cur_path,
                         s_browser_path_stack[s_browser_stack_depth],
                         (size_t)(ZX_BROWSER_PATH_MAX - 1u));
                cur_path[ZX_BROWSER_PATH_MAX - 1u] = '\0';
                selected = s_browser_selected_stack[s_browser_stack_depth];
                if (!ZX_BrowserLoadFiles (cur_path)) {
                    goto done;
                }
                if (!ZX_BrowserRenderRetry (cur_path, selected)) {
                    printf ("WARN: z80select redraw timeout\r\n");
                }
            }
            suppress_enter_loops = 0u;
            continue;
        }
        if (ZX_IsEnterKey (key)) {
            if (suppress_enter_loops > 0u) {
                continue;
            }
            {
                char entry_name[ZX_BROWSER_NAME_MAX];
                uint8_t entry_is_dir;
                if (!ZX_BrowserGetEntry ((uint16_t)selected, entry_name, &entry_is_dir)) {
                    printf ("z80select: cannot resolve entry %u\r\n",
                            (unsigned)selected);
                    continue;
                }
                if (entry_is_dir != 0u) {
                    /* Navigate into directory */
                    if (s_browser_stack_depth < 3u) {
                        char new_path[ZX_BROWSER_PATH_MAX];
                        if (!ZX_BrowserBuildPath (new_path, sizeof (new_path),
                                                  cur_path, entry_name)) {
                            printf ("z80select: path too long\r\n");
                            continue;
                        }
                        strncpy (s_browser_path_stack[s_browser_stack_depth],
                                 cur_path,
                                 (size_t)(ZX_BROWSER_PATH_MAX - 1u));
                        s_browser_path_stack[s_browser_stack_depth][ZX_BROWSER_PATH_MAX - 1u] = '\0';
                        s_browser_selected_stack[s_browser_stack_depth] = selected;
                        ++s_browser_stack_depth;
                        strncpy (cur_path, new_path, (size_t)(ZX_BROWSER_PATH_MAX - 1u));
                        cur_path[ZX_BROWSER_PATH_MAX - 1u] = '\0';
                        selected = 0u;
                        if (!ZX_BrowserLoadFiles (cur_path)) {
                            goto done;
                        }
                        if (!ZX_BrowserRenderRetry (cur_path, selected)) {
                            printf ("WARN: z80select redraw timeout\r\n");
                        }
                    } else {
                        printf ("z80select: max folder depth reached\r\n");
                    }
                    suppress_enter_loops = 20u;
                    continue;
                }
                if (!ZX_BrowserBuildPath (full_path, sizeof (full_path), cur_path, entry_name)) {
                    printf ("z80select: path too long, cannot open\r\n");
                    continue;
                }
            if (ZX_HasRomExtension (entry_name)) {
                    /* Interface 2 cartridge ROM: show info page, wait for ENTER to
                       confirm or 0 (any non-ENTER key) to cancel back to the
                       browser.  On confirm: show a brief LOADING page and switch
                       the cart engine into pure-ROM mode.  Board then acts as a
                       16 KB ROM cart until hardware reset. */
                    FILINFO rfi;
                    uint32_t rsize = 0u;
                    int rconfirmed = 0;

                    if (f_stat (full_path, &rfi) == FR_OK) {
                        rsize = (uint32_t)rfi.fsize;
                    }

                    if (!ZX_BrowserShowRomInfo (full_path, rsize)) {
                        printf ("WARN: rom info draw timeout\r\n");
                    }
                    for (;;) {
                        uint8_t ikey = 0u;
                        int krc = ZX_KeyPoll (&ikey);
                        if (krc < 0) { break; }
                        if (krc > 0) {
                            if (ZX_IsEnterKey (ikey)) { rconfirmed = 1; }
                            break;
                        }
                        ZX_TerminalPollUsb ();
                        if (s_usb_lost) { break; }
                        /* ZX hardware reset wiped the ROM info screen —
                         * invalidate the diff cache and redraw it so the
                         * user sees what they were confirming. */
                        if (ZX_HandleExternalResetIfAny ()) {
                            s_prev_valid = 0u;
                            if (!ZX_BrowserShowRomInfo (full_path, rsize)) {
                                printf ("WARN: rom info redraw after ZX reset timeout\r\n");
                            }
                        }
                        Delay_Ms (20u);
                    }
                    if (!rconfirmed) {
                        if (s_usb_lost) { launched = 0; goto done; }
                        if (!ZX_BrowserRenderRetry (cur_path, selected)) {
                            printf ("WARN: z80select redraw timeout\r\n");
                        }
                        suppress_enter_loops = 20u;
                        continue;
                    }

                    /* Show LOADING page (still in launcher mode — cart shadow RAM
                       at 0x3000+ is what backs the ZX display).  Then drop the
                       draw suspension and run the actual swap. */
                    if (!ZX_BrowserShowRomLoading (full_path)) {
                        printf ("WARN: rom loading draw timeout\r\n");
                    }

                    s_selector_font_mode = 0u;
                    if (draw_suspended) {
                        ZX_TerminalMarkBridgeDirty();
                        ZX_CartDrawResume();
                        draw_suspended = 0;
                    }
                    if (!IF2_LoadRomFromFile (full_path)) {
                        printf ("z80select: if2 load failed for %s\r\n", full_path);
                        /* Re-suspend draw and redraw the browser so the user can retry. */
                        s_selector_font_mode = 1u;
                        ZX_CartDrawSuspend();
                        Delay_Ms (50u);
                        draw_suspended = 1;
                        if (!ZX_BrowserRenderRetry (cur_path, selected)) {
                            printf ("WARN: z80select redraw timeout\r\n");
                        }
                        suppress_enter_loops = 20u;
                        continue;
                    }
                    launched = 1;
                    goto done;
                }
            if (ZX_HasTapExtension (entry_name) ||
                ZX_HasTzxExtension (entry_name)) {
                TAP_Player_Stop();
                if (!TAP_Player_Load (full_path)) {
                    printf ("z80select: tap prepare failed for %s\r\n", full_path);
                    suppress_enter_loops = 20u;
                    continue;
                }

                strncpy (s_tapselect_pending, full_path, (size_t)(ZX_BROWSER_PATH_MAX - 1u));
                s_tapselect_pending[ZX_BROWSER_PATH_MAX - 1u] = '\0';

                if (!ZX_BrowserShowTapReady (full_path)) {
                    printf ("WARN: z80select tap-ready draw timeout\r\n");
                }
                printf ("z80select: queued tap %s\r\n", full_path);
                printf ("z80select: type LOAD \"\" and press Enter on the Spectrum, then short-press Play/Reset button to start playback\r\n");
                printf ("z80select: press any Spectrum key to return to BASIC (0 cancels)\r\n");
                {
                    int go_basic = ZX_WaitAnyKeyOrCancel0();
                    if (go_basic < 0) {
                        printf ("ERR: key mailbox read failed\r\n");
                        goto done;
                    }
                    if (go_basic == 0) {
                        if (!ZX_BrowserRenderRetry (cur_path, selected)) {
                            printf ("WARN: z80select redraw timeout\r\n");
                        }
                        suppress_enter_loops = 20u;
                        continue;
                    }
                }
                s_selector_font_mode = 0u;
                if (draw_suspended) {
                    ZX_TerminalMarkBridgeDirty();
                    ZX_CartDrawResume();
                    draw_suspended = 0;
                }
                ZX_RomcsRelease();
                ZX_Z80Reset();
                goto done;
            } else {
                /* Show snapshot info page and wait for confirmation. */
                {
                    Z80FileInfo fi;
                    int info_rc = Z80_GetFileInfo (full_path, &fi);
                    if (info_rc == Z80L_OK) {
                        int confirmed = 0;
                        if (!ZX_BrowserShowZ80Info (full_path, &fi)) {
                            printf ("WARN: z80select info draw timeout\r\n");
                        }
                        for (;;) {
                            uint8_t ikey = 0u;
                            int krc = ZX_KeyPoll (&ikey);
                            if (krc < 0) { break; }
                            if (krc > 0) {
                                if (ZX_IsEnterKey (ikey)) { confirmed = 1; }
                                break;
                            }
                            ZX_TerminalPollUsb ();
                            if (s_usb_lost) { break; }
                            /* ZX hardware reset wiped the info screen —
                             * invalidate the diff cache and redraw it so
                             * the user sees what they were confirming. */
                            if (ZX_HandleExternalResetIfAny ()) {
                                s_prev_valid = 0u;
                                if (!ZX_BrowserShowZ80Info (full_path, &fi)) {
                                    printf ("WARN: z80select info redraw after ZX reset timeout\r\n");
                                }
                            }
                            Delay_Ms (20u);
                        }
                        if (!confirmed) {
                            if (s_usb_lost) { launched = 0; goto done; }
                            if (!ZX_BrowserRenderRetry (cur_path, selected)) {
                                printf ("WARN: z80select redraw timeout\r\n");
                            }
                            suppress_enter_loops = 20u;
                            continue;
                        }
                    }
                    /* Proceed with load (even if info read failed, attempt anyway). */
                    printf ("z80select: loading %s\r\n", full_path);
                    {
                        int load_rc = Z80_LoadAndRun (full_path);
                        if (load_rc == Z80L_OK) {
                            s_z80select_pending[0] = '\0';
                            launched = 1;
                            goto done;
                        }

                        printf ("z80select: load failed (rc=%d)\r\n", load_rc);
                        if (!ZX_BrowserShowLoadError (full_path, load_rc)) {
                            printf ("WARN: z80select error screen draw timeout\r\n");
                        }
                        ZX_WaitAnyKey();

                        selected = 0u;
                        if (!ZX_BrowserLoadFiles (cur_path)) {
                            goto done;
                        }
                        if (!ZX_BrowserRenderRetry (cur_path, selected)) {
                            printf ("WARN: z80select redraw timeout\r\n");
                        }
                        suppress_enter_loops = 20u;
                    }
                }
            }
            continue;
            }   /* close scope that contains entry_name / entry_is_dir */
        }
    }

done:
    s_selector_font_mode = 0u;
    if (draw_suspended) {
        ZX_TerminalMarkBridgeDirty();
        ZX_CartDrawResume();
    }
    return launched;
}

void ZX_TerminalCommandTapSelect (const char *path) {
    uint16_t selected = 0u;
    uint8_t suppress_enter_loops = 0u;
    char full_path[ZX_BROWSER_PATH_MAX];
    char cur_path[ZX_BROWSER_PATH_MAX];
    int draw_suspended = 0;
    int go_to_basic = 0;

    s_browser_mode = ZX_BROWSER_MODE_TAP;
    s_tapselect_pending[0] = '\0';
    s_browser_stack_depth = 0u;
    strncpy (cur_path, ((path != NULL) && (path[0] != '\0')) ? path : "/",
             (size_t)(ZX_BROWSER_PATH_MAX - 1u));
    cur_path[ZX_BROWSER_PATH_MAX - 1u] = '\0';

    if (!ZX_WaitNmiMailboxReady (3000u)) {
        printf ("ERR: tapselect mailbox not ready\r\n");
        goto done;
    }

    s_selector_font_mode = 1u;
    ZX_CartDrawSuspend();
    Delay_Ms (50u);
    draw_suspended = 1;

    if (!ZX_BrowserLoadFiles (cur_path)) {
        goto done;
    }
    if (!ZX_BrowserRenderRetry (cur_path, selected)) {
        printf ("ERR: tapselect draw failed\r\n");
        goto done;
    }

    printf ("tapselect: Q/A move, O/P page, ENTER queue/open, 0 back\r\n");

    for (;;) {
        uint8_t key = 0u;
        int rc = ZX_KeyPoll (&key);

        if (rc < 0) {
            printf ("ERR: key mailbox read failed\r\n");
            goto done;
        }
        if (rc == 0) {
            ZX_TerminalPollUsb ();
            if (s_usb_lost) {
                printf ("tapselect: USB drive gone, aborting browser\r\n");
                go_to_basic = 0;
                goto done;
            }
            if (suppress_enter_loops > 0u) {
                --suppress_enter_loops;
            }
            /* Detect a ZX hardware reset (PC6 falling edge).  zxprog clears
             * 0x4000-0xFFFF on startup, which wipes the screen we pushed;
             * invalidate the diff cache so the next render pushes the full
             * 6144 + 768 bytes, then redraw the current page so the browser
             * comes back instead of staying blank until the user types. */
            if (ZX_HandleExternalResetIfAny ()) {
                s_prev_valid = 0u;
                if (!ZX_BrowserRenderRetry (cur_path, selected)) {
                    printf ("WARN: tapselect redraw after ZX reset timeout\r\n");
                }
            }
            Delay_Ms (20u);
            continue;
        }

        if ((key == 'q') || (key == 'Q')) {
            if (selected > 0u) {
                --selected;
                if (!ZX_BrowserRenderRetry (cur_path, selected)) {
                    printf ("WARN: tapselect redraw timeout\r\n");
                }
            }
            suppress_enter_loops = 0u;
            continue;
        }
        if ((key == 'a') || (key == 'A')) {
            if ((uint16_t)selected + 1u < s_browser_count) {
                ++selected;
                if (!ZX_BrowserRenderRetry (cur_path, selected)) {
                    printf ("WARN: tapselect redraw timeout\r\n");
                }
            }
            suppress_enter_loops = 0u;
            continue;
        }
        if ((key == 'o') || (key == 'O')) {
            if (selected >= ZX_BROWSER_PAGE_ROWS) {
                selected = (uint16_t)(selected - ZX_BROWSER_PAGE_ROWS);
            } else {
                selected = 0u;
            }
            if (!ZX_BrowserRenderRetry (cur_path, selected)) {
                printf ("WARN: tapselect redraw timeout\r\n");
            }
            suppress_enter_loops = 20u;
            continue;
        }
        if ((key == 'p') || (key == 'P')) {
            uint16_t next = (uint16_t)selected + ZX_BROWSER_PAGE_ROWS;
            if (next >= s_browser_count) {
                next = (s_browser_count == 0u) ? 0u : (uint16_t)(s_browser_count - 1u);
            }
            selected = next;
            if (!ZX_BrowserRenderRetry (cur_path, selected)) {
                printf ("WARN: tapselect redraw timeout\r\n");
            }
            suppress_enter_loops = 20u;
            continue;
        }
        if ((key == 'w') || (key == 'W')) {
            /* Jump back by ZX_BROWSER_JUMP_STEP (5 pages). */
            if (selected >= ZX_BROWSER_JUMP_STEP) {
                selected = (uint16_t)(selected - ZX_BROWSER_JUMP_STEP);
            } else {
                selected = 0u;
            }
            if (!ZX_BrowserRenderRetry (cur_path, selected)) {
                printf ("WARN: tapselect redraw timeout\r\n");
            }
            suppress_enter_loops = 20u;
            continue;
        }
        if ((key == 'e') || (key == 'E')) {
            /* Jump forward by ZX_BROWSER_JUMP_STEP (5 pages). */
            uint16_t next = (uint16_t)selected + ZX_BROWSER_JUMP_STEP;
            if (next >= s_browser_count) {
                next = (s_browser_count == 0u) ? 0u : (uint16_t)(s_browser_count - 1u);
            }
            selected = next;
            if (!ZX_BrowserRenderRetry (cur_path, selected)) {
                printf ("WARN: tapselect redraw timeout\r\n");
            }
            suppress_enter_loops = 20u;
            continue;
        }
        if (key == '0') {
            if (s_browser_stack_depth > 0u) {
                --s_browser_stack_depth;
                strncpy (cur_path,
                         s_browser_path_stack[s_browser_stack_depth],
                         (size_t)(ZX_BROWSER_PATH_MAX - 1u));
                cur_path[ZX_BROWSER_PATH_MAX - 1u] = '\0';
                selected = s_browser_selected_stack[s_browser_stack_depth];
                if (!ZX_BrowserLoadFiles (cur_path)) {
                    goto done;
                }
                if (!ZX_BrowserRenderRetry (cur_path, selected)) {
                    printf ("WARN: tapselect redraw timeout\r\n");
                }
            }
            suppress_enter_loops = 0u;
            continue;
        }
        if (!ZX_IsEnterKey (key)) {
            continue;
        }
        if (suppress_enter_loops > 0u) {
            continue;
        }
        {
            char entry_name[ZX_BROWSER_NAME_MAX];
            uint8_t entry_is_dir;
            if (!ZX_BrowserGetEntry ((uint16_t)selected, entry_name, &entry_is_dir)) {
                printf ("tapselect: cannot resolve entry %u\r\n",
                        (unsigned)selected);
                continue;
            }
            if (entry_is_dir != 0u) {
                if (s_browser_stack_depth < 3u) {
                    char new_path[ZX_BROWSER_PATH_MAX];
                    if (!ZX_BrowserBuildPath (new_path, sizeof (new_path),
                                              cur_path, entry_name)) {
                        printf ("tapselect: path too long\r\n");
                        continue;
                    }
                    strncpy (s_browser_path_stack[s_browser_stack_depth],
                             cur_path,
                             (size_t)(ZX_BROWSER_PATH_MAX - 1u));
                    s_browser_path_stack[s_browser_stack_depth][ZX_BROWSER_PATH_MAX - 1u] = '\0';
                    s_browser_selected_stack[s_browser_stack_depth] = selected;
                    ++s_browser_stack_depth;
                    strncpy (cur_path, new_path, (size_t)(ZX_BROWSER_PATH_MAX - 1u));
                    cur_path[ZX_BROWSER_PATH_MAX - 1u] = '\0';
                    selected = 0u;
                    if (!ZX_BrowserLoadFiles (cur_path)) {
                        goto done;
                    }
                    if (!ZX_BrowserRenderRetry (cur_path, selected)) {
                        printf ("WARN: tapselect redraw timeout\r\n");
                    }
                } else {
                    printf ("tapselect: max folder depth reached\r\n");
                }
                suppress_enter_loops = 20u;
                continue;
            }
            if (!ZX_BrowserBuildPath (full_path, sizeof (full_path), cur_path, entry_name)) {
                printf ("tapselect: path too long, cannot queue\r\n");
                continue;
            }
        }

        TAP_Player_Stop();
        if (!TAP_Player_Load (full_path)) {
            printf ("tapselect: prepare failed for %s\r\n", full_path);
            suppress_enter_loops = 20u;
            continue;
        }

        strncpy (s_tapselect_pending, full_path, (size_t)(ZX_BROWSER_PATH_MAX - 1u));
        s_tapselect_pending[ZX_BROWSER_PATH_MAX - 1u] = '\0';

        if (!ZX_BrowserShowTapReady (full_path)) {
            printf ("WARN: tapselect ready screen draw timeout\r\n");
        }
        printf ("tapselect: queued %s\r\n", full_path);
        printf ("tapselect: type LOAD \"\" and press Enter on the Spectrum, then short-press BUTTON to start playback\r\n");
        printf ("tapselect: press any Spectrum key to return to BASIC (0 cancels)\r\n");
        {
            int go_basic_now = ZX_WaitAnyKeyOrCancel0();
            if (go_basic_now < 0) {
                printf ("ERR: key mailbox read failed\r\n");
                goto done;
            }
            if (go_basic_now == 0) {
                if (!ZX_BrowserRenderRetry (cur_path, selected)) {
                    printf ("WARN: tapselect redraw timeout\r\n");
                }
                suppress_enter_loops = 20u;
                continue;
            }
        }
        go_to_basic = 1;
        goto done;
    }

done:
    s_selector_font_mode = 0u;
    if (draw_suspended) {
        ZX_TerminalMarkBridgeDirty();
        ZX_CartDrawResume();
    }
    if (go_to_basic) {
        ZX_RomcsRelease();
        ZX_Z80Reset();
    }
}

const char *ZX_TerminalPendingZ80Selection (void) {
    return s_z80select_pending;
}

void ZX_TerminalClearPendingZ80Selection (void) {
    s_z80select_pending[0] = '\0';
}

const char *ZX_TerminalPendingTapSelection (void) {
    return s_tapselect_pending;
}

void ZX_TerminalClearPendingTapSelection (void) {
    s_tapselect_pending[0] = '\0';
}

/*----------------------------------------------------------------------
 * USB-lost signalling
 *
 * The blocking keyboard loops inside the file browser can't easily
 * jump back to the main loop on their own, so they call
 * ZX_TerminalPollUsb() each time the ZX keyboard has no input
 * available.  If the host stack reports the drive is gone we set
 * s_usb_lost and the browser returns to the main loop, which then
 * re-runs ZX_TerminalWaitUsbDriveReady().
 *----------------------------------------------------------------------*/
uint8_t ZX_TerminalUsbLost (void) {
    return s_usb_lost;
}

void ZX_TerminalClearUsbLost (void) {
    s_usb_lost = 0u;
}

void ZX_TerminalPollUsb (void) {
    if (s_usb_lost) return;            /* sticky until cleared */
    if (!USBH_IsReady ()) {
        s_usb_lost = 1u;
    }
}
