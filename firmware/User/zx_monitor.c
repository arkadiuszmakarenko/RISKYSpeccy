#include "zx_monitor.h"

#include "debug.h"
#include "z80_loader.h"
#include "zx_terminal.h"
#include "ff.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZX_MONITOR_BUF_SIZE 96u

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

static void ZX_PrintHelp (void) {
    printf("Commands:\r\n");
    printf("  help                         - show commands\r\n");
    printf("  ls [path]                    - list directory on USB drive\r\n");
    printf("  z80info <path>               - parse .z80 v1 snapshot header\r\n");
    printf("  z80run <path>                - copy .z80 body to RAM via NMI mailbox (no launch)\r\n");
    printf("  z80run-nmi <path>            - alias of z80run (same NMI copy path)\r\n");
    printf("  z80select [path]             - browse USB .z80 files on ZX screen and run\r\n");
    printf("  romcs <on|off>               - assert/release cart ROMCS manually\r\n");
}

static void ZX_CommandZ80Select (const char *path) {
    ZX_TerminalCommandZ80Select (path);
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


static void ZX_ExecuteCommand (char *line) {
    char *cmd = strtok (line, " \t");
    char *a0;

    if (cmd == NULL) {
        return;
    }

    if (ZX_StrIeq (cmd, "help") || ZX_StrIeq (cmd, "?")) {
        ZX_PrintHelp();
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

    if (ZX_StrIeq (cmd, "z80run-nmi") || ZX_StrIeq (cmd, "z80run")) {
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



    printf("Unknown command: %s\r\n", cmd);
}

void ZX_Monitor_Init (void) {
    s_monitor_len = 0u;
    memset (s_monitor_line, 0, sizeof (s_monitor_line));

    /* Disable stdout buffering so per-character local echo appears
       immediately rather than waiting for a newline flush. */
    setvbuf (stdout, NULL, _IONBF, 0);
     ZX_TerminalInit();

    printf("ZX monitor ready.\r\n");
    printf("Clock sync uses GPIOB.13 inverted ZX clock edges.\r\n");
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
