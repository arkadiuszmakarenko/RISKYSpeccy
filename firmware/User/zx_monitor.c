#include "zx_monitor.h"

#include "zx_bus.h"

#include <ctype.h>
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

#define ZX_GFXTEST_FRAMES   5u
#define ZX_GFXTEST_FRAME_MS 700u
#define ZX_GFXTEST_NMI_TO   200u

#define ZX_RAMTEST_CHUNK     0x0200u  /* matches ZX_NMI_WCMD_CHUNK in zx_bus.c */
#define ZX_RAMTEST_NMI_TO    300u
#define ZX_RAMTEST_DEFAULT_BASE 0xC000u
#define ZX_RAMTEST_DEFAULT_LEN  0x4000u  /* 16 KB top RAM, uncontended, safe */
#define ZX_RAMTEST_MAX_ERRORS   8u

static char s_monitor_line[ZX_MONITOR_BUF_SIZE];
static uint8_t s_monitor_len = 0u;

static uint8_t s_gfx_pixels[ZX_SCREEN_PIXELS_LEN];
static uint8_t s_gfx_attrs[ZX_SCREEN_ATTRS_LEN];
static uint8_t s_bridge_seq = 0u;
static uint8_t s_view_seq = 0u;

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
    printf("  nmi                          - trigger NMI pulse\r\n");
    printf("  test [addr]                  - %u-byte self-test in 3000..3FFF (hex addr)\r\n", (unsigned)ZX_TEST_LEN);
    printf("  busdiag                      - probe all ZX memory regions for r/w\r\n");
    printf("  busrwtest <addr> <n>         - write/readback test N times at addr\r\n");
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

    printf("Unknown command: %s\r\n", cmd);
}

void ZX_Monitor_Init (void) {
    s_monitor_len = 0u;
    memset (s_monitor_line, 0, sizeof (s_monitor_line));

    printf("ZX monitor ready. BUSREQ/BUSACK memory access enabled.\r\n");
    printf("Clock sync uses GPIOB.13 inverted ZX clock edges.\r\n");
    printf("Address arguments are HEX (e.g. 3000, 3FFF, 0x3000).\r\n");
    ZX_PrintHelp();
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
