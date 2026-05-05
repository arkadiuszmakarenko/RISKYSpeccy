/*
 * tape_player.c — Experimental ZX Spectrum .TAP player.
 *
 * Architecture
 * ------------
 * Instead of copying data into ZX RAM via the NMI mailbox, this module
 * feeds the EAR bit (data bus bit 6) directly in response to every
 * IN A,(#FE) the Z80 issues.  Two ISRs work together:
 *
 *   TAP_TimerISR  (TIM2, VTF slot 2)
 *       One-shot timer.  On each fire it toggles s_ear_bit and advances
 *       the tape state machine, then arms the next one-shot for the period
 *       of the next half-pulse.  Never touches the data bus.
 *
 *   TAP_IorqISR  (EXTI9_5 on PC8 = /IORQ falling edge, VTF slot 1)
 *       Fires whenever /IORQ asserts.  If /RD is also low and A0=0
 *       (ZX ULA port read), it drives the full 8-bit data bus:
 *         bits 0-4 = 1  (all keyboard rows "no key pressed")
 *         bit  5   = 1  (reserved, normally 1)
 *         bit  6   = s_ear_bit  (← tape EAR signal)
 *         bit  7   = 1  (reserved, normally 1)
 *       It holds the bus until /RD deasserts, then tristates.
 *
 * Tape timing reference (3.5 MHz Z80 T-states)
 * ---------------------------------------------
 *   Pilot half-pulse  : 2168 T
 *   Sync first half   :  667 T
 *   Sync second half  :  735 T
 *   Bit-0 half-pulse  :  855 T  (two equal halves per bit)
 *   Bit-1 half-pulse  : 1710 T
 *   Pilot pulse count : 8064 (header) / 3220 (data)
 *   Inter-block pause : ~1 second (EAR held constant, no transitions)
 *
 * Timer configuration
 * -------------------
 *   TIM2 prescaler = 1 (÷2) → 144 MHz / 2 = 72 MHz tick rate.
 *   T-state ≈ 72e6 / 3.5e6 = 20.57 → rounded to 20 ticks (~3 % short,
 *   well within the ±10 % tolerance of the ZX ROM tape loader).
 *
 * Trigger flow (from zx_monitor)
 * --------------------------------
 *   1. ZX_RomcsRelease()          — tristate cart ROMCS; ZX ULA ROM visible
 *   2. ZX_Z80Reset()              — Z80 boots to Spectrum BASIC prompt
 *   3. TAP_Player_Load(path)      — load .tap from USB
 *   4. User types LOAD "" on the Spectrum keyboard (keyboard works normally
 *      because the IORQ ISR is not yet active)
 *   5. User presses Enter on the UART console
 *   6. TAP_Player_Start()         — arm TIM2 + IORQ EXTI; pilot begins
 *   7. ROM tape loader detects pilot edges, syncs, loads the game
 */

#include "tape_player.h"
#include "ff.h"

#include <ctype.h>
#include <string.h>
#include <stdio.h>

#pragma GCC push_options
#pragma GCC optimize("Ofast")

/* ------------------------------------------------------------------ */
/* Build constants                                                      */
/* ------------------------------------------------------------------ */

/* Tape buffer: 50 KB covers a complete standard 48 K game .tap       */
#define TAP_BUF_SIZE        (50u * 1024u)

/* TIM2: PSC=1 → 72 MHz ticks.  T-state ≈ 20 ticks.                 */
#define TAP_PSC             1u
#define TAP_TICKS_PER_T     20u

#define TAP_PILOT_HALF  ((uint16_t)(2168u * TAP_TICKS_PER_T))  /* 43360 */
#define TAP_SYNC1_HALF  ((uint16_t)( 667u * TAP_TICKS_PER_T))  /* 13340 */
#define TAP_SYNC2_HALF  ((uint16_t)( 735u * TAP_TICKS_PER_T))  /* 14700 */
#define TAP_BIT0_HALF   ((uint16_t)( 855u * TAP_TICKS_PER_T))  /* 17100 */
#define TAP_BIT1_HALF   ((uint16_t)(1710u * TAP_TICKS_PER_T))  /* 34200 */

/* Pause = ~1 s expressed as number of pilot-half-period intervals.
   1000 ms / (43360 / 72e6 * 1e3 ms) = 1000 / 0.6022 ≈ 1660          */
#define TAP_PAUSE_COUNT     1660u

/* Standard pilot pulse counts (full pulses; each = 2 half-pulses)    */
#define TAP_PILOT_HDR       8064u
#define TAP_PILOT_DAT       3220u

/* ZX bus pins                                                         */
#define ZX_PIN_RD           GPIO_Pin_5   /* GPIOB — active low         */
#define ZX_IORQ_LINE        EXTI_Line8   /* PC8 — /IORQ                */
#define ZX_EAR_BIT          0x40u        /* data bus bit 6             */
#define ZX_DATA_IDLE        0xFFu        /* all bits high: no keys, EAR=1 */

/* ------------------------------------------------------------------ */
/* State machine                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    TAP_IDLE            = 0,
    TAP_PILOT,               /* sending pilot half-pulses              */
    TAP_SYNC1,               /* first sync half-pulse                  */
    TAP_SYNC2,               /* second sync half-pulse                 */
    TAP_DATA_FIRST_HALF,     /* first half of current data bit         */
    TAP_DATA_SECOND_HALF,    /* second half of current data bit        */
    TAP_PAUSE,               /* inter-block silence                    */
    TAP_DONE                 /* all blocks played                      */
} TapState;

/* ------------------------------------------------------------------ */
/* Player state — volatile members accessed from both ISRs             */
/* ------------------------------------------------------------------ */

static uint8_t          s_tap_buf[TAP_BUF_SIZE];
static uint32_t         s_tap_total = 0u;   /* bytes loaded            */

static volatile TapState  s_state             = TAP_IDLE;
static volatile uint8_t   s_ear_bit           = 0u;
static volatile uint8_t   s_running           = 0u;

/* Non-volatile: only written/read from TIM2 ISR                       */
static uint32_t           s_tap_pos           = 0u;
static uint32_t           s_pilot_half_rem    = 0u;
static uint16_t           s_block_bytes_rem   = 0u;
static uint8_t            s_cur_byte          = 0u;
static uint8_t            s_cur_bit_mask      = 0u;
static uint16_t           s_pause_count       = 0u;

static int TAP_PathHasExtension (const char *path, const char *ext3) {
    size_t len;

    if ((path == NULL) || (ext3 == NULL)) {
        return 0;
    }
    len = strlen (path);
    if (len < 4u) {
        return 0;
    }
    path += (len - 4u);
    return (path[0] == '.') &&
           (tolower ((unsigned char)path[1]) == (unsigned char)ext3[0]) &&
           (tolower ((unsigned char)path[2]) == (unsigned char)ext3[1]) &&
           (tolower ((unsigned char)path[3]) == (unsigned char)ext3[2]);
}

/* Convert supported TZX blocks to TAP-style blocks in-place in s_tap_buf.
   Currently supported: 0x10 (standard speed data).
   Ignored metadata/flow blocks: 0x20, 0x21, 0x22, 0x30, 0x31, 0x32, 0x33, 0x35, 0x5A.
   Unsupported data-path/timing blocks (e.g. turbo/pure/CSW/direct) return 0. */
static int TAP_ConvertTzxInPlace (uint32_t in_bytes, uint32_t *out_bytes) {
    uint32_t rd = 10u;
    uint32_t wr = 0u;
    uint32_t converted_blocks = 0u;

    if ((out_bytes == NULL) || (in_bytes < 10u)) {
        return 0;
    }

    if ((memcmp (s_tap_buf, "ZXTape!\x1A", 8u) != 0) || (s_tap_buf[8] != 0x01u)) {
        return 0;
    }

    while (rd < in_bytes) {
        uint8_t id = s_tap_buf[rd];

        switch (id) {
        case 0x10u: {
            uint16_t len;

            if ((rd + 5u) > in_bytes) {
                return 0;
            }
            len = (uint16_t)s_tap_buf[rd + 3u] |
                  (uint16_t)((uint16_t)s_tap_buf[rd + 4u] << 8);
            if ((rd + 5u + (uint32_t)len) > in_bytes) {
                return 0;
            }
            if ((wr + 2u + (uint32_t)len) > TAP_BUF_SIZE) {
                return 0;
            }

            s_tap_buf[wr] = (uint8_t)(len & 0xFFu);
            s_tap_buf[wr + 1u] = (uint8_t)(len >> 8);
            memmove (&s_tap_buf[wr + 2u], &s_tap_buf[rd + 5u], len);

            wr += (uint32_t)len + 2u;
            rd += (uint32_t)len + 5u;
            ++converted_blocks;
            break;
        }

        case 0x20u: /* pause */
            if ((rd + 3u) > in_bytes) { return 0; }
            rd += 3u;
            break;

        case 0x21u: { /* group start */
            uint8_t n;
            if ((rd + 2u) > in_bytes) { return 0; }
            n = s_tap_buf[rd + 1u];
            if ((rd + 2u + n) > in_bytes) { return 0; }
            rd += 2u + n;
            break;
        }

        case 0x22u: /* group end */
            rd += 1u;
            break;

        case 0x30u: { /* text description */
            uint8_t n;
            if ((rd + 2u) > in_bytes) { return 0; }
            n = s_tap_buf[rd + 1u];
            if ((rd + 2u + n) > in_bytes) { return 0; }
            rd += 2u + n;
            break;
        }

        case 0x31u: { /* message block */
            uint8_t n;
            if ((rd + 3u) > in_bytes) { return 0; }
            n = s_tap_buf[rd + 2u];
            if ((rd + 3u + n) > in_bytes) { return 0; }
            rd += 3u + n;
            break;
        }

        case 0x32u: { /* archive info */
            uint16_t n;
            if ((rd + 3u) > in_bytes) { return 0; }
            n = (uint16_t)s_tap_buf[rd + 1u] |
                (uint16_t)((uint16_t)s_tap_buf[rd + 2u] << 8);
            if ((rd + 3u + (uint32_t)n) > in_bytes) { return 0; }
            rd += 3u + (uint32_t)n;
            break;
        }

        case 0x33u: { /* hardware type */
            uint8_t n;
            if ((rd + 2u) > in_bytes) { return 0; }
            n = s_tap_buf[rd + 1u];
            if ((rd + 2u + (uint32_t)n * 3u) > in_bytes) { return 0; }
            rd += 2u + (uint32_t)n * 3u;
            break;
        }

        case 0x35u: { /* custom info */
            uint32_t n;
            if ((rd + 21u) > in_bytes) { return 0; }
            n = (uint32_t)s_tap_buf[rd + 17u] |
                ((uint32_t)s_tap_buf[rd + 18u] << 8) |
                ((uint32_t)s_tap_buf[rd + 19u] << 16) |
                ((uint32_t)s_tap_buf[rd + 20u] << 24);
            if ((rd + 21u + n) > in_bytes) { return 0; }
            rd += 21u + n;
            break;
        }

        case 0x5Au: /* glue block */
            if ((rd + 10u) > in_bytes) { return 0; }
            rd += 10u;
            break;

        default:
            printf ("tap: unsupported TZX block 0x%02X\r\n", (unsigned)id);
            return 0;
        }
    }

    if (converted_blocks == 0u) {
        return 0;
    }

    *out_bytes = wr;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Inline timer helper                                                  */
/* ------------------------------------------------------------------ */

static inline void TAP_ArmTimer (uint16_t period) {
    TIM2->ATRLR = (uint16_t)(period - 1u);
    TIM2->INTFR = 0u;           /* clear UIF before restarting         */
    TIM2->CTLR1 |= TIM_CEN;    /* OPM already set; start one-shot     */
}

/* ------------------------------------------------------------------ */
/* TAP block helpers (called only from TIM2 ISR)                       */
/* ------------------------------------------------------------------ */

/* Advance s_tap_pos to the next TAP block and set up block state.
   Returns 1 if a block was loaded, 0 if no more data.                */
static int TAP_NextBlock (void) {
    uint16_t block_len;

    if ((s_tap_pos + 2u) > s_tap_total) {
        return 0;
    }

    block_len  = (uint16_t)s_tap_buf[s_tap_pos];
    block_len |= (uint16_t)((uint16_t)s_tap_buf[s_tap_pos + 1u] << 8);
    s_tap_pos += 2u;

    if ((s_tap_pos + (uint32_t)block_len) > s_tap_total) {
        return 0;   /* truncated block */
    }

    s_block_bytes_rem = block_len;

    /* Flag byte (first byte of block data) decides pilot length       */
    {
        uint8_t flag = s_tap_buf[s_tap_pos];
        uint32_t pulses = (flag == 0x00u) ? TAP_PILOT_HDR : TAP_PILOT_DAT;
        s_pilot_half_rem = pulses * 2u;   /* each pulse = 2 half-pulses */
    }

    s_cur_bit_mask = 0u;   /* no bit in progress                       */
    return 1;
}

/* Load next bit from s_cur_byte / s_tap_buf.
   Returns 1 if a bit is available, 0 if the block is exhausted.      */
static int TAP_NextBit (void) {
    if (s_cur_bit_mask == 0u) {
        if (s_block_bytes_rem == 0u) {
            return 0;
        }
        s_cur_byte     = s_tap_buf[s_tap_pos++];
        s_block_bytes_rem--;
        s_cur_bit_mask = 0x80u;   /* MSB first                         */
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* TIM2 ISR — tape state machine                                        */
/* ------------------------------------------------------------------ */

__attribute__((section(".text.fastirq"), aligned(64), noinline))
void TAP_TimerISR (void) {
    TIM2->INTFR = 0u;   /* acknowledge UIF                             */

    /* ---- Pause state: hold EAR constant, count down ~1 s ---------- */
    if (s_state == TAP_PAUSE) {
        if (s_pause_count > 0u) {
            --s_pause_count;
            TAP_ArmTimer (TAP_PILOT_HALF);   /* reuse pilot period as tick */
            return;
        }
        /* Pause elapsed: load next block                               */
        s_ear_bit = 0u;
        if (TAP_NextBlock()) {
            s_state = TAP_PILOT;
            TAP_ArmTimer (TAP_PILOT_HALF);
        } else {
            s_state   = TAP_DONE;
            s_running = 0u;
            TIM2->CTLR1   &= (uint16_t)~TIM_CEN;
            NVIC_DisableIRQ (TIM2_IRQn);
            EXTI->INTENR  &= ~ZX_IORQ_LINE;
            NVIC_DisableIRQ (EXTI9_5_IRQn);
        }
        return;
    }

    /* ---- All other states: toggle EAR on every half-pulse ---------- */
    s_ear_bit ^= 1u;

    switch (s_state) {

    /* ---- Pilot ------------------------------------------------------ */
    case TAP_PILOT:
        if (s_pilot_half_rem > 1u) {
            --s_pilot_half_rem;
            TAP_ArmTimer (TAP_PILOT_HALF);
        } else {
            s_state = TAP_SYNC1;
            TAP_ArmTimer (TAP_SYNC1_HALF);
        }
        break;

    /* ---- Sync ------------------------------------------------------- */
    case TAP_SYNC1:
        s_state = TAP_SYNC2;
        TAP_ArmTimer (TAP_SYNC2_HALF);
        break;

    case TAP_SYNC2:
        s_state = TAP_DATA_FIRST_HALF;
        if (TAP_NextBit()) {
            uint16_t p = (s_cur_byte & s_cur_bit_mask) ? TAP_BIT1_HALF : TAP_BIT0_HALF;
            TAP_ArmTimer (p);
        } else {
            /* Empty block — go straight to pause                       */
            s_state       = TAP_PAUSE;
            s_pause_count = TAP_PAUSE_COUNT;
            TAP_ArmTimer (TAP_PILOT_HALF);
        }
        break;

    /* ---- Data bits -------------------------------------------------- */
    case TAP_DATA_FIRST_HALF:
        /* Same period for the second half of this bit                  */
        s_state = TAP_DATA_SECOND_HALF;
        {
            uint16_t p = (s_cur_byte & s_cur_bit_mask) ? TAP_BIT1_HALF : TAP_BIT0_HALF;
            TAP_ArmTimer (p);
        }
        break;

    case TAP_DATA_SECOND_HALF:
        /* Bit complete — advance to next bit                           */
        s_cur_bit_mask >>= 1;
        s_state = TAP_DATA_FIRST_HALF;
        if (TAP_NextBit()) {
            uint16_t p = (s_cur_byte & s_cur_bit_mask) ? TAP_BIT1_HALF : TAP_BIT0_HALF;
            TAP_ArmTimer (p);
        } else {
            /* Block exhausted — inter-block pause                      */
            s_state       = TAP_PAUSE;
            s_pause_count = TAP_PAUSE_COUNT;
            TAP_ArmTimer (TAP_PILOT_HALF);
        }
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* IORQ ISR — respond to IN A,(#FE) with current EAR bit               */
/* ------------------------------------------------------------------ */

__attribute__((section(".text.fastirq"), aligned(64), noinline))
void TAP_IorqISR (void) {
    /*
     * /IORQ (PC8) just fell LOW.
     * Gate on: A0=0 (ZX ULA port) AND /RD=0 (IN instruction).
     * If /RD is still high this is an OUT or interrupt-acknowledge —
     * just clear the flag and return.
     */
    if (((GPIOE->INDR & 1u) == 0u) && ((GPIOB->INDR & ZX_PIN_RD) == 0u)) {
        uint8_t data = ZX_DATA_IDLE;
        if (s_ear_bit == 0u) {
            data = (uint8_t)(ZX_DATA_IDLE & ~ZX_EAR_BIT);  /* clear bit 6 */
        }

        GPIOD->CFGLR = 0x33333333u;                        /* data bus: PP output */
        GPIOD->OUTDR = (GPIOD->OUTDR & ~0xFFu) | data;

        while ((GPIOB->INDR & ZX_PIN_RD) == 0u) {}        /* hold until /RD deasserts */

        GPIOD->CFGLR = 0x44444444u;                        /* data bus: floating input */
    }

    EXTI->INTFR = ZX_IORQ_LINE;   /* clear EXTI_Line8 pending flag    */
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int TAP_Player_Load (const char *path) {
    FIL     fp;
    FRESULT fr;
    UINT    got;
    uint32_t converted_bytes;
    int is_tzx;

    s_tap_total = 0u;
    s_tap_pos   = 0u;

    if (path == NULL) {
        return 0;
    }

    fr = f_open (&fp, path, FA_READ);
    if (fr != FR_OK) {
        printf ("tape: cannot open '%s' (err=%d)\r\n", path, (int)fr);
        return 0;
    }

    fr = f_read (&fp, s_tap_buf, TAP_BUF_SIZE, &got);
    f_close (&fp);

    if (fr != FR_OK) {
        printf ("tape: read error (err=%d)\r\n", (int)fr);
        return 0;
    }
    if (got == 0u) {
        printf ("tape: file is empty\r\n");
        return 0;
    }
    if (got == TAP_BUF_SIZE) {
        printf ("tape: WARNING file truncated to %u bytes\r\n",
                (unsigned)TAP_BUF_SIZE);
    }

    is_tzx = TAP_PathHasExtension (path, "tzx");
    if (is_tzx) {
        if (!TAP_ConvertTzxInPlace ((uint32_t)got, &converted_bytes)) {
            printf ("tape: unsupported or invalid .tzx '%s'\r\n", path);
            return 0;
        }
        s_tap_total = converted_bytes;
        printf ("tape: loaded .tzx '%s' (%lu bytes, converted to %lu TAP-stream bytes)\r\n",
                path,
                (unsigned long)got,
                (unsigned long)converted_bytes);
    } else {
        s_tap_total = (uint32_t)got;
        printf ("tape: loaded .tap '%s' (%lu bytes)\r\n",
                path,
                (unsigned long)got);
    }

    return 1;
}

void TAP_Player_Start (void) {
    GPIO_InitTypeDef  gpio = {0};
    EXTI_InitTypeDef  exti = {0};

    if (s_tap_total == 0u) {
        printf ("tap: no data — call TAP_Player_Load first\r\n");
        return;
    }

    /* Initialise player state */
    s_tap_pos        = 0u;
    s_ear_bit        = 0u;
    s_state          = TAP_IDLE;
    s_cur_bit_mask   = 0u;
    s_pause_count    = 0u;

    if (!TAP_NextBlock()) {
        printf ("tap: TAP file contains no valid blocks\r\n");
        return;
    }
    s_state  = TAP_PILOT;
    s_running = 1u;

    /* ---- Configure TIM2 -------------------------------------------- */
    RCC_APB1PeriphClockCmd (RCC_APB1Periph_TIM2, ENABLE);

    TIM2->CTLR1  = 0u;                      /* reset control register  */
    TIM2->PSC    = TAP_PSC;                  /* 144 MHz / 2 = 72 MHz    */
    TIM2->ATRLR  = (uint16_t)(TAP_PILOT_HALF - 1u);
    TIM2->CNT    = 0u;
    TIM2->INTFR  = 0u;
    TIM2->DMAINTENR = TIM_IT_Update;         /* enable UIF interrupt    */
    TIM2->CTLR1  = TIM_OPM | TIM_CEN;       /* one-pulse, start        */

    SetVTFIRQ ((u32)TAP_TimerISR, TIM2_IRQn, 2, ENABLE);
    NVIC_EnableIRQ (TIM2_IRQn);

    /* ---- Configure EXTI on PC8 (/IORQ) falling edge ---------------- */
    RCC_APB2PeriphClockCmd (RCC_APB2Periph_AFIO | RCC_APB2Periph_GPIOC, ENABLE);

    gpio.GPIO_Pin  = GPIO_Pin_8;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init (GPIOC, &gpio);

    GPIO_EXTILineConfig (GPIO_PortSourceGPIOC, GPIO_PinSource8);

    exti.EXTI_Line    = ZX_IORQ_LINE;
    exti.EXTI_Mode    = EXTI_Mode_Interrupt;
    exti.EXTI_Trigger = EXTI_Trigger_Falling;
    exti.EXTI_LineCmd = ENABLE;
    EXTI_Init (&exti);

    EXTI->INTFR = ZX_IORQ_LINE;             /* clear any stale flag    */
    SetVTFIRQ ((u32)TAP_IorqISR, EXTI9_5_IRQn, 1, ENABLE);
    NVIC_EnableIRQ (EXTI9_5_IRQn);

    printf ("tap: playback started — pilot tone running\r\n");
}

void TAP_Player_Stop (void) {
    NVIC_DisableIRQ (TIM2_IRQn);
    TIM2->CTLR1    &= (uint16_t)~TIM_CEN;
    TIM2->DMAINTENR = 0u;

    NVIC_DisableIRQ (EXTI9_5_IRQn);
    EXTI->INTENR   &= ~ZX_IORQ_LINE;
    EXTI->INTFR     = ZX_IORQ_LINE;

    /* Tristate data bus — ensure no stale output drive                */
    GPIOD->CFGLR = 0x44444444u;

    s_state   = TAP_IDLE;
    s_running = 0u;
    printf ("tap: stopped\r\n");
}

int TAP_Player_IsRunning (void) {
    return (int)s_running;
}

int TAP_Player_HasTapeLoaded (void) {
    return (s_tap_total != 0u) ? 1 : 0;
}

#pragma GCC pop_options
