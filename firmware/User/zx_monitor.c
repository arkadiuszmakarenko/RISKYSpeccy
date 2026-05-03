#include "zx_monitor.h"

#include "zx_bus.h"
#include "z80_loader.h"
#include "zx_terminal.h"
#include "ff.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZX_MONITOR_BUF_SIZE 96u
#define ZX_DUMP_MAX_LEN     256u
#define ZX_VIEW_MAX_BYTES   16u
#define ZX_CART_RAM_BASE    0x3000u
#define ZX_CART_RAM_LAST    0x3FFFu

static char s_monitor_line[ZX_MONITOR_BUF_SIZE];
static uint8_t s_monitor_len = 0u;

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
    printf("  suspend                      - pause ZX screen drawing\r\n");
    printf("  resume                       - resume ZX screen drawing\r\n");
    printf("  zxview <addr> [len]          - show live ZX RAM bytes on screen\r\n");
    printf("  zxmsg <text>                 - send text to ZX on-screen host bridge\r\n");
    printf("  zxttyinit                    - init MPU-side terminal and clear screen\r\n");
    printf("  zxtty <text>                 - write text/escapes (\\n \\r \\t \\e[...m) via MPU\r\n");
    printf("  keyread                      - read one ZX key event from mailbox\r\n");
    printf("  nmi                          - trigger NMI pulse\r\n");
    printf("  ls [path]                    - list directory on USB drive\r\n");
    printf("  z80info <path>               - parse .z80 v1 snapshot header\r\n");
    printf("  z80run <path>                - copy .z80 body to RAM via BUSREQ (no launch)\r\n");
    printf("  z80run-bus|z80run-nmi <path> - aliases of z80run (same BUSREQ copy path)\r\n");
    printf("  z80select [path]             - browse USB .z80 files on ZX screen and run\r\n");
    printf("  romcs <on|off>               - assert/release cart ROMCS manually\r\n");
}

static void ZX_CommandViewOff (void) {
    ZX_TerminalCommandViewOff();
}

static void ZX_CommandView (uint16_t address, uint8_t length) {
    ZX_TerminalCommandView (address, length);
}

static void ZX_CommandBridgeText (char *firstToken) {
    ZX_TerminalCommandBridgeText (firstToken);
}

static void ZX_CommandTermInit (void) {
    ZX_TerminalCommandTermInit();
}

static void ZX_CommandTermWrite (char *firstToken) {
    ZX_TerminalCommandTermWrite (firstToken);
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

static void ZX_CommandZ80Select (const char *path) {
    ZX_TerminalCommandZ80Select (path);
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

    if (ZX_StrIeq (cmd, "keyread")) {
        ZX_CommandKeyRead();
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

    if (ZX_StrIeq (cmd, "ls")) {
        a0 = strtok (NULL, " \t");
        ZX_CommandLs (a0);
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
        const char *pending;
        a0 = strtok (NULL, " \t");
        ZX_CommandZ80Select (a0);
        pending = ZX_TerminalPendingZ80Selection();
        if ((pending != NULL) && (pending[0] != '\0')) {
            printf ("z80select: loading %s\r\n", pending);
            (void)Z80_LoadAndRun (pending);
            ZX_TerminalClearPendingZ80Selection();
        }
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
     ZX_TerminalInit();

    printf("ZX monitor ready. BUSREQ/BUSACK memory access enabled.\r\n");
    printf("Clock sync uses GPIOB.13 inverted ZX clock edges.\r\n");
    printf("Address arguments are HEX (e.g. 3000, 3FFF, 0x3000).\r\n");
    ZX_PrintHelp();
    printf("> ");
}

void ZX_Monitor_AutoStartZ80Select (void) {
    const char *pending;

    printf("\r\nAuto-start: z80select\r\n");
    ZX_CommandZ80Select (NULL);
    pending = ZX_TerminalPendingZ80Selection();
    if ((pending != NULL) && (pending[0] != '\0')) {
        printf("z80select: loading %s\r\n", pending);
        (void)Z80_LoadAndRun (pending);
        ZX_TerminalClearPendingZ80Selection();
    }
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
