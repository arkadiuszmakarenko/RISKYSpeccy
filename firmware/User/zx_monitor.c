#include "zx_monitor.h"

#include "zx_bus.h"
#include "tap_loader.h"
#include "z80_loader.h"
#include "launch_test.h"
#include "ff.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZX_MONITOR_BUF_SIZE 96u
#define ZX_DUMP_MAX_LEN     256u
#define ZX_TEST_LEN         16u
#define ZX_CART_RAM_BASE    0x3000u
#define ZX_CART_RAM_LAST    0x3FFFu

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

#define ZX_GFXTEST_FRAMES   5u
#define ZX_GFXTEST_FRAME_MS 700u
#define ZX_GFXTEST_NMI_TO   200u

#define ZX_RAMTEST_CHUNK     0x0200u  /* matches ZX_NMI_WCMD_CHUNK in zx_bus.c */
#define ZX_RAMTEST_NMI_TO    300u
#define ZX_RAMTEST_DEFAULT_BASE 0xC000u
#define ZX_RAMTEST_DEFAULT_LEN  0x4000u  /* 16 KB top RAM, uncontended, safe */
#define ZX_RAMTEST_MAX_ERRORS   8u
#define ZX_BROWSER_MAX_FILES    64u
#define ZX_BROWSER_NAME_MAX     48u
#define ZX_BROWSER_PAGE_ROWS    18u

static char s_monitor_line[ZX_MONITOR_BUF_SIZE];
static uint8_t s_monitor_len = 0u;

static uint8_t s_gfx_pixels[ZX_SCREEN_PIXELS_LEN];
static uint8_t s_gfx_attrs[ZX_SCREEN_ATTRS_LEN];
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

static int ZX_TermFlushBuffers (void) {
    if (!ZX_NmiWriteBlock (ZX_SCREEN_PIXELS_ADDR,
                           s_gfx_pixels,
                           ZX_SCREEN_PIXELS_LEN,
                           ZX_TERM_NMI_TO)) {
        return 0;
    }
    if (!ZX_NmiWriteBlock (ZX_SCREEN_ATTRS_ADDR,
                           s_gfx_attrs,
                           ZX_SCREEN_ATTRS_LEN,
                           ZX_TERM_NMI_TO)) {
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

static USART_TypeDef *ZX_DebugUart (void) {
#if(DEBUG == DEBUG_UART1)
    return USART1;
#elif(DEBUG == DEBUG_UART2)
    return USART2;
#else
    return USART3;
#endif
}

static int ZX_UartTryReadChar (char *out) {
    USART_TypeDef *uart = ZX_DebugUart();
    if (USART_GetFlagStatus (uart, USART_FLAG_RXNE) == RESET) {
        return 0;
    }

    *out = (char)(USART_ReceiveData (uart) & 0xFFu);
    return 1;
}

static int ZX_StrIeq (const char *a, const char *b) {
    while ((*a != '\0') && (*b != '\0')) {
        if (tolower ((unsigned char)*a) != tolower ((unsigned char)*b)) {
            return 0;
        }
        ++a;
        ++b;
    }

    return (*a == '\0') && (*b == '\0');
}

static int ZX_ParseU32 (const char *text, uint32_t *value) {
    char *end = NULL;
    unsigned long parsed;

    if (text == NULL) {
        return 0;
    }

    parsed = strtoul (text, &end, 0);
    if ((end == text) || (*end != '\0')) {
        return 0;
    }

    *value = (uint32_t)parsed;
    return 1;
}

static int ZX_ParseAddressHex (const char *text, uint32_t *value) {
    char *end = NULL;
    unsigned long parsed;

    if (text == NULL) {
        return 0;
    }

    parsed = strtoul (text, &end, 16);
    if ((end == text) || (*end != '\0')) {
        return 0;
    }

    *value = (uint32_t)parsed;
    return 1;
}

static void ZX_PrintHelp (void) {
    printf("Commands:\r\n");
    printf("  help                         - show commands\r\n");
    printf("  dump <addr> <len>            - dump region (addr hex, len dec/0x)\r\n");
    printf("  rd <addr> [len]              - read byte(s), addr is hex\r\n");
    printf("  wd <addr> <byte>             - write byte (addr/byte hex, or byte dec/0x)\r\n");
    printf("  wfill <addr> <byte> <len>    - fill range (addr/byte hex, len dec/0x)\r\n");
    printf("  wram <addr> <byte>           - NMI write to any addr (incl ULA RAM)\r\n");
    printf("  wfillram <addr> <byte> <len> - NMI fill range (incl ULA RAM)\r\n");
    printf("  nmitest <addr> <len>         - write/verify/restore via NMI at addr\r\n");
    printf("  nmidiag                      - probe ZX addr map via NMI vs BUSREQ\r\n");
    printf("  suspend                      - pause ZX screen drawing\r\n");
    printf("  resume                       - resume ZX screen drawing\r\n");
    printf("  gfxtest                      - animated gfx demo on ZX screen RAM\r\n");
    printf("  ramtest [addr] [len]         - write/read patterns across ZX RAM (default C000/4000)\r\n");
    printf("  zxview <addr> [len]          - show live ZX RAM bytes on screen\r\n");
    printf("  zxmsg <text>                 - send text to ZX on-screen host bridge\r\n");
    printf("  zxttyinit                    - init MPU-side terminal and clear screen\r\n");
    printf("  zxtty <text>                 - write text/escapes (\\n \\r \\t \\e[...m) via MPU\r\n");
    printf("  zxttytest                    - render terminal demo/test pattern\r\n");
    printf("  keyread                      - read one ZX key event from mailbox\r\n");
    printf("  keytest [count] [timeout_ms] - wait for ZX key event(s) and print them\r\n");
    printf("  nmi                          - trigger NMI pulse\r\n");
    printf("  test [addr]                  - %u-byte self-test in 3000..3FFF (hex addr)\r\n", (unsigned)ZX_TEST_LEN);
    printf("  busdiag                      - probe all ZX memory regions for r/w\r\n");
    printf("  busrwtest <addr> <n>         - write/readback test N times at addr\r\n");
    printf("  ls [path]                    - list directory on USB drive\r\n");
    printf("  tapinfo <path>               - parse .tap and list its blocks\r\n");
    printf("  taprun <path> [start_hex]    - load .tap CODE blocks, release ROMCS, run\r\n");
    printf("  z80info <path>               - parse .z80 v1 snapshot header\r\n");
    printf("  z80run-bus <path>            - load .z80 via BUSREQ, resume snapshot\r\n");
    printf("  z80run-nmi <path>            - load .z80 via NMI mailbox, resume snapshot\r\n");
    printf("  z80select [path]             - browse USB .z80 files on ZX screen and run\r\n");
    printf("  launchtest <id>              - 1=NMI mailbox, 2=marker trampoline\r\n");
    printf("  romcs <on|off>               - assert/release cart ROMCS manually\r\n");
}

static void ZX_CommandViewOff (void) {
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

static void ZX_CommandView (uint16_t address, uint8_t length) {
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

static void ZX_CommandBridgeText (char *firstToken) {
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

    ++s_bridge_seq;
    if (!ZX_CartRamWriteBlock (ZX_BRIDGE_SEQ_ADDR, &s_bridge_seq, 1u)) {
        printf("ERR: bridge seq write failed\r\n");
        return;
    }

    ZX_TriggerNMI();
    printf("ZX message sent (%u chars)\r\n", (unsigned)len);
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

static void ZX_CommandTermInit (void) {
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

static void ZX_CommandTermWrite (char *firstToken) {
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

static void ZX_CommandTermTest (void) {
    static const uint8_t kTermTestScript[] =
        "ZX MPU TERMINAL TEST\n"
        "DEFAULT: BLACK ON WHITE\n"
        "\\e[31mRED \\e[32mGREEN \\e[34mBLUE \\e[30;47mRESET\n"
        "TAB:\tCOL2\tCOL3\n"
        "CURSOR MOVE NEXT...\n"
        "\\e[10;6HROW10 COL6\n"
        "\\e[12;1HCLEAR TO EOL -> XXXXX\\e[K\n"
        "\\e[22;1HSCROLL TEST START\n"
        "LINE 23\n"
        "LINE 24\n"
        "LINE 25 -> SHOULD SCROLL";
    uint8_t parsed[256];
    uint16_t len = 0u;

    ZX_CommandTermInit();
    if (!ZX_ParseEscapedText ((const char *)kTermTestScript,
                              parsed,
                              (uint16_t)sizeof (parsed),
                              &len)) {
        printf("ERR: zxttytest script parse failed\r\n");
        return;
    }

    if (!ZX_TermWriteBuffer (parsed, len)) {
        printf("ERR: zxttytest draw failed\r\n");
        return;
    }
    if (!ZX_TermCommit()) {
        printf("ERR: zxttytest flush failed\r\n");
        return;
    }

    printf("zxttytest: terminal demo rendered\r\n");
}

static void ZX_CommandKeyRead (void) {
    uint8_t key = 0u;
    int rc = ZX_KeyPoll (&key);
    if (rc < 0) {
        printf("ERR: key mailbox read failed\r\n");
        return;
    }
    if (rc == 0) {
        printf("No key event\r\n");
        return;
    }

    if ((key >= 32u) && (key <= 126u)) {
        printf("KEY: 0x%02X '%c'\r\n", (unsigned)key, (char)key);
    } else {
        printf("KEY: 0x%02X\r\n", (unsigned)key);
    }
}

static void ZX_CommandKeyTest (uint8_t wanted_count, uint32_t timeout_ms) {
    uint8_t got = 0u;
    uint32_t waited = 0u;

    if (wanted_count == 0u) {
        wanted_count = 1u;
    }
    if (timeout_ms == 0u) {
        timeout_ms = 3000u;
    }

    printf("keytest: waiting for %u key event%s (%lu ms timeout)\r\n",
           (unsigned)wanted_count,
           (wanted_count == 1u) ? "" : "s",
           (unsigned long)timeout_ms);

    while ((got < wanted_count) && (waited < timeout_ms)) {
        uint8_t key = 0u;
        int rc = ZX_KeyPoll (&key);

        if (rc < 0) {
            printf("ERR: key mailbox read failed\r\n");
            return;
        }

        if (rc > 0) {
            ++got;
            if ((key >= 32u) && (key <= 126u)) {
                printf("  [%u] 0x%02X '%c'\r\n",
                       (unsigned)got,
                       (unsigned)key,
                       (char)key);
            } else {
                printf("  [%u] 0x%02X\r\n",
                       (unsigned)got,
                       (unsigned)key);
            }
        } else {
            Delay_Ms (10u);
            waited += 10u;
        }
    }

    if (got == wanted_count) {
        printf("keytest: PASS (%u event%s)\r\n",
               (unsigned)got,
               (got == 1u) ? "" : "s");
    } else {
        printf("keytest: TIMEOUT (%u/%u event%s)\r\n",
               (unsigned)got,
               (unsigned)wanted_count,
               (wanted_count == 1u) ? "" : "s");
    }
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

static void ZX_CommandZ80Select (const char *path) {
    uint8_t selected = 0u;
    char full_path[128];

    if (!ZX_BrowserLoadFiles (path)) {
        return;
    }
    if (!ZX_BrowserRender (path, selected)) {
        printf("ERR: z80select draw failed\r\n");
        return;
    }
    if (s_browser_count == 0u) {
        printf("z80select: no .z80 files\r\n");
        return;
    }

    printf("z80select: Q/A move, O/P page, ENTER run, SPACE exit\r\n");

    for (;;) {
        uint8_t key = 0u;
        int rc = ZX_KeyPoll (&key);

        if (rc < 0) {
            printf("ERR: key mailbox read failed\r\n");
            return;
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
                    return;
                }
            }
            continue;
        }
        if ((key == 'a') || (key == 'A')) {
            if ((uint16_t)selected + 1u < s_browser_count) {
                ++selected;
                if (!ZX_BrowserRender (path, selected)) {
                    printf("ERR: z80select redraw failed\r\n");
                    return;
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
                return;
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
                return;
            }
            continue;
        }
        if (key == ' ') {
            printf("z80select: cancelled\r\n");
            return;
        }
        if (key == '\n') {
            ZX_BrowserBuildPath (full_path, sizeof (full_path), path, s_browser_files[selected]);
            printf("z80select: running %s\r\n", full_path);
            (void)Z80_LoadAndRun (full_path);
            return;
        }
    }
}

static void ZX_PrintHexLine (uint16_t address, const uint8_t *buffer, uint16_t count) {
    uint16_t i;
    printf("%04X: ", (unsigned)address);
    for (i = 0u; i < count; ++i) {
        printf("%02X ", (unsigned)buffer[i]);
    }
    printf("\r\n");
}

static void ZX_CommandDump (uint16_t address, uint16_t length) {
    uint8_t block[16];
    uint16_t remaining = length;

    while (remaining > 0u) {
        uint16_t chunk = (remaining > (uint16_t)sizeof (block)) ? (uint16_t)sizeof (block) : remaining;
        if (!ZX_BusReadBlock (address, block, chunk)) {
            printf("ERR: dump failed at 0x%04X\r\n", (unsigned)address);
            return;
        }

        ZX_PrintHexLine (address, block, chunk);
        address = (uint16_t)(address + chunk);
        remaining = (uint16_t)(remaining - chunk);
    }
}

static void ZX_CommandRead (uint16_t address, uint16_t length) {
    if (length == 1u) {
        uint8_t value = 0u;
        if (!ZX_BusReadBlock (address, &value, 1u)) {
            printf("ERR: rd failed\r\n");
            return;
        }
        printf("RD 0x%04X = 0x%02X\r\n", (unsigned)address, (unsigned)value);
        return;
    }

    ZX_CommandDump (address, length);
}

static void ZX_CommandWrite (uint16_t address, uint8_t value) {
    uint8_t verify = 0u;

    if (!ZX_BusWriteBlock (address, &value, 1u)) {
        printf("ERR: wd failed\r\n");
        return;
    }

    if (!ZX_BusReadBlock (address, &verify, 1u)) {
        printf("WR 0x%04X <= 0x%02X (verify read failed)\r\n", (unsigned)address, (unsigned)value);
        return;
    }

    printf("WR 0x%04X <= 0x%02X", (unsigned)address, (unsigned)value);
    if (verify == value) {
        printf(" OK\r\n");
    } else {
        printf(" MISMATCH read=0x%02X\r\n", (unsigned)verify);
    }
}

static void ZX_CommandFill (uint16_t address, uint8_t value, uint16_t length) {
    uint8_t buffer[16];
    uint16_t remaining = length;
    uint16_t chunk_size;
    uint16_t i;

    if (length == 0u) {
        printf("ERR: fill length must be > 0\r\n");
        return;
    }

    for (i = 0u; i < (uint16_t)sizeof (buffer); ++i) {
        buffer[i] = value;
    }

    while (remaining > 0u) {
        chunk_size = (remaining > (uint16_t)sizeof (buffer)) ? (uint16_t)sizeof (buffer) : remaining;
        if (!ZX_BusWriteBlock (address, buffer, chunk_size)) {
            printf("ERR: fill failed at 0x%04X\r\n", (unsigned)address);
            return;
        }
        address = (uint16_t)(address + chunk_size);
        remaining = (uint16_t)(remaining - chunk_size);
    }

    printf("Filled 0x%04X..0x%04X with 0x%02X (%u bytes)\r\n",
           (unsigned)(address - length),
           (unsigned)(address - 1u),
           (unsigned)value,
           (unsigned)length);
}

static void ZX_CommandSelfTest (uint16_t address) {
    uint8_t backup[ZX_TEST_LEN];
    uint8_t pattern[ZX_TEST_LEN];
    uint8_t verify[ZX_TEST_LEN];
    uint32_t i;
    int ok = 1;

    if ((address < ZX_CART_RAM_BASE) || ((uint32_t)address + ZX_TEST_LEN - 1u > ZX_CART_RAM_LAST)) {
        printf("TEST: addr out of cart RAM range 0x3000..0x3FFF\r\n");
        return;
    }

    for (i = 0u; i < ZX_TEST_LEN; ++i) {
        pattern[i] = (uint8_t)(0xA5u ^ (uint8_t)(i * 17u));
    }

    if (!ZX_CartRamReadBlock (address, backup, ZX_TEST_LEN)) {
        printf("TEST: read backup failed\r\n");
        return;
    }

    if (!ZX_CartRamWriteBlock (address, pattern, ZX_TEST_LEN)) {
        printf("TEST: write pattern failed\r\n");
        return;
    }

    if (!ZX_CartRamReadBlock (address, verify, ZX_TEST_LEN)) {
        printf("TEST: read verify failed\r\n");
        (void)ZX_CartRamWriteBlock (address, backup, ZX_TEST_LEN);
        return;
    }

    for (i = 0u; i < ZX_TEST_LEN; ++i) {
        if (verify[i] != pattern[i]) {
            ok = 0;
            printf("TEST: mismatch @0x%04X wr=0x%02X rd=0x%02X\r\n",
                   (unsigned)(address + (uint16_t)i), (unsigned)pattern[i], (unsigned)verify[i]);
            break;
        }
    }

    (void)ZX_CartRamWriteBlock (address, backup, ZX_TEST_LEN);

    if (ok) {
        printf("TEST PASS: 0x%04X..0x%04X write/read/restore\r\n",
               (unsigned)address, (unsigned)(address + ZX_TEST_LEN - 1u));
    }
}

/* Probe a fixed set of landmark addresses across the ZX address space.
   For each, write 0xA5, read back, restore original.
   Report pass/fail, the readback value, and bus acquisition time. */
#define ZX_NMIWRITE_TIMEOUT_MS  200u
#define ZX_NMITEST_LEN          16u

static void ZX_CommandNmiWrite (uint16_t address, uint8_t value) {
    uint8_t buf[1];
    buf[0] = value;

    if (!ZX_NmiWriteBlock (address, buf, 1u, ZX_NMIWRITE_TIMEOUT_MS)) {
        printf("ERR: NMI write timeout at 0x%04X\r\n", (unsigned)address);
        return;
    }

    /* Verify by reading back over BUSREQ */
    uint8_t rb = 0u;
    if (!ZX_BusReadBlock (address, &rb, 1u)) {
        printf("NMI WR 0x%04X <= 0x%02X (verify read failed)\r\n",
               (unsigned)address, (unsigned)value);
        return;
    }

    printf("NMI WR 0x%04X <= 0x%02X", (unsigned)address, (unsigned)value);
    if (rb == value) {
        printf(" OK\r\n");
    } else {
        printf(" MISMATCH read=0x%02X\r\n", (unsigned)rb);
    }
}

static void ZX_CommandNmiFill (uint16_t address, uint8_t value, uint16_t length) {
    uint8_t buffer[ZX_NMI_WCMD_CHUNK_EXPOSED];
    uint16_t remaining = length;
    uint16_t chunk;
    uint16_t i;

    if (length == 0u) {
        printf("ERR: fill length must be > 0\r\n");
        return;
    }

    for (i = 0u; i < (uint16_t)sizeof (buffer); ++i) {
        buffer[i] = value;
    }

    while (remaining > 0u) {
        chunk = (remaining > (uint16_t)sizeof (buffer)) ? (uint16_t)sizeof (buffer) : remaining;
        if (!ZX_NmiWriteBlock (address, buffer, chunk, ZX_NMIWRITE_TIMEOUT_MS)) {
            printf("ERR: NMI fill timeout at 0x%04X\r\n", (unsigned)address);
            return;
        }
        address = (uint16_t)(address + chunk);
        remaining = (uint16_t)(remaining - chunk);
    }

    printf("NMI Filled 0x%04X..0x%04X with 0x%02X (%u bytes)\r\n",
           (unsigned)(address - length),
           (unsigned)(address - 1u),
           (unsigned)value,
           (unsigned)length);
}

static void ZX_CommandNmiTest (uint16_t address) {
    uint8_t backup[ZX_NMITEST_LEN];
    uint8_t pattern[ZX_NMITEST_LEN];
    uint8_t verify[ZX_NMITEST_LEN];
    uint16_t i;
    int ok = 1;

    printf("NMITEST 0x%04X..0x%04X\r\n",
           (unsigned)address, (unsigned)(address + ZX_NMITEST_LEN - 1u));

    /* Backup original contents via BUSREQ */
    if (!ZX_BusReadBlock (address, backup, ZX_NMITEST_LEN)) {
        printf("ERR: backup read failed (BUSREQ)\r\n");
        return;
    }
    printf("  Backup OK");
    for (i = 0u; i < ZX_NMITEST_LEN; ++i) { printf(" %02X", (unsigned)backup[i]); }
    printf("\r\n");

    /* Build incrementing test pattern */
    for (i = 0u; i < ZX_NMITEST_LEN; ++i) {
        pattern[i] = (uint8_t)(0xA5u ^ (uint8_t)(i * 17u));
    }

    /* Write pattern via NMI (ZX CPU does the actual write, bypasses ULA contention) */
    if (!ZX_NmiWriteBlock (address, pattern, ZX_NMITEST_LEN, ZX_NMIWRITE_TIMEOUT_MS)) {
        printf("ERR: NMI write timed out\r\n");
        return;
    }
    printf("  NMI write OK\r\n");

    /* Read back via BUSREQ */
    if (!ZX_BusReadBlock (address, verify, ZX_NMITEST_LEN)) {
        printf("ERR: verify read failed (BUSREQ)\r\n");
        (void)ZX_NmiWriteBlock (address, backup, ZX_NMITEST_LEN, ZX_NMIWRITE_TIMEOUT_MS);
        return;
    }

    printf("  Verify:");
    for (i = 0u; i < ZX_NMITEST_LEN; ++i) {
        if (verify[i] != pattern[i]) {
            printf(" [%02X!=%02X@%04X]",
                   (unsigned)verify[i], (unsigned)pattern[i],
                   (unsigned)(address + i));
            ok = 0;
        } else {
            printf(" %02X", (unsigned)verify[i]);
        }
    }
    printf("\r\n");

    /* Restore original */
    if (!ZX_NmiWriteBlock (address, backup, ZX_NMITEST_LEN, ZX_NMIWRITE_TIMEOUT_MS)) {
        printf("WARN: restore NMI write timed out\r\n");
    }

    if (ok) {
        printf("NMITEST PASS\r\n");
    } else {
        printf("NMITEST FAIL (check ULA contention or NMI handler active)\r\n");
    }
}

static void ZX_CommandNmiDiag (void) {
    static const uint16_t probe_addrs[] = {
        0x4000u, /* screen RAM start (ULA contended) */
        0x4001u,
        0x5C00u, /* ZX system variables (contended) */
        0x6000u, /* free RAM (contended) */
        0x7000u, /* free RAM (contended) */
        0x7FFEu, /* top of contended bank */
        0x8000u, /* free RAM (uncontended) */
        0x8001u,
        0x9000u, /* free RAM (uncontended) */
        0xA000u,
        0xC000u,
        0xE000u,
        0xFFFEu, /* near top of RAM */
    };
    const uint8_t n = (uint8_t)(sizeof (probe_addrs) / sizeof (probe_addrs[0]));
    const uint8_t TEST_VAL = 0xA5u;
    uint8_t backup;
    uint8_t rb_nmi;
    uint8_t rb_bus;
    uint8_t i;

    printf("NMI write-path diagnostic vs BUSREQ\r\n");
    printf("(ZX screen drawing suspended during test)\r\n");
    printf("Addr   NMI-Wr  NMI-Rd  BUS-Rd  NMI     BUS\r\n");
    printf("----   ------  ------  ------  ---     ---\r\n");

    ZX_CartDrawSuspend();
    Delay_Ms (50u);  /* wait for any in-progress draw_bridge_screen to finish */

    for (i = 0u; i < n; ++i) {
        uint16_t addr = probe_addrs[i];
        int nmi_ok;
        int bus_ok;

        /* Save original */
        if (!ZX_BusReadBlock (addr, &backup, 1u)) {
            printf("%04X   ??      ??      ??      ERR(bus acq)\r\n", (unsigned)addr);
            continue;
        }

        /* NMI write path */
        nmi_ok = ZX_NmiWriteBlock (addr, &TEST_VAL, 1u, ZX_NMIWRITE_TIMEOUT_MS);
        if (!nmi_ok) {
            printf("%04X   A5      --      --      TIMEOUT     --\r\n", (unsigned)addr);
            /* Try to restore anyway */
            (void)ZX_NmiWriteBlock (addr, &backup, 1u, ZX_NMIWRITE_TIMEOUT_MS);
            continue;
        }
        (void)ZX_BusReadBlock (addr, &rb_nmi, 1u);

        /* BUSREQ path (back-to-back so same location) */
        int bus_result = ZX_BusWriteReadVerify (addr, TEST_VAL, &rb_bus);

        /* Restore original via NMI (works on contended) */
        (void)ZX_NmiWriteBlock (addr, &backup, 1u, ZX_NMIWRITE_TIMEOUT_MS);

        nmi_ok = (rb_nmi == TEST_VAL);
        bus_ok = (bus_result == 1);

        /* For screen pixel RAM (0x4000-0x57FF) the ZX program's main loop calls
           draw_bridge_screen() which memsets 0x4000 to zero.  If the NMI write
           lands during a draw_bridge_screen() call the readback via BUSREQ may
           see 0x00 rather than the written value — not a write failure, just a
           race.  Flag it separately so it doesn't look like a bus problem. */
        if (!nmi_ok && (rb_nmi == 0x00u) && (addr < 0x5800u)) {
            printf("%04X   A5      %02X      %02X      OK(race) %-8s\r\n",
                   (unsigned)addr,
                   (unsigned)rb_nmi,
                   (unsigned)rb_bus,
                   (bus_result < 0) ? "ACQFAIL" : (bus_ok ? "OK" : "MISMATCH"));
        } else {
            printf("%04X   A5      %02X      %02X      %-8s%-8s\r\n",
                   (unsigned)addr,
                   (unsigned)rb_nmi,
                   (unsigned)rb_bus,
                   nmi_ok ? "OK" : "MISMATCH",
                   (bus_result < 0) ? "ACQFAIL" : (bus_ok ? "OK" : "MISMATCH"));
        }
    }

    ZX_CartDrawResume();
}

static void ZX_CommandBusDiag (void) {
    static const uint16_t probe_addrs[] = {
        0x4000u, /* screen RAM start (ULA contended) */
        0x4001u,
        0x5C00u, /* ZX system variables (contended) */
        0x6000u, /* free RAM (contended) */
        0x7000u, /* free RAM (contended) */
        0x7FFEu, /* top of contended bank */
        0x8000u, /* free RAM (uncontended) */
        0x8001u,
        0x9000u, /* free RAM (uncontended) */
        0xA000u,
        0xC000u,
        0xE000u,
        0xFFFEu, /* near top of RAM */
    };
    static const uint8_t TRIALS = 8u;
    uint32_t acq_cycles;
    uint8_t i;
    const uint8_t n = (uint8_t)(sizeof (probe_addrs) / sizeof (probe_addrs[0]));

    /* First report bus acquisition speed */
    if (ZX_BusAcquireDbg (&acq_cycles)) {
        printf("BUSREQ acquired in %lu clock waits\r\n", (unsigned long)acq_cycles);
        ZX_BusReleaseDbg();
    } else {
        printf("BUSREQ FAILED to acquire bus!\r\n");
        return;
    }

    printf("Addr  Pass/%-2u  Fail values (ULA refresh collision pattern)\r\n", (unsigned)TRIALS);
    printf("----  -------  -----------------------------------------------\r\n");

    for (i = 0u; i < n; ++i) {
        uint16_t addr = probe_addrs[i];
        uint8_t pass = 0u;
        uint8_t fail = 0u;
        uint8_t fail_vals[TRIALS];
        uint8_t fail_count = 0u;
        uint8_t t;

        for (t = 0u; t < TRIALS; ++t) {
            uint8_t rb = 0u;
            int result = ZX_BusWriteReadVerify (addr, 0xA5u, &rb);
            if (result == 1) {
                ++pass;
            } else if (result == 0) {
                ++fail;
                if (fail_count < TRIALS) {
                    fail_vals[fail_count++] = rb;
                }
            } else {
                printf("%04X  BUS ACQUIRE FAIL\r\n", (unsigned)addr);
                goto next_addr;
            }
        }

        if (fail == 0u) {
            printf("%04X  %u/%-2u     OK\r\n", (unsigned)addr, (unsigned)pass, (unsigned)TRIALS);
        } else if (pass == 0u) {
            /* All trials failed - show first bad value and bit pattern */
            uint8_t diff = (uint8_t)(0xA5u ^ fail_vals[0]);
            printf("%04X  0/%-2u     rd=%02X diff=%02X (bits",
                   (unsigned)addr, (unsigned)TRIALS,
                   (unsigned)fail_vals[0], (unsigned)diff);
            uint8_t b;
            for (b = 0u; b < 8u; ++b) {
                if ((diff >> b) & 1u) { printf(" %u", (unsigned)b); }
            }
            printf(") CONTENDED\r\n");
        } else {
            /* Intermittent - ULA refresh collision */
            uint8_t diff = (uint8_t)(0xA5u ^ fail_vals[0]);
            printf("%04X  %u/%-2u     rd=%02X diff=%02X (bits",
                   (unsigned)addr, (unsigned)pass, (unsigned)TRIALS,
                   (unsigned)fail_vals[0], (unsigned)diff);
            uint8_t b;
            for (b = 0u; b < 8u; ++b) {
                if ((diff >> b) & 1u) { printf(" %u", (unsigned)b); }
            }
            printf(") INTERMITTENT\r\n");
        }
        next_addr:;
    }
}

/* Write test_value then read back N times at same address to detect intermittent
   contention. Reports pass count, fail count, and any unique readback values seen. */
static void ZX_CommandBusRwTest (uint16_t address, uint8_t iterations) {
    uint8_t pass = 0u;
    uint8_t fail = 0u;
    uint8_t seen[8];
    uint8_t seen_count = 0u;
    uint8_t rb;
    int result;
    uint8_t i;
    uint8_t j;
    uint8_t found;

    printf("BUSRWTEST 0x%04X x%u with 0xA5\r\n", (unsigned)address, (unsigned)iterations);

    for (i = 0u; i < iterations; ++i) {
        result = ZX_BusWriteReadVerify (address, 0xA5u, &rb);
        if (result == 1) {
            ++pass;
        } else {
            ++fail;
            /* Track unique bad readback values */
            if (seen_count < (uint8_t)(sizeof (seen) / sizeof (seen[0]))) {
                found = 0u;
                for (j = 0u; j < seen_count; ++j) {
                    if (seen[j] == rb) { found = 1u; break; }
                }
                if (!found) {
                    seen[seen_count++] = rb;
                }
            }
        }
    }

    printf("Pass: %u  Fail: %u  (%.0u%%)\r\n",
           (unsigned)pass, (unsigned)fail,
           iterations > 0u ? (unsigned)(((uint32_t)pass * 100u) / (uint32_t)iterations) : 0u);

    if (fail > 0u) {
        printf("Bad readback values seen:");
        for (j = 0u; j < seen_count; ++j) {
            printf(" 0x%02X", (unsigned)seen[j]);
        }
        printf("\r\n");

        if (address >= 0x4000u && address <= 0x7FFFu) {
            printf("NOTE: 0x%04X is in ULA-contended RAM (0x4000-0x7FFF).\r\n", (unsigned)address);
            printf("      Writes here may fail due to ULA video bus contention.\r\n");
            printf("      Try 0x8000-0xBFFF for reliable BUSREQ writes.\r\n");
        }
    }
}

/* Animated graphics test: suspends zxprog's screen drawing, paints a series
   of full-screen patterns directly into ZX video RAM via NMI writes (which
   bypass ULA contention), then resumes zxprog so it redraws the bridge UI.

   The pattern generator works in ZX screen-address order: the unusual ZX
   layout encodes y as bits [12:11]=third, [10:8]=row-within-char,
   [7:5]=char-row-within-third; x_byte is bits [4:0]. */

static uint8_t ZX_GfxPixelByte (uint8_t frame, uint8_t y, uint8_t x_byte) {
    switch (frame) {
        case 0u:
            /* Vertical 8-pixel stripes */
            return (uint8_t)((x_byte & 1u) ? 0xFFu : 0x00u);
        case 1u:
            /* Horizontal 8-pixel stripes */
            return (uint8_t)((y & 0x08u) ? 0xFFu : 0x00u);
        case 2u: {
            /* 8x8 checkerboard */
            uint8_t cell = (uint8_t)((x_byte ^ (y >> 3)) & 1u);
            return cell ? 0xFFu : 0x00u;
        }
        case 3u:
            /* Diagonal lines */
            return (uint8_t)(0x80u >> ((y + x_byte) & 7u));
        case 4u:
        default: {
            /* Border frame + diagonal cross */
            uint8_t out = 0x00u;
            if ((y < 4u) || (y >= 188u)) {
                out = 0xFFu;
            } else if (x_byte == 0u) {
                out = 0x01u;
            } else if (x_byte == 31u) {
                out = 0x80u;
            } else {
                /* Cross: y == x*6 (approx diag) and y == 192 - x*6 */
                uint8_t xb6 = (uint8_t)(x_byte * 6u);
                if ((y == xb6) || (y == (uint8_t)(192u - xb6))) {
                    out = 0xFFu;
                }
            }
            return out;
        }
    }
}

static void ZX_GfxBuildPixels (uint8_t frame) {
    uint16_t addr;
    for (addr = 0u; addr < ZX_SCREEN_PIXELS_LEN; ++addr) {
        uint8_t y = (uint8_t)(((addr >> 11) & 0x03u) << 6);
        y |= (uint8_t)(((addr >> 5) & 0x07u) << 3);
        y |= (uint8_t)((addr >> 8) & 0x07u);
        uint8_t x_byte = (uint8_t)(addr & 0x1Fu);
        s_gfx_pixels[addr] = ZX_GfxPixelByte (frame, y, x_byte);
    }
}

static void ZX_GfxBuildAttrs (uint8_t frame) {
    static const uint8_t fg_table[ZX_GFXTEST_FRAMES] = {7u, 6u, 0u, 5u, 3u};
    static const uint8_t bg_table[ZX_GFXTEST_FRAMES] = {1u, 2u, 7u, 0u, 4u};
    uint8_t fg = fg_table[frame % ZX_GFXTEST_FRAMES];
    uint8_t bg = bg_table[frame % ZX_GFXTEST_FRAMES];
    uint16_t i;
    for (i = 0u; i < ZX_SCREEN_ATTRS_LEN; ++i) {
        uint8_t row = (uint8_t)(i >> 5);
        uint8_t col = (uint8_t)(i & 0x1Fu);
        uint8_t ink = fg;
        uint8_t paper = bg;
        if (((row + col) & 1u) == 0u) {
            uint8_t t = ink; ink = paper; paper = t;
        }
        s_gfx_attrs[i] = (uint8_t)(0x40u | ((paper & 0x07u) << 3) | (ink & 0x07u));
    }
}

static void ZX_CommandGfxTest (void) {
    uint8_t frame;

    printf("GFXTEST: suspending zxprog draw, animating %u frames\r\n",
           (unsigned)ZX_GFXTEST_FRAMES);

    ZX_CartDrawSuspend();
    /* Wait for any in-progress draw_bridge_screen() (memset of 0x4000) to finish. */
    Delay_Ms (50u);

    for (frame = 0u; frame < ZX_GFXTEST_FRAMES; ++frame) {
        ZX_GfxBuildPixels (frame);
        ZX_GfxBuildAttrs (frame);

        if (!ZX_NmiWriteBlock (ZX_SCREEN_PIXELS_ADDR, s_gfx_pixels,
                               ZX_SCREEN_PIXELS_LEN, ZX_GFXTEST_NMI_TO)) {
            printf("GFXTEST: pixel write timeout (frame %u)\r\n", (unsigned)frame);
            ZX_CartDrawResume();
            return;
        }
        if (!ZX_NmiWriteBlock (ZX_SCREEN_ATTRS_ADDR, s_gfx_attrs,
                               ZX_SCREEN_ATTRS_LEN, ZX_GFXTEST_NMI_TO)) {
            printf("GFXTEST: attr write timeout (frame %u)\r\n", (unsigned)frame);
            ZX_CartDrawResume();
            return;
        }

        printf("GFXTEST: frame %u painted\r\n", (unsigned)frame);
        Delay_Ms (ZX_GFXTEST_FRAME_MS);
    }

    /* Bump bridge sequence so zxprog's main loop sets bridge_dirty and
       redraws its UI as soon as we clear the suspend flag. Existing text/len
       remain in cart RAM, so the redraw repaints the previous content. */
    ++s_bridge_seq;
    (void)ZX_CartRamWriteBlock (ZX_BRIDGE_SEQ_ADDR, &s_bridge_seq, 1u);

    ZX_CartDrawResume();
    printf("GFXTEST: complete, zxprog resumed\r\n");
}

/* ---------------- Full RAM walk test ---------------------------------- */

/* Pattern generator. Produces deterministic byte for (pattern, abs_addr).
   Patterns:
     0: 0x00       1: 0xFF       2: 0xAA       3: 0x55
     4: addr_lo    5: ~addr_lo   6: LFSR8 pseudo-random (seed 0xACE1) */
static uint8_t ZX_RamTestByte (uint8_t pattern, uint16_t addr) {
    switch (pattern) {
        case 0u: return 0x00u;
        case 1u: return 0xFFu;
        case 2u: return 0xAAu;
        case 3u: return 0x55u;
        case 4u: return (uint8_t)(addr & 0xFFu);
        case 5u: return (uint8_t)~((uint8_t)(addr & 0xFFu));
        case 6u:
        default: {
            /* Fast closed-form pseudo-random mix, deterministic per-address.
               Avoids per-byte loops that made full-range tests look hung. */
            uint16_t x = (uint16_t)(addr * 0x9E37u);
            x ^= (uint16_t)(addr >> 3);
            x ^= (uint16_t)(x >> 7);
            x = (uint16_t)(x * 0x85EBu);
            x ^= (uint16_t)(x >> 4);
            return (uint8_t)((x ^ 0x5Au) & 0xFFu);
        }
    }
}

static const char *ZX_RamTestPatternName (uint8_t pattern) {
    switch (pattern) {
        case 0u: return "0x00";
        case 1u: return "0xFF";
        case 2u: return "0xAA";
        case 3u: return "0x55";
        case 4u: return "addr_lo";
        case 5u: return "~addr_lo";
        case 6u: default: return "mix";
    }
}

#define ZX_RAMTEST_NUM_PATTERNS 7u

/* Test a [base..base+len-1] range with all patterns. zxprog draw must already
   be suspended by caller. Uses s_gfx_pixels as a chunk scratch buffer. */
static int ZX_RamTestRange (uint16_t base, uint32_t len) {
    uint8_t pattern;
    uint8_t total_ok = 1;

    for (pattern = 0u; pattern < ZX_RAMTEST_NUM_PATTERNS; ++pattern) {
        uint32_t offset;
        uint32_t errors = 0u;
        uint8_t shown = 0u;

        printf("  Pattern %u (%s):", (unsigned)pattern,
               ZX_RamTestPatternName (pattern));

        /* Phase 1: NMI-write the whole range, chunk by chunk. */
        offset = 0u;
        while (offset < len) {
            uint32_t remaining = len - offset;
            uint16_t chunk = (remaining > ZX_RAMTEST_CHUNK)
                                 ? (uint16_t)ZX_RAMTEST_CHUNK
                                 : (uint16_t)remaining;
            uint16_t i;
            uint16_t addr;

            for (i = 0u; i < chunk; ++i) {
                addr = (uint16_t)(base + offset + i);
                s_gfx_pixels[i] = ZX_RamTestByte (pattern, addr);
            }

            if (!ZX_NmiWriteBlock ((uint16_t)(base + offset),
                                   s_gfx_pixels, chunk,
                                   ZX_RAMTEST_NMI_TO)) {
                printf(" WRITE TIMEOUT @0x%04X\r\n",
                       (unsigned)(base + offset));
                return 0;
            }
            offset += chunk;
        }

        /* Phase 2: BUSREQ-read whole range and compare. */
        offset = 0u;
        while (offset < len) {
            uint32_t remaining = len - offset;
            uint16_t chunk = (remaining > ZX_RAMTEST_CHUNK)
                                 ? (uint16_t)ZX_RAMTEST_CHUNK
                                 : (uint16_t)remaining;
            uint16_t i;
            uint16_t addr;

            if (!ZX_BusReadBlock ((uint16_t)(base + offset),
                                  s_gfx_pixels, chunk)) {
                printf(" READ FAIL @0x%04X\r\n",
                       (unsigned)(base + offset));
                return 0;
            }

            for (i = 0u; i < chunk; ++i) {
                addr = (uint16_t)(base + offset + i);
                uint8_t expected = ZX_RamTestByte (pattern, addr);
                if (s_gfx_pixels[i] != expected) {
                    if (shown < ZX_RAMTEST_MAX_ERRORS) {
                        if (shown == 0u) { printf("\r\n"); }
                        printf("    @0x%04X exp=%02X got=%02X diff=%02X\r\n",
                               (unsigned)addr,
                               (unsigned)expected,
                               (unsigned)s_gfx_pixels[i],
                               (unsigned)(expected ^ s_gfx_pixels[i]));
                        ++shown;
                    }
                    ++errors;
                }
            }

            offset += chunk;
        }

        if (errors == 0u) {
            printf(" OK\r\n");
        } else {
            printf("    -> %lu error(s)%s\r\n",
                   (unsigned long)errors,
                   (errors > ZX_RAMTEST_MAX_ERRORS) ? " (truncated)" : "");
            total_ok = 0;
        }
    }

    return total_ok;
}

static void ZX_CommandRamTest (uint16_t base, uint32_t len) {
    int ok;

    if (len == 0u) {
        printf("ERR: ramtest length must be > 0\r\n");
        return;
    }
    if ((uint32_t)base < 0x4000u) {
        printf("ERR: ramtest base must be >= 0x4000 (ZX RAM)\r\n");
        return;
    }
    if ((uint32_t)base + len > 0x10000u) {
        printf("ERR: ramtest range exceeds 0xFFFF\r\n");
        return;
    }

    /* Warn if range overlaps zxprog BSS/stack area (0x8800-0x8AFF). Caller's
       choice — overwriting will likely crash zxprog and require reset. */
    if ((base < 0x8B00u) && ((uint32_t)base + len > 0x8800u)) {
        printf("WARN: range overlaps zxprog BSS/stack 0x8800-0x8AFF — zxprog will crash\r\n");
    }

    printf("RAMTEST 0x%04X..0x%04X (%lu bytes), %u patterns\r\n",
           (unsigned)base,
           (unsigned)(base + len - 1u),
           (unsigned long)len,
           (unsigned)ZX_RAMTEST_NUM_PATTERNS);

    ZX_CartDrawSuspend();
    Delay_Ms (50u);

    ok = ZX_RamTestRange (base, len);

    /* Restore: write 0x00 everywhere we touched so we don't leave junk
       (especially relevant for screen RAM / system var areas). */
    {
        uint32_t offset = 0u;
        uint16_t i;
        for (i = 0u; i < ZX_RAMTEST_CHUNK; ++i) { s_gfx_pixels[i] = 0x00u; }
        while (offset < len) {
            uint32_t remaining = len - offset;
            uint16_t chunk = (remaining > ZX_RAMTEST_CHUNK)
                                 ? (uint16_t)ZX_RAMTEST_CHUNK
                                 : (uint16_t)remaining;
            (void)ZX_NmiWriteBlock ((uint16_t)(base + offset),
                                    s_gfx_pixels, chunk,
                                    ZX_RAMTEST_NMI_TO);
            offset += chunk;
        }
    }

    /* Force zxprog to repaint its UI on resume. */
    ++s_bridge_seq;
    (void)ZX_CartRamWriteBlock (ZX_BRIDGE_SEQ_ADDR, &s_bridge_seq, 1u);
    ZX_CartDrawResume();

    printf("RAMTEST %s\r\n", ok ? "PASS" : "FAIL");
}

static void ZX_CommandLs (const char *path) {
    DIR dir;
    FILINFO fno;
    FRESULT fr;
    const char *target = (path != NULL && path[0] != '\0') ? path : "/";
    int count = 0;

    fr = f_opendir (&dir, target);
    if (fr != FR_OK) {
        printf("ls: cannot open '%s' (fr=%d)\r\n", target, (int)fr);
        return;
    }

    printf("Directory of %s\r\n", target);
    while (1) {
        fr = f_readdir (&dir, &fno);
        if (fr != FR_OK || fno.fname[0] == '\0') {
            break;
        }
        if (fno.fattrib & AM_DIR) {
            printf("  <DIR>            %s\r\n", fno.fname);
        } else {
            printf("  %10lu     %s\r\n", (unsigned long)fno.fsize, fno.fname);
        }
        ++count;
    }
    f_closedir (&dir);
    printf("(%d entries)\r\n", count);
}

static void ZX_CommandTapInfo (const char *path) {
    if ((path == NULL) || (path[0] == '\0')) {
        printf("Usage: tapinfo <path>\r\n");
        return;
    }
    (void)Tap_Info (path);
}

static void ZX_CommandTapRun (const char *path, const char *start_arg) {
    uint16_t start = 0u;
    uint32_t parsed = 0u;

    if ((path == NULL) || (path[0] == '\0')) {
        printf("Usage: taprun <path> [start_hex]\r\n");
        return;
    }
    if (start_arg != NULL) {
        if (!ZX_ParseAddressHex (start_arg, &parsed)) {
            printf("Bad start address (use hex, e.g. 8000)\r\n");
            return;
        }
        start = (uint16_t)parsed;
    }
    (void)Tap_LoadAndRun (path, start);
}

static void ZX_CommandRomcs (const char *arg) {
    if (arg == NULL) {
        printf("ROMCS state: %s\r\n",
               ZX_RomcsIsReleased() ? "RELEASED (Spectrum ROM)" : "ASSERTED (cart ROM)");
        return;
    }
    if (ZX_StrIeq (arg, "off") || ZX_StrIeq (arg, "release")) {
        ZX_RomcsRelease();
        printf("ROMCS released — internal Spectrum ROM active\r\n");
    } else if (ZX_StrIeq (arg, "on") || ZX_StrIeq (arg, "assert")) {
        ZX_RomcsAssert();
        printf("ROMCS asserted — cart ROM active\r\n");
    } else {
        printf("Usage: romcs <on|off>\r\n");
    }
}

static void ZX_ExecuteCommand (char *line) {
    char *cmd = strtok (line, " \t");
    char *a0;
    char *a1;
    uint32_t v0;
    uint32_t v1;

    if (cmd == NULL) {
        return;
    }

    if (ZX_StrIeq (cmd, "help") || ZX_StrIeq (cmd, "?")) {
        ZX_PrintHelp();
        return;
    }

    if (ZX_StrIeq (cmd, "nmi")) {
        ZX_TriggerNMI();
        printf("NMI pulse sent\r\n");
        return;
    }

    if (ZX_StrIeq (cmd, "zxmsg")) {
        a0 = strtok (NULL, " \t");
        ZX_CommandBridgeText (a0);
        return;
    }

    if (ZX_StrIeq (cmd, "zxttyinit")) {
        ZX_CommandTermInit();
        return;
    }

    if (ZX_StrIeq (cmd, "zxtty")) {
        a0 = strtok (NULL, " \t");
        ZX_CommandTermWrite (a0);
        return;
    }

    if (ZX_StrIeq (cmd, "zxttytest")) {
        ZX_CommandTermTest();
        return;
    }

    if (ZX_StrIeq (cmd, "keyread")) {
        ZX_CommandKeyRead();
        return;
    }

    if (ZX_StrIeq (cmd, "keytest")) {
        uint32_t count = 1u;
        uint32_t timeout_ms = 3000u;

        a0 = strtok (NULL, " \t");
        a1 = strtok (NULL, " \t");
        if (a0 != NULL) {
            if (!ZX_ParseU32 (a0, &count) || (count == 0u) || (count > 32u)) {
                printf("Usage: keytest [count 1..32] [timeout_ms]\r\n");
                return;
            }
        }
        if (a1 != NULL) {
            if (!ZX_ParseU32 (a1, &timeout_ms) || (timeout_ms == 0u) || (timeout_ms > 60000u)) {
                printf("Usage: keytest [count 1..32] [timeout_ms]\r\n");
                return;
            }
        }

        ZX_CommandKeyTest ((uint8_t)count, timeout_ms);
        return;
    }

    if (ZX_StrIeq (cmd, "zxview")) {
        a0 = strtok (NULL, " \t");
        a1 = strtok (NULL, " \t");

        if ((a0 != NULL) && ZX_StrIeq (a0, "off")) {
            ZX_CommandViewOff();
            return;
        }

        if (!ZX_ParseAddressHex (a0, &v0)) {
            printf("Usage: zxview <addr> [len] | zxview off\r\n");
            return;
        }

        if (a1 == NULL) {
            v1 = ZX_VIEW_MAX_BYTES;
        } else if (!ZX_ParseU32 (a1, &v1) || (v1 == 0u)) {
            printf("Usage: zxview <addr> [len] | zxview off\r\n");
            return;
        }

        if (v1 > ZX_VIEW_MAX_BYTES) {
            v1 = ZX_VIEW_MAX_BYTES;
        }

        ZX_CommandView ((uint16_t)v0, (uint8_t)v1);
        return;
    }

    if (ZX_StrIeq (cmd, "dump")) {
        a0 = strtok (NULL, " \t");
        a1 = strtok (NULL, " \t");
        if (!ZX_ParseAddressHex (a0, &v0) || !ZX_ParseU32 (a1, &v1) || (v1 == 0u)) {
            printf("Usage: dump <addr> <len>\r\n");
            return;
        }
        if (v1 > ZX_DUMP_MAX_LEN) {
            v1 = ZX_DUMP_MAX_LEN;
        }
        ZX_CommandDump ((uint16_t)v0, (uint16_t)v1);
        return;
    }

    if (ZX_StrIeq (cmd, "rd")) {
        a0 = strtok (NULL, " \t");
        a1 = strtok (NULL, " \t");
        if (!ZX_ParseAddressHex (a0, &v0)) {
            printf("Usage: rd <addr> [len]\r\n");
            return;
        }
        if (a1 != NULL) {
            if (!ZX_ParseU32 (a1, &v1) || (v1 == 0u)) {
                printf("Usage: rd <addr> [len]\r\n");
                return;
            }
            if (v1 > ZX_DUMP_MAX_LEN) {
                v1 = ZX_DUMP_MAX_LEN;
            }
            ZX_CommandRead ((uint16_t)v0, (uint16_t)v1);
        } else {
            ZX_CommandRead ((uint16_t)v0, 1u);
        }
        return;
    }

    if (ZX_StrIeq (cmd, "wd")) {
        a0 = strtok (NULL, " \t");
        a1 = strtok (NULL, " \t");
        if (!ZX_ParseAddressHex (a0, &v0)) {
            printf("Usage: wd <addr> <byte>\r\n");
            return;
        }
        if (!ZX_ParseAddressHex (a1, &v1)) {
            if (!ZX_ParseU32 (a1, &v1)) {
                printf("Usage: wd <addr> <byte>\r\n");
                return;
            }
        }
        ZX_CommandWrite ((uint16_t)v0, (uint8_t)v1);
        return;
    }

    if (ZX_StrIeq (cmd, "wfill")) {
        a0 = strtok (NULL, " \t");
        char *a2 = strtok (NULL, " \t");
        a1 = strtok (NULL, " \t");
        if (!ZX_ParseAddressHex (a0, &v0) || a2 == NULL || a1 == NULL) {
            printf("Usage: wfill <addr> <byte> <len>\r\n");
            return;
        }
        if (!ZX_ParseAddressHex (a2, &v1)) {
            if (!ZX_ParseU32 (a2, &v1)) {
                printf("Usage: wfill <addr> <byte> <len>\r\n");
                return;
            }
        }
        uint32_t len = 0u;
        if (!ZX_ParseU32 (a1, &len) || (len == 0u)) {
            printf("Usage: wfill <addr> <byte> <len>\r\n");
            return;
        }
        if (len > 0x1000u) {
            printf("ERR: max fill length is 0x1000 (4096)\r\n");
            return;
        }
        ZX_CommandFill ((uint16_t)v0, (uint8_t)v1, (uint16_t)len);
        return;
    }

    if (ZX_StrIeq (cmd, "wram")) {
        a0 = strtok (NULL, " \t");
        a1 = strtok (NULL, " \t");
        if (!ZX_ParseAddressHex (a0, &v0)) {
            printf("Usage: wram <addr> <byte>\r\n");
            return;
        }
        if (!ZX_ParseAddressHex (a1, &v1)) {
            if (!ZX_ParseU32 (a1, &v1)) {
                printf("Usage: wram <addr> <byte>\r\n");
                return;
            }
        }
        ZX_CommandNmiWrite ((uint16_t)v0, (uint8_t)v1);
        return;
    }

    if (ZX_StrIeq (cmd, "wfillram")) {
        a0 = strtok (NULL, " \t");
        char *a2b = strtok (NULL, " \t");
        a1 = strtok (NULL, " \t");
        if (!ZX_ParseAddressHex (a0, &v0) || a2b == NULL || a1 == NULL) {
            printf("Usage: wfillram <addr> <byte> <len>\r\n");
            return;
        }
        if (!ZX_ParseAddressHex (a2b, &v1)) {
            if (!ZX_ParseU32 (a2b, &v1)) {
                printf("Usage: wfillram <addr> <byte> <len>\r\n");
                return;
            }
        }
        uint32_t lenr = 0u;
        if (!ZX_ParseU32 (a1, &lenr) || (lenr == 0u)) {
            printf("Usage: wfillram <addr> <byte> <len>\r\n");
            return;
        }
        if (lenr > 0x4000u) {
            printf("ERR: max NMI fill length is 0x4000 (16384)\r\n");
            return;
        }
        ZX_CommandNmiFill ((uint16_t)v0, (uint8_t)v1, (uint16_t)lenr);
        return;
    }

    if (ZX_StrIeq (cmd, "nmitest")) {
        a0 = strtok (NULL, " \t");
        if (!ZX_ParseAddressHex (a0, &v0)) {
            printf("Usage: nmitest <addr>\r\n");
            return;
        }
        ZX_CommandNmiTest ((uint16_t)v0);
        return;
    }

    if (ZX_StrIeq (cmd, "test")) {
        a0 = strtok (NULL, " \t");
        if (a0 == NULL) {
            v0 = ZX_CART_RAM_BASE;
        } else if (!ZX_ParseAddressHex (a0, &v0)) {
            printf("Usage: test [addr]\r\n");
            return;
        }
        ZX_CommandSelfTest ((uint16_t)v0);
        return;
    }

    if (ZX_StrIeq (cmd, "busdiag")) {
        ZX_CommandBusDiag();
        return;
    }

    if (ZX_StrIeq (cmd, "nmidiag")) {
        ZX_CommandNmiDiag();
        return;
    }

    if (ZX_StrIeq (cmd, "suspend")) {
        ZX_CartDrawSuspend();
        printf("ZX screen drawing suspended\r\n");
        return;
    }

    if (ZX_StrIeq (cmd, "resume")) {
        ZX_CartDrawResume();
        printf("ZX screen drawing resumed\r\n");
        return;
    }

    if (ZX_StrIeq (cmd, "gfxtest")) {
        ZX_CommandGfxTest();
        return;
    }

    if (ZX_StrIeq (cmd, "ramtest")) {
        uint32_t base = ZX_RAMTEST_DEFAULT_BASE;
        uint32_t len = ZX_RAMTEST_DEFAULT_LEN;
        a0 = strtok (NULL, " \t");
        a1 = strtok (NULL, " \t");
        if (a0 != NULL) {
            if (!ZX_ParseAddressHex (a0, &base)) {
                printf("Usage: ramtest [addr] [len]\r\n");
                return;
            }
        }
        if (a1 != NULL) {
            /* Accept length as hex (without 0x), 0x-prefixed hex, or decimal. */
            if (!ZX_ParseAddressHex (a1, &len)) {
                if (!ZX_ParseU32 (a1, &len)) {
                    printf("Usage: ramtest [addr] [len]\r\n");
                    return;
                }
            }
        }
        ZX_CommandRamTest ((uint16_t)base, len);
        return;
    }

    if (ZX_StrIeq (cmd, "busrwtest")) {
        a0 = strtok (NULL, " \t");
        a1 = strtok (NULL, " \t");
        if (!ZX_ParseAddressHex (a0, &v0)) {
            printf("Usage: busrwtest <addr> <n>\r\n");
            return;
        }
        v1 = 16u;
        if (a1 != NULL) {
            (void)ZX_ParseU32 (a1, &v1);
        }
        if (v1 == 0u || v1 > 100u) { v1 = 16u; }
        ZX_CommandBusRwTest ((uint16_t)v0, (uint8_t)v1);
        return;
    }

    if (ZX_StrIeq (cmd, "ls")) {
        a0 = strtok (NULL, " \t");
        ZX_CommandLs (a0);
        return;
    }

    if (ZX_StrIeq (cmd, "tapinfo")) {
        a0 = strtok (NULL, " \t");
        ZX_CommandTapInfo (a0);
        return;
    }

    if (ZX_StrIeq (cmd, "taprun")) {
        a0 = strtok (NULL, " \t");
        a1 = strtok (NULL, " \t");
        ZX_CommandTapRun (a0, a1);
        return;
    }

    if (ZX_StrIeq (cmd, "z80info")) {
        a0 = strtok (NULL, " \t");
        if ((a0 == NULL) || (a0[0] == '\0')) { printf("Usage: z80info <path>\r\n"); return; }
        (void)Z80_Info (a0);
        return;
    }

    if (ZX_StrIeq (cmd, "z80run-bus") || ZX_StrIeq (cmd, "z80run-nmi") || ZX_StrIeq (cmd, "z80run")) {
        a0 = strtok (NULL, " \t");
        if ((a0 == NULL) || (a0[0] == '\0')) { printf("Usage: z80run <path>\r\n"); return; }
        (void)Z80_LoadAndRun (a0);
        return;
    }

    if (ZX_StrIeq (cmd, "z80select")) {
        a0 = strtok (NULL, " \t");
        ZX_CommandZ80Select (a0);
        return;
    }

    if (ZX_StrIeq (cmd, "launchtest")) {
        int id = 0;
        a0 = strtok (NULL, " \t");
        if ((a0 == NULL) || (a0[0] == '\0')) {
            printf("Usage: launchtest <id>  (1=NMI mailbox, 2=marker tramp)\r\n");
            return;
        }
        id = (int)strtol (a0, NULL, 0);
        (void)LaunchTest_Run (id);
        return;
    }

    if (ZX_StrIeq (cmd, "romcs")) {
        a0 = strtok (NULL, " \t");
        ZX_CommandRomcs (a0);
        return;
    }

    printf("Unknown command: %s\r\n", cmd);
}

void ZX_Monitor_Init (void) {
    s_monitor_len = 0u;
    memset (s_monitor_line, 0, sizeof (s_monitor_line));

    /* Disable stdout buffering so per-character local echo appears
       immediately rather than waiting for a newline flush. */
    setvbuf (stdout, NULL, _IONBF, 0);

    printf("ZX monitor ready. BUSREQ/BUSACK memory access enabled.\r\n");
    printf("Clock sync uses GPIOB.13 inverted ZX clock edges.\r\n");
    printf("Address arguments are HEX (e.g. 3000, 3FFF, 0x3000).\r\n");
    ZX_PrintHelp();
    printf("> ");
}

void ZX_Monitor_AutoStartZ80Select (void) {
    printf("\r\nAuto-start: z80select\r\n");
    ZX_CommandZ80Select (NULL);
    printf("> ");
}

void ZX_Monitor_Poll (void) {
    char ch;

    while (ZX_UartTryReadChar (&ch)) {
        if ((ch == '\r') || (ch == '\n')) {
            printf("\r\n");
            s_monitor_line[s_monitor_len] = '\0';
            ZX_ExecuteCommand (s_monitor_line);
            s_monitor_len = 0u;
            s_monitor_line[0] = '\0';
            printf("> ");
            continue;
        }

        if ((ch == '\b') || (ch == 0x7Fu)) {
            if (s_monitor_len > 0u) {
                --s_monitor_len;
                s_monitor_line[s_monitor_len] = '\0';
                printf("\b \b");
            }
            continue;
        }

        if (isprint ((unsigned char)ch) && (s_monitor_len < (ZX_MONITOR_BUF_SIZE - 1u))) {
            s_monitor_line[s_monitor_len++] = ch;
            s_monitor_line[s_monitor_len] = '\0';
            printf("%c", ch);
        }
    }
}
