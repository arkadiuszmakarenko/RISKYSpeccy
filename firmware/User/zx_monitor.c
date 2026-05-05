#include "zx_monitor.h"

#include "debug.h"
#include "z80_loader.h"
#include "zx_bus.h"
#include "zx_terminal.h"
#include "tape_player.h"
#include "ff.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZX_MONITOR_BUF_SIZE 96u

static char s_monitor_line[ZX_MONITOR_BUF_SIZE];
static uint8_t s_monitor_len = 0u;
static uint8_t s_autostart_done = 0u;
static uint8_t s_autostart_pending = 0u;

static void ZX_CommandZ80Select (const char *path);

static void ZX_TryAutoStartZ80Select (void) {
    const char *pending;
    uint8_t probe = 0u;

    if (s_autostart_pending == 0u) {
        return;
    }

    if (!ZX_BusReadBlock (0x0000u, &probe, 1u)) {
        return;
    }

    s_autostart_pending = 0u;
    s_autostart_done = 1u;

    ZX_CommandZ80Select (NULL);
    pending = ZX_TerminalPendingZ80Selection();
    if ((pending != NULL) && (pending[0] != '\0')) {
        printf("z80select: loading %s\r\n", pending);
        (void)Z80_LoadAndRun (pending);
        ZX_TerminalClearPendingZ80Selection();
    }
    printf("> ");
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

static void ZX_PrintHelp (void) {
    printf("Commands:\r\n");
    printf("  help                         - show commands\r\n");
    printf("  ls [path]                    - list directory on USB drive\r\n");
    printf("  z80info <path>               - parse .z80 v1 snapshot header\r\n");
    printf("  z80run <path>                - copy .z80 body to RAM via NMI mailbox (no launch)\r\n");
    printf("  z80run-nmi <path>            - alias of z80run (same NMI copy path)\r\n");
    printf("  z80select [path]             - browse USB .z80 files on ZX screen and run\r\n");
    printf("  romcs <on|off>               - assert/release cart ROMCS manually\r\n");
    printf("  tapplay <path>               - load .tap, reset Spectrum, play tape on #FE EAR\r\n");
    printf("  tapstop                      - stop tape playback\r\n");
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

    if (ZX_StrIeq (cmd, "tapplay")) {
        char ch;
        a0 = strtok (NULL, " \t");
        if ((a0 == NULL) || (a0[0] == '\0')) {
            printf ("Usage: tapplay <path>\r\n");
            return;
        }
        if (!TAP_Player_Load (a0)) {
            return;
        }
        /* Release cart ROM so the ZX ULA ROM becomes visible, then reset
           the Z80 so it boots to the Spectrum BASIC prompt (~1.2 s).   */
        ZX_RomcsRelease();
        ZX_Z80Reset();
        printf ("Spectrum ready.  Type LOAD \"\" on the Spectrum keyboard,\r\n");
        printf ("then press Enter here to start tape playback: ");
        /* Wait for Enter on the UART console — keyboard still works on
           the Spectrum since the IORQ ISR is not yet active.           */
        while (1) {
            if (ZX_UartTryReadChar (&ch)) {
                if ((ch == '\r') || (ch == '\n')) {
                    break;
                }
            }
        }
        printf ("\r\n");
        TAP_Player_Start();
        return;
    }

    if (ZX_StrIeq (cmd, "tapstop")) {
        TAP_Player_Stop();
        return;
    }



    printf("Unknown command: %s\r\n", cmd);
}

void ZX_Monitor_Init (void) {
    s_monitor_len = 0u;
    s_autostart_done = 0u;
    s_autostart_pending = 0u;
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
    if (s_autostart_done != 0u) {
        return;
    }
    if (s_autostart_pending != 0u) {
        return;
    }
    s_autostart_pending = 1u;

    printf("\r\nAuto-start: z80select\r\n");
    ZX_TryAutoStartZ80Select();
}

void ZX_Monitor_Poll (void) {
    char ch;

    ZX_TryAutoStartZ80Select();

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
