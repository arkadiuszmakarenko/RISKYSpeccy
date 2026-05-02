#include "zx_bus.h"
#include "zx_image.h"


// GPIOE Pins    0 - 16      Address
// GPIOD Pins    0 - 8       Data

// GPIOB Pin     3           ROMCS  0x0008
// GPIOB Pin     4           WR     0x0010
// GPIOB Pin     5           RD     0x0020

// GPIOB Pin     6           BUSACK 0x0040
// GPIOB Pin     7           BUSREQ 0x0080
// GPIOB Pin     8           HALT   0x0100

// GPIOB Pin     9           M1    0x0200
// GPIOB Pin     10          MREQ  0x0400
// GPIOB Pin     11          RESH  0x0800
// GPIOB Pin     12          INT
// GPIOB Pin     13          CK

// GPIOC Pin     6           RESET 0x0040
// GPIOC Pin     7           WAIT  0x0080
// GPIOC Pin     8           IORQ  0x0100
// GPIOC Pin     9           BDIR  0x0200

#pragma GCC push_options
#pragma GCC optimize("Ofast")

#define ZX_ROM_LAST_ADDRESS 0x3FFFu

#define ZX_PIN_WR      GPIO_Pin_4
#define ZX_PIN_RD      GPIO_Pin_5
#define ZX_PIN_BUSACK  GPIO_Pin_6
#define ZX_PIN_BUSREQ  GPIO_Pin_7
#define ZX_PIN_MREQ    GPIO_Pin_10
#define ZX_PIN_INT     GPIO_Pin_12
#define ZX_PIN_CK_INV  GPIO_Pin_13

#define ZX_CTRL_MASK   (ZX_PIN_WR | ZX_PIN_RD | ZX_PIN_MREQ)

#define ZX_BUS_TIMEOUT        120000u

#define ZX_KEY_SEQ_ADDR       0x3028u
#define ZX_KEY_CODE_ADDR      0x3029u

struct ZXCartState {
    uint32_t BusOn;
    uint32_t BusOff;
    uint32_t DataMask;
    uint16_t RamBase;
    uint16_t RomLast;
    uint16_t PinRD;
    uint16_t PinWR;
    uint16_t PinMREQ;
    uint32_t IRQLine;
    volatile uint8_t ram[0x1000];
};

static struct ZXCartState s_state;
static struct ZXCartState *state_pointer;
static volatile int s_rom_released = 0;
static uint8_t s_key_last_seq = 0u;

/* M1 handover: when armed, the cart ISR drops ROMCS the instant the Z80 does
   an opcode (M1) fetch from s_handover_addr.  Used by the .z80 snapshot
   loader so the very last fetch in cart-ROM space (a RET in upper RAM)
   triggers ROMCS handoff to the internal Spectrum ROM, just-in-time before
   the user code's first fetch. */
static volatile uint16_t s_handover_addr  = 0xFFFFu;
static volatile uint16_t s_handover_addr_b = 0xFFFFu;
static volatile uint16_t s_handover_fired_addr = 0xFFFFu;
static volatile int      s_handover_armed = 0;
static volatile int      s_handover_fired = 0;

/* Bus tracer.  Records MREQ-low events into a ring buffer.
   During the launch window RunCartWithM1Watch records every MREQ cycle
   (launcher POPs, tail execution, handover M1).  After ROMCS is released
   ZX_BusTraceSample continues recording post-handover fetches.
   512 entries cover the full launcher sequence (~100 events including
   RFSH cycles) plus a healthy post-handover window. */
#define ZX_TRACE_DEPTH 512u
typedef struct {
    uint16_t addr;
    uint8_t  data;
    uint8_t  ctrl;   /* bit0=M1 bit1=RD bit2=WR (1=high) */
} ZX_TraceEntry;
static volatile ZX_TraceEntry s_trace[ZX_TRACE_DEPTH];
static volatile uint16_t      s_trace_head = 0u;
static volatile uint8_t       s_trace_full = 0u;

#define ZX_PIN_M1      GPIO_Pin_9

static void ZX_DataBusInput (void) {
    GPIOD->CFGLR = 0x44444444;
}

static void ZX_DataBusOutput (void) {
    GPIOD->CFGLR = 0x33333333;
}

static void ZX_AddrBusInput (void) {
    GPIOE->CFGLR = 0x44444444;
    GPIOE->CFGHR = 0x44444444;
}

static void ZX_AddrBusOutput (void) {
    GPIOE->CFGLR = 0x33333333;
    GPIOE->CFGHR = 0x33333333;
}

static void ZX_CtrlLinesInput (void) {
    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = ZX_CTRL_MASK;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init (GPIOB, &gpio);
}

static void ZX_CtrlLinesOutput (void) {
    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = ZX_CTRL_MASK;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init (GPIOB, &gpio);
    GPIO_SetBits (GPIOB, ZX_CTRL_MASK);
}

static int ZX_WaitClockToggle (uint32_t timeout) {
    uint16_t start_level = GPIOB->INDR & ZX_PIN_CK_INV;

    while (timeout-- > 0u) {
        if ((GPIOB->INDR & ZX_PIN_CK_INV) != start_level) {
            return 1;
        }
    }

    return 0;
}

static int ZX_BusAcquire (void) {
    uint32_t timeout = ZX_BUS_TIMEOUT;

    /* Pre-position buses as inputs BEFORE asserting BUSREQ.
       NVIC stays enabled here so the cart ISR continues serving
       Z80 ROM fetches during the BUSREQ/BUSACK handshake.
       The Z80 completes its current M-cycle before asserting BUSACK,
       and that final fetch must be served or the Z80 will latch garbage
       and crash. Only disable NVIC once BUSACK is confirmed (buses floated). */
    ZX_CtrlLinesInput();
    ZX_AddrBusInput();
    ZX_DataBusInput();

    /* If the cart edge has been tristated (post-handover), BUSREQ is a
       floating input — GPIO_ResetBits is a no-op on inputs.  Re-arm it
       as push-pull output so we can drive it low. */
    if (s_rom_released) {
        GPIO_InitTypeDef gpio = {0};
        GPIO_SetBits (GPIOB, ZX_PIN_BUSREQ);   /* preload HIGH */
        gpio.GPIO_Pin = ZX_PIN_BUSREQ;
        gpio.GPIO_Mode = GPIO_Mode_Out_PP;
        gpio.GPIO_Speed = GPIO_Speed_50MHz;
        GPIO_Init (GPIOB, &gpio);
    }

    GPIO_ResetBits (GPIOB, ZX_PIN_BUSREQ);

    while (timeout-- > 0u) {
        if ((GPIOB->INDR & ZX_PIN_BUSACK) == 0u) {
            /* Z80 has floated its buses - safe to disable cart ISR now */
            NVIC_DisableIRQ (EXTI15_10_IRQn);
            EXTI->INTENR &= ~EXTI_Line10;
            EXTI->INTFR = EXTI_Line10;
            ZX_AddrBusOutput();
            ZX_CtrlLinesOutput();
            return 1;
        }
        (void)ZX_WaitClockToggle (512u);
    }

    GPIO_SetBits (GPIOB, ZX_PIN_BUSREQ);
    return 0;
}

static void ZX_BusRelease (void) {
    ZX_CtrlLinesInput();
    ZX_DataBusInput();
    ZX_AddrBusInput();

    GPIO_SetBits (GPIOB, ZX_PIN_BUSREQ);

    EXTI->INTFR = EXTI_Line10;
    EXTI->INTENR |= EXTI_Line10;
    NVIC_EnableIRQ (EXTI15_10_IRQn);
}

static int ZX_BusReadCycle (uint16_t address, uint8_t *value) {
    if (value == NULL) {
        return 0;
    }

    GPIOE->OUTDR = (GPIOE->OUTDR & 0xFFFF0000u) | (uint32_t)address;
    ZX_DataBusInput();

    GPIO_SetBits (GPIOB, ZX_CTRL_MASK);

    if (!ZX_WaitClockToggle (ZX_BUS_TIMEOUT)) {
        return 0;
    }

    GPIO_ResetBits (GPIOB, ZX_PIN_MREQ | ZX_PIN_RD);

    /* Wait a full ZX clock period (~286ns = 2 half-period toggles) before sampling.
       The ULA generates /RAS and /CAS from MREQ; 4116 DRAM tRAC = 150ns from /RAS.
       One toggle (~143ns) is right at the edge — sampling too early causes bits to
       read back as 1 (bus not yet driven by DRAM, sitting at pullup level).
       Two toggles guarantees DRAM output has fully settled before we latch. */
    if (!ZX_WaitClockToggle (ZX_BUS_TIMEOUT)) {
        GPIO_SetBits (GPIOB, ZX_PIN_MREQ | ZX_PIN_RD);
        return 0;
    }
    if (!ZX_WaitClockToggle (ZX_BUS_TIMEOUT)) {
        GPIO_SetBits (GPIOB, ZX_PIN_MREQ | ZX_PIN_RD);
        return 0;
    }

    *value = (uint8_t)(GPIOD->INDR & 0x00FFu);
    GPIO_SetBits (GPIOB, ZX_PIN_MREQ | ZX_PIN_RD);
    return 1;
}

static int ZX_BusWriteCycle (uint16_t address, uint8_t value) {
    GPIOE->OUTDR = (GPIOE->OUTDR & 0xFFFF0000u) | (uint32_t)address;

    /* Preload OUTDR BEFORE enabling output drivers.
       If OUTDR is set after ZX_DataBusOutput(), the pin briefly drives the stale
       OUTDR value (leftover from the previous operation) until the register is
       updated.  Bits 1 and 6 of the stale value differ from A5 in exactly the
       pattern seen (E7), causing intermittent DRAM write corruption for those bits. */
    GPIOD->OUTDR = (GPIOD->OUTDR & ~0x00FFu) | (uint32_t)value;
    ZX_DataBusOutput();   /* pins now drive correct value from the moment they go live */

    GPIO_SetBits (GPIOB, ZX_CTRL_MASK);

    /* Align to ZX clock edge */
    if (!ZX_WaitClockToggle (ZX_BUS_TIMEOUT)) {
        ZX_DataBusInput();
        return 0;
    }

    /* Assert MREQ first — generates /RAS through ULA address decode.
       Z80 write timing: MREQ goes low at T1, WR goes low one T-state later at T2.
       Asserting both simultaneously gives the ULA no RAS setup time before WE,
       which is marginal on some DRAM bits depending on ULA clock phase. */
    GPIO_ResetBits (GPIOB, ZX_PIN_MREQ);

    if (!ZX_WaitClockToggle (ZX_BUS_TIMEOUT)) {
        GPIO_SetBits (GPIOB, ZX_CTRL_MASK);
        ZX_DataBusInput();
        return 0;
    }

    /* One toggle after MREQ, assert WR — matches Z80 T2 (data + WE valid) */
    GPIO_ResetBits (GPIOB, ZX_PIN_WR);

    if (!ZX_WaitClockToggle (ZX_BUS_TIMEOUT)) {
        GPIO_SetBits (GPIOB, ZX_CTRL_MASK);
        ZX_DataBusInput();
        return 0;
    }
    if (!ZX_WaitClockToggle (ZX_BUS_TIMEOUT)) {
        GPIO_SetBits (GPIOB, ZX_CTRL_MASK);
        ZX_DataBusInput();
        return 0;
    }

    /* Deassert both — DRAM has had ample write hold time */
    GPIO_SetBits (GPIOB, ZX_PIN_MREQ | ZX_PIN_WR);
    ZX_DataBusInput();
    return 1;
}

int ZX_BusReadBlock (uint16_t address, uint8_t *buffer, uint16_t length) {
    uint16_t i;

    if ((buffer == NULL) && (length != 0u)) {
        return 0;
    }

    if (!ZX_BusAcquire()) {
        return 0;
    }

    for (i = 0u; i < length; ++i) {
        if (!ZX_BusReadCycle ((uint16_t)(address + i), &buffer[i])) {
            ZX_BusRelease();
            return 0;
        }
    }

    ZX_BusRelease();
    return 1;
}

int ZX_BusWriteBlock (uint16_t address, const uint8_t *buffer, uint16_t length) {
    uint16_t i;

    if ((buffer == NULL) && (length != 0u)) {
        return 0;
    }

    if (!ZX_BusAcquire()) {
        return 0;
    }

    for (i = 0u; i < length; ++i) {
        if (!ZX_BusWriteCycle ((uint16_t)(address + i), buffer[i])) {
            ZX_BusRelease();
            return 0;
        }
    }

    ZX_BusRelease();
    return 1;
}

int ZX_CartRamReadBlock (uint16_t address, uint8_t *buffer, uint16_t length) {
    uint16_t i;

    if (length == 0u) {
        return 1;
    }

    if ((buffer == NULL) && (length != 0u)) {
        return 0;
    }

    if ((address < state_pointer->RamBase) ||
        ((uint32_t)address + length - 1u > state_pointer->RomLast)) {
        return 0;
    }

    NVIC_DisableIRQ (EXTI15_10_IRQn);
    for (i = 0u; i < length; ++i) {
        buffer[i] = state_pointer->ram[(uint16_t)(address - state_pointer->RamBase + i)];
    }
    NVIC_EnableIRQ (EXTI15_10_IRQn);
    return 1;
}

int ZX_CartRamWriteBlock (uint16_t address, const uint8_t *buffer, uint16_t length) {
    uint16_t i;

    if (length == 0u) {
        return 1;
    }

    if ((buffer == NULL) && (length != 0u)) {
        return 0;
    }

    if ((address < state_pointer->RamBase) ||
        ((uint32_t)address + length - 1u > state_pointer->RomLast)) {
        return 0;
    }

    NVIC_DisableIRQ (EXTI15_10_IRQn);
    for (i = 0u; i < length; ++i) {
        state_pointer->ram[(uint16_t)(address - state_pointer->RamBase + i)] = buffer[i];
    }
    NVIC_EnableIRQ (EXTI15_10_IRQn);
    return 1;
}

int ZX_KeyPoll (uint8_t *keycode_out) {
    uint8_t seq;
    uint8_t code;

    if (keycode_out == NULL) {
        return -1;
    }

    if (!ZX_CartRamReadBlock (ZX_KEY_SEQ_ADDR, &seq, 1u)) {
        return -1;
    }
    if (seq == s_key_last_seq) {
        return 0;
    }
    if (!ZX_CartRamReadBlock (ZX_KEY_CODE_ADDR, &code, 1u)) {
        return -1;
    }

    s_key_last_seq = seq;
    *keycode_out = code;
    return 1;
}

void ZX_TriggerNMI (void) {
    (void)ZX_WaitClockToggle (ZX_BUS_TIMEOUT);
    GPIO_ResetBits (GPIOB, ZX_PIN_INT);
    Delay_Us (40u);
    GPIO_SetBits (GPIOB, ZX_PIN_INT);
}

/* Acquire bus and return number of clock toggles waited, or 0 on failure. */
int ZX_BusAcquireDbg (uint32_t *cycles_out) {
    uint32_t timeout = ZX_BUS_TIMEOUT;
    uint32_t waited = 0u;

    if (cycles_out != NULL) {
        *cycles_out = 0u;
    }

    /* Same BUSREQ-first policy as ZX_BusAcquire: keep NVIC enabled until BUSACK
       so the cart ISR can serve Z80 ROM reads during the handshake. */
    ZX_CtrlLinesInput();
    ZX_AddrBusInput();
    ZX_DataBusInput();

    GPIO_ResetBits (GPIOB, ZX_PIN_BUSREQ);

    while (timeout-- > 0u) {
        if ((GPIOB->INDR & ZX_PIN_BUSACK) == 0u) {
            NVIC_DisableIRQ (EXTI15_10_IRQn);
            EXTI->INTENR &= ~EXTI_Line10;
            EXTI->INTFR = EXTI_Line10;
            ZX_AddrBusOutput();
            ZX_CtrlLinesOutput();
            if (cycles_out != NULL) {
                *cycles_out = waited;
            }
            return 1;
        }
        (void)ZX_WaitClockToggle (512u);
        ++waited;
    }

    GPIO_SetBits (GPIOB, ZX_PIN_BUSREQ);
    return 0;
}

void ZX_BusReleaseDbg (void) {
    ZX_BusRelease();
}

/* Write one byte, read it back immediately, restore original. Returns 1 if write matched. */
int ZX_BusWriteReadVerify (uint16_t address, uint8_t value, uint8_t *readback_out) {
    uint8_t original = 0u;
    uint8_t verify = 0u;

    if (!ZX_BusAcquire()) {
        return -1;
    }

    (void)ZX_BusReadCycle (address, &original);
    (void)ZX_BusWriteCycle (address, value);
    (void)ZX_BusReadCycle (address, &verify);
    (void)ZX_BusWriteCycle (address, original);

    ZX_BusRelease();

    if (readback_out != NULL) {
        *readback_out = verify;
    }
    return (verify == value) ? 1 : 0;
}

/* Write to any ZX address (including ULA-contended 0x4000-0x7FFF) by staging
   data in cart RAM and triggering NMI so the ZX CPU does the actual write.
   Waits for ZX to acknowledge (WCMD_DONE matches seq) up to timeout_ms.
   Max chunk size is ZX_NMI_WCMD_CHUNK bytes; larger blocks are split.
   Returns 1 on success, 0 on timeout/error. */
#define ZX_NMI_WCMD_SEQ_ADDR    0x302Eu
#define ZX_NMI_WCMD_DONE_ADDR   0x302Fu
#define ZX_NMI_WCMD_DST_LO_ADDR 0x3030u
#define ZX_NMI_WCMD_DST_HI_ADDR 0x3031u
#define ZX_NMI_WCMD_LEN_LO_ADDR 0x3032u
#define ZX_NMI_WCMD_LEN_HI_ADDR 0x3033u
#define ZX_NMI_WCMD_DATA_ADDR   0x3034u
#define ZX_NMI_WCMD_CHUNK       0x0200u  /* 512 bytes per NMI chunk */

static uint8_t s_nmi_wcmd_seq = 0u;

int ZX_NmiWriteBlock (uint16_t address, const uint8_t *buffer, uint16_t length, uint32_t timeout_ms) {
    uint16_t offset = 0u;

    if ((buffer == NULL) && (length != 0u)) {
        return 0;
    }

    while (offset < length) {
        uint16_t chunk = (uint16_t)(length - offset);
        uint8_t seq;
        uint32_t waited;
        uint8_t done;
        uint16_t dst = (uint16_t)(address + offset);
        uint8_t dst_lo = (uint8_t)(dst & 0x00FFu);
        uint8_t dst_hi = (uint8_t)(dst >> 8);
        uint8_t len_lo;
        uint8_t len_hi;

        if (chunk > ZX_NMI_WCMD_CHUNK) {
            chunk = ZX_NMI_WCMD_CHUNK;
        }

        len_lo = (uint8_t)(chunk & 0x00FFu);
        len_hi = (uint8_t)(chunk >> 8);
        ++s_nmi_wcmd_seq;
        seq = s_nmi_wcmd_seq;

        /* Write mailbox directly into cart RAM shadow — no NVIC locking.
           NVIC_DisableIRQ would block the cart ISR so the ZX CPU gets no
           response on cart ROM reads, corrupting its execution mid-instruction.
           Single-byte volatile writes are atomic; the SEQ-last protocol ensures
           ZX only acts on the mailbox once all data and control bytes are present. */
        if (state_pointer == NULL) { return 0; }
        {
            uint16_t i;
            for (i = 0u; i < chunk; ++i) {
                state_pointer->ram[(ZX_NMI_WCMD_DATA_ADDR - 0x3000u) + i] = (buffer + offset)[i];
            }
        }
        state_pointer->ram[ZX_NMI_WCMD_DST_LO_ADDR - 0x3000u] = dst_lo;
        state_pointer->ram[ZX_NMI_WCMD_DST_HI_ADDR - 0x3000u] = dst_hi;
        state_pointer->ram[ZX_NMI_WCMD_LEN_LO_ADDR - 0x3000u] = len_lo;
        state_pointer->ram[ZX_NMI_WCMD_LEN_HI_ADDR - 0x3000u] = len_hi;
        /* Compiler barrier: seq write must not be reordered before data/control */
        __asm volatile ("" ::: "memory");
        state_pointer->ram[ZX_NMI_WCMD_SEQ_ADDR - 0x3000u] = seq;

        /* Trigger NMI so ZX executes the write */
        ZX_TriggerNMI();

        /* Poll DONE without NVIC locking — single volatile byte read is atomic */
        waited = 0u;
        do {
            done = state_pointer->ram[ZX_NMI_WCMD_DONE_ADDR - 0x3000u];
            if (done == seq) {
                break;
            }
            Delay_Ms (1u);
            ++waited;
        } while (waited < timeout_ms);

        if (done != seq) {
            return 0;  /* timeout */
        }

        offset = (uint16_t)(offset + chunk);
    }

    return 1;
}

/* Z80-driven read: have zxprog memcpy(SRC, RBUF, LEN) on NMI, then we
   read RBUF directly from cart RAM (no bus contention).  Use this to
   verify what the Z80 actually sees in contended bank, since BUSREQ
   reads of 0x4000-0x7FFF are unreliable due to ULA arbitration. */
#define ZX_NMI_RCMD_SEQ_ADDR    0x3F20u
#define ZX_NMI_RCMD_DONE_ADDR   0x3F21u
#define ZX_NMI_RCMD_SRC_LO_ADDR 0x3F22u
#define ZX_NMI_RCMD_SRC_HI_ADDR 0x3F23u
#define ZX_NMI_RCMD_LEN_ADDR    0x3F24u
#define ZX_NMI_RCMD_BUF_ADDR    0x3F40u
#define ZX_NMI_RCMD_MAX_LEN     64u

static uint8_t s_nmi_rcmd_seq = 0u;

int ZX_NmiReadBlock (uint16_t address, uint8_t *buffer, uint8_t length, uint32_t timeout_ms) {
    uint8_t seq;
    uint32_t waited;
    uint8_t done;
    uint8_t src_lo = (uint8_t)(address & 0x00FFu);
    uint8_t src_hi = (uint8_t)(address >> 8);
    uint16_t i;

    if ((buffer == NULL) || (length == 0u) || (length > ZX_NMI_RCMD_MAX_LEN)) {
        return 0;
    }
    if (state_pointer == NULL) { return 0; }

    ++s_nmi_rcmd_seq;
    seq = s_nmi_rcmd_seq;

    state_pointer->ram[ZX_NMI_RCMD_SRC_LO_ADDR - 0x3000u] = src_lo;
    state_pointer->ram[ZX_NMI_RCMD_SRC_HI_ADDR - 0x3000u] = src_hi;
    state_pointer->ram[ZX_NMI_RCMD_LEN_ADDR    - 0x3000u] = length;
    __asm volatile ("" ::: "memory");
    state_pointer->ram[ZX_NMI_RCMD_SEQ_ADDR    - 0x3000u] = seq;

    ZX_TriggerNMI();

    waited = 0u;
    do {
        done = state_pointer->ram[ZX_NMI_RCMD_DONE_ADDR - 0x3000u];
        if (done == seq) {
            break;
        }
        Delay_Ms (1u);
        ++waited;
    } while (waited < timeout_ms);

    if (done != seq) {
        return 0;
    }

    for (i = 0u; i < length; ++i) {
        buffer[i] = state_pointer->ram[(ZX_NMI_RCMD_BUF_ADDR - 0x3000u) + i];
    }
    return 1;
}

/* Cart-side control flags mailbox (0x3F00 in cart RAM, offset 0x0F00 from RamBase 0x3000).
   Set CTRL_DRAW_SUSPEND to prevent ZX main loop from calling draw_bridge_screen()
   (which does memset(0x4000,0,6144)), allowing CH32 to write/verify screen RAM. */
#define ZX_CTRL_FLAGS_OFFSET  0x0F02u  /* 0x3F02 - 0x3000; avoids BSS at 0x3F00-0x3F01 */
#define ZX_CTRL_DRAW_SUSPEND  0x01u

void ZX_CartDrawSuspend (void) {
    if (state_pointer != NULL) {
        state_pointer->ram[ZX_CTRL_FLAGS_OFFSET] |= ZX_CTRL_DRAW_SUSPEND;
    }
}

void ZX_CartDrawResume (void) {
    if (state_pointer != NULL) {
        state_pointer->ram[ZX_CTRL_FLAGS_OFFSET] &= (uint8_t)~ZX_CTRL_DRAW_SUSPEND;
        /* bridge_dirty was kept set while suspended, so ZX will redraw immediately */
    }
}

//
//  Config Cart emulation hardware.
void Init_Cart() {
    GPIO_InitTypeDef gpio = {0};

    FLASH_Enhance_Mode (ENABLE);

    state_pointer = &s_state;
    state_pointer->BusOn = 0x33333333u;
    state_pointer->BusOff = 0x44444444u;
    state_pointer->DataMask = 0x00FFu;
    state_pointer->RamBase = 0x3000u;
    state_pointer->RomLast = ZX_ROM_LAST_ADDRESS;
    state_pointer->PinRD = GPIO_Pin_5;
    state_pointer->PinWR = GPIO_Pin_4;
    state_pointer->PinMREQ = GPIO_Pin_10;
    state_pointer->IRQLine = EXTI_Line10;

    gpio.GPIO_Pin = ZX_PIN_BUSACK | ZX_PIN_CK_INV;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init (GPIOB, &gpio);

    gpio.GPIO_Pin = ZX_PIN_BUSREQ | ZX_PIN_INT;
    gpio.GPIO_Mode = GPIO_Mode_Out_OD;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init (GPIOB, &gpio);
    GPIO_SetBits (GPIOB, ZX_PIN_BUSREQ | ZX_PIN_INT);

    /* ROMCS as push-pull output. Default HIGH = cart ROM asserted (the level
       that has been working in practice while zxprog runs normally).
       ZX_RomcsRelease() drives LOW to disable the cart and let the internal
       Spectrum ROM respond to 0x0000-0x3FFF. */
    gpio.GPIO_Pin = GPIO_Pin_3;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init (GPIOB, &gpio);
    GPIO_SetBits (GPIOB, GPIO_Pin_3);

    ZX_CtrlLinesInput();
    ZX_DataBusInput();
    ZX_AddrBusInput();

    EXTI->INTFR = state_pointer->IRQLine;
    SetVTFIRQ ((u32)RunCartWithRAM, EXTI15_10_IRQn, 0, ENABLE);
    NVIC_EnableIRQ (EXTI15_10_IRQn);

    (void)ZX_CartRamReadBlock (ZX_KEY_SEQ_ADDR, &s_key_last_seq, 1u);
}

void RunCart16k (void) {
    if ((GPIOB->INDR & GPIO_Pin_5) == 0) { //Check for RD active (active low)

        uint16_t address = (uint16_t)GPIOE->INDR;
        if (address <= ZX_ROM_LAST_ADDRESS) { // Only respond to addresses within the ZX ROM range
            GPIOD->CFGLR = 0x33333333; // Set GPIOD pins 0-7 as output
            GPIOD->OUTDR = (GPIOD->OUTDR & ~0x00FFu) | g_zx_image[address]; // Output data to GPIOD pins 0-7
            while (((GPIOB->INDR & GPIO_Pin_5) == 0)) { }; // Wait until RD goes inactive
            GPIOD->CFGLR = 0x44444444; // Set GPIOD pins 0-7 back to input
            EXTI->INTFR = EXTI_Line10; // Clear the interrupt flag for EXTI line 10 to allow the next interrupt to be triggered
        }
    }

    return;
}


/* ── RunCartWithRAM: RISC-V assembly for minimum bus-service latency. ──────
 *
 * Why assembly: this is the steady-state cart ISR used on every MREQ edge,
 * so it must be as short as possible.  Handover checks are intentionally NOT
 * in this function; they run only in RunCartWithM1Watch during launch.
 *
 * GPIO/peripheral base addresses and offsets (hardcoded — no struct loads):
 *   GPIOB  0x40010C00  +0x08 INDR  +0x14 BCR
 *   GPIOD  0x40011400  +0x00 CFGLR +0x08 INDR +0x0C OUTDR
 *   GPIOE  0x40011800  +0x08 INDR  (address bus)
 *   EXTI   0x40010400  +0x14 INTFR
 *     EXTI_INTFR = GPIOB_base + (-0x7EC) = 0x40010414
 *     GPIOB_BCR  = GPIOB_base + 0x14    = 0x40010C14
 *
 * Register allocation (all caller-saved; saved by hardware on ISR entry):
 *   t1  Z80 address (GPIOE INDR[15:0])
 *   t2  GPIOB INDR snapshot / scratch
 *   t3  scratch / comparison value
 *   t4  data byte / temp
 *   t5  GPIOD OUTDR preload / WR store address
 *   t6  GPIOB base  0x40010C00  (kept throughout hot path)
 *   a0  GPIOD base  0x40011400  (kept throughout hot path)
 */
__attribute__((interrupt ("WCH-Interrupt-fast"))) void RunCartWithRAM (void) {
    __asm__ (

    /* ── Load fixed bases; read address bus ── */
    "   li      t6, 0x40010C00          \n" /* GPIOB base */
    "   li      a0, 0x40011400          \n" /* GPIOD base */
    "   li      t0, 0x40011808          \n" /* GPIOE INDR address */
    "   lhu     t1, 0(t0)               \n" /* t1 = address[15:0] */

    /* ── Normal path ── */
    ".Lrca_normal:                      \n"
    "   la      t0, s_rom_released      \n"
    "   lhu     t2, 8(t6)               \n" /* GPIOB INDR */
    "   lw      t3, 0(t0)               \n" /* s_rom_released */
    "   bnez    t3, .Lrca_exit          \n" /* ROMCS released → just exit */
    "   andi    t3, t2, 0x20            \n" /* isolate PinRD (bit 5) */
    "   bnez    t3, .Lrca_qualify      \n" /* RD may still be high at MREQ edge */

    /* ── Read cycle ── */
    ".Lrca_read_entry:                  \n"
    "   lui     t3, 3                   \n" /* t3 = 0x3000 = RamBase */
    "   bltu    t1, t3, .Lrca_rom       \n" /* addr < 0x3000 → ROM image */
    "   lui     t3, 4                   \n" /* t3 = 0x4000 */
    "   bgeu    t1, t3, .Lrca_exit      \n" /* addr >= 0x4000 → out of range */

    /* ── Cart RAM read: 0x3000 ≤ addr ≤ 0x3FFF ── */
    "   lui     t3, 3                   \n"
    "   sub     t4, t1, t3              \n" /* t4 = addr - 0x3000 (byte offset) */
    "   la      t0, state_pointer       \n"
    "   lhu     t5, 0xC(a0)             \n" /* preload GPIOD OUTDR while pointer loads */
    "   lw      t0, 0(t0)               \n" /* t0 = ZXCartState* */
    "   addi    t0, t0, 28              \n" /* &sp->ram[0] */
    "   add     t0, t0, t4              \n" /* &sp->ram[offset] */
    "   lbu     t4, 0(t0)               \n" /* t4 = data byte */
    "   andi    t5, t5, -256            \n" /* clear OUTDR[7:0] */
    "   or      t5, t5, t4              \n" /* merge data byte */
    "   sw      t5, 0xC(a0)             \n" /* GPIOD OUTDR = value (still input mode → no glitch) */
    "   li      t3, 0x33333333          \n"
    "   sw      t3, 0(a0)               \n" /* GPIOD CFGLR = BusOn → data drives pins */
    ".Lrca_ram_wait:                    \n"
    "   lhu     t2, 8(t6)               \n"
    "   andi    t2, t2, 0x20            \n"
    "   beqz    t2, .Lrca_ram_wait      \n" /* wait until RD goes high */
    "   li      t3, 0x44444444          \n"
    "   sw      t3, 0(a0)               \n" /* GPIOD CFGLR = BusOff (tristate) */
    "   j       .Lrca_exit              \n"

    /* ── ROM image read: addr < 0x3000 ── */
    ".Lrca_rom:                         \n"
    "   la      t0, g_zx_image          \n"
    "   lhu     t5, 0xC(a0)             \n" /* preload GPIOD OUTDR */
    "   add     t0, t0, t1              \n" /* &g_zx_image[addr] */
    "   lbu     t4, 0(t0)               \n" /* t4 = data byte */
    "   andi    t5, t5, -256            \n"
    "   or      t5, t5, t4              \n"
    "   sw      t5, 0xC(a0)             \n" /* GPIOD OUTDR */
    "   li      t3, 0x33333333          \n"
    "   sw      t3, 0(a0)               \n" /* GPIOD CFGLR = BusOn */
    ".Lrca_rom_wait:                    \n"
    "   lhu     t2, 8(t6)               \n"
    "   andi    t2, t2, 0x20            \n"
    "   beqz    t2, .Lrca_rom_wait      \n" /* wait until RD goes high */
    "   li      t3, 0x44444444          \n"
    "   sw      t3, 0(a0)               \n" /* GPIOD CFGLR = BusOff */
    "   j       .Lrca_exit              \n"

    /* ── Qualify cycle type while MREQ is active ── */
    ".Lrca_qualify:                     \n"
    ".Lrca_qualify_wait:                \n"
    "   lhu     t2, 8(t6)               \n"
    "   andi    t3, t2, 0x400           \n" /* PinMREQ (bit 10) */
    "   bnez    t3, .Lrca_exit          \n" /* MREQ de-asserted → ignore */
    "   andi    t3, t2, 0x20            \n" /* PinRD (bit 5), active low */
    "   beqz    t3, .Lrca_read_entry    \n" /* it's a read cycle */
    "   andi    t3, t2, 0x10            \n" /* PinWR (bit 4), active low */
    "   bnez    t3, .Lrca_qualify_wait  \n" /* wait for RD or WR assertion */

    /* WR asserted while MREQ low → write cycle; check address in cart RAM range */
    ".Lrca_wr_start:                    \n"
    "   lui     t3, 3                   \n"
    "   bltu    t1, t3, .Lrca_exit      \n" /* addr < 0x3000 → ignore */
    "   lui     t3, 4                   \n"
    "   bgeu    t1, t3, .Lrca_exit      \n" /* addr >= 0x4000 → ignore */
    "   lui     t3, 3                   \n"
    "   sub     t4, t1, t3              \n" /* t4 = addr - 0x3000 */
    "   la      t0, state_pointer       \n"
    "   lw      t0, 0(t0)               \n"
    "   addi    t0, t0, 28              \n"
    "   add     t5, t0, t4              \n" /* t5 = &sp->ram[offset] */
    "   li      t3, 0x44444444          \n"
    "   sw      t3, 0(a0)               \n" /* GPIOD CFGLR = BusOff (input for WR) */
    ".Lrca_wr_data:                     \n"
    "   lhu     t2, 8(t6)               \n"
    "   lhu     t3, 8(a0)               \n" /* GPIOD INDR: Z80 data bus */
    "   andi    t3, t3, 0xFF            \n"
    "   sb      t3, 0(t5)               \n" /* sp->ram[offset] = data */
    "   andi    t2, t2, 0x10            \n" /* PinWR */
    "   beqz    t2, .Lrca_wr_data       \n" /* loop while WR remains low */

    /* ── Clear EXTI pending flag and return ── */
    ".Lrca_exit:                        \n"
    "   addi    t0, t6, -0x7EC          \n" /* t0 = 0x40010C00 - 0x7EC = 0x40010414 = EXTI INTFR */
    "   li      t3, 0x400               \n" /* EXTI_Line10 */
    "   sw      t3, 0(t0)               \n"
    "   mret                            \n"

    ); /* end __asm__ */
}

/* Launch-window ISR in RISC-V asm.
   Fast path: if no handover match, jump directly to RunCartWithRAM.
   Slow path: on address match, verify launcher alive marker (0x3FAA=0xAA),
   then drop ROMCS and signal handover fired. */
__attribute__((interrupt ("WCH-Interrupt-fast"))) void RunCartWithM1Watch (void) {
    __asm__ (
    "   li      t6, 0x40010C00          \n" /* GPIOB base */
    "   li      t0, 0x40011808          \n" /* GPIOE INDR address */
    "   lhu     t1, 0(t0)               \n" /* t1 = address */

     /* Only attempt handover on opcode fetch timing: M1 low.
         EXTI triggers on MREQ falling edge, which can precede RD going low. */
    "   lhu     t2, 8(t6)               \n" /* GPIOB INDR */
    "   andi    t3, t2, 0x200           \n" /* PinM1 (bit 9), active low */
    "   bnez    t3, .Lrwm_to_ram        \n"

    /* If handover is not armed, use normal fast cart service immediately. */
    "   la      t2, s_handover_armed    \n"
    "   lw      t3, 0(t2)               \n"
    "   beqz    t3, .Lrwm_to_ram        \n"

    /* Match primary handover address. */
    "   la      t2, s_handover_addr     \n"
    "   lhu     t3, 0(t2)               \n"
    "   beq     t1, t3, .Lrwm_check_alive \n"

    /* Match optional secondary handover address. */
    "   la      t2, s_handover_addr_b   \n"
    "   lhu     t3, 0(t2)               \n"
    "   li      t4, 0xFFFF              \n"
    "   beq     t3, t4, .Lrwm_to_ram    \n"
    "   bne     t1, t3, .Lrwm_to_ram    \n"

    /* Address matched. Require launcher alive marker before handover. */
    ".Lrwm_check_alive:                 \n"
    "   la      t0, state_pointer       \n"
    "   lw      t0, 0(t0)               \n"
    "   lui     t3, 1                   \n" /* 0x1000 */
    "   add     t0, t0, t3              \n"
    "   lbu     t4, -58(t0)             \n" /* sp->ram[0x0FAA] */
    "   li      t3, 0xAA                \n"
    "   bne     t4, t3, .Lrwm_to_ram    \n"

    /* Handover fire: ROMCS LOW + flags + EXTI clear + return. */
    "   addi    t0, t6, 0x14            \n" /* GPIOB BCR */
    "   li      t3, 8                   \n" /* GPIO_Pin_3 */
    "   sw      t3, 0(t0)               \n"
    "   la      t0, s_rom_released      \n"
    "   li      t3, 1                   \n"
    "   sw      t3, 0(t0)               \n"
    "   la      t0, s_handover_armed    \n"
    "   sw      zero, 0(t0)             \n"
    "   la      t0, s_handover_fired_addr \n"
    "   sh      t1, 0(t0)               \n"
    "   la      t0, s_handover_fired    \n"
    "   li      t3, 1                   \n"
    "   sw      t3, 0(t0)               \n"
    "   addi    t0, t6, -0x7EC          \n" /* EXTI INTFR */
    "   li      t3, 0x400               \n" /* EXTI_Line10 */
    "   sw      t3, 0(t0)               \n"
    "   mret                            \n"

    /* Normal service: jump to the steady-state fast ISR (ends with mret). */
    ".Lrwm_to_ram:                      \n"
    "   j       RunCartWithRAM          \n"
    );
}

/* Pure-observer ISR.  Triggered on EXTI line 10 (MREQ falling edge).
   ROMCS is already floated so we don't drive the data bus \u2014 we just
   sample address/data/control and store one entry per MREQ event.  Stops
   recording when the buffer is full so the most-interesting first
   instructions after handover survive even if the trace runs longer. */
void ZX_BusTraceISR (void) {
    EXTI->INTFR = EXTI_Line10;
    if (s_trace_full) {
        return;
    }
    {
        uint16_t addr = (uint16_t)GPIOE->INDR;
        uint16_t bport = (uint16_t)GPIOB->INDR;
        uint8_t data = (uint8_t)(GPIOD->INDR & 0xFFu);
        uint8_t ctrl = 0u;
        if (bport & ZX_PIN_M1)   ctrl |= 0x01u;
        if (bport & GPIO_Pin_5)  ctrl |= 0x02u;   /* RD */
        if (bport & GPIO_Pin_4)  ctrl |= 0x04u;   /* WR */
        s_trace[s_trace_head].addr = addr;
        s_trace[s_trace_head].data = data;
        s_trace[s_trace_head].ctrl = ctrl;
        s_trace_head = (uint16_t)(s_trace_head + 1u);
        if (s_trace_head >= ZX_TRACE_DEPTH) {
            s_trace_full = 1u;
        }
    }
}

#pragma GCC pop_options

void ZX_BusTraceArm (void) {
    s_trace_head = 0u;
    s_trace_full = 0u;
}

void ZX_BusTraceStop (void) {
    /* Nothing to do \u2014 sampler is synchronous. */
}

/* Synchronous bus sampler: polls MREQ falling edges in a tight loop and
   stores up to ZX_TRACE_DEPTH samples or until cycles_budget loop
   iterations elapse.  EXTI is unreliable here because the cart edge
   pin configurations (and NVIC state) get reshuffled around handover;
   this approach has no setup cost. */
void ZX_BusTraceSample (uint32_t loop_budget) {
    uint16_t prev = (uint16_t)(GPIOB->INDR & GPIO_Pin_10);
    while ((loop_budget != 0u) && !s_trace_full) {
        uint16_t cur = (uint16_t)(GPIOB->INDR & GPIO_Pin_10);
        if ((prev != 0u) && (cur == 0u)) {
            /* MREQ falling edge */
            uint16_t addr = (uint16_t)GPIOE->INDR;
            uint16_t bport = (uint16_t)GPIOB->INDR;
            uint8_t data = (uint8_t)(GPIOD->INDR & 0xFFu);
            uint8_t ctrl = 0u;
            if (bport & ZX_PIN_M1)  ctrl |= 0x01u;
            if (bport & GPIO_Pin_5) ctrl |= 0x02u;   /* RD */
            if (bport & GPIO_Pin_4) ctrl |= 0x04u;   /* WR */
            s_trace[s_trace_head].addr = addr;
            s_trace[s_trace_head].data = data;
            s_trace[s_trace_head].ctrl = ctrl;
            s_trace_head = (uint16_t)(s_trace_head + 1u);
            if (s_trace_head >= ZX_TRACE_DEPTH) {
                s_trace_full = 1u;
            }
        }
        prev = cur;
        --loop_budget;
    }
}

void ZX_BusTraceDump (void) {
    uint16_t n = s_trace_full ? ZX_TRACE_DEPTH : s_trace_head;
    uint16_t i;
    printf ("z80: bus trace (%u events, %s):\r\n",
            (unsigned)n, s_trace_full ? "BUFFER FULL" : "partial");
    for (i = 0u; i < n; ++i) {
        uint8_t c = s_trace[i].ctrl;
        printf ("  %3u: %04X = %02X  M1=%u RD=%u WR=%u%s\r\n",
                (unsigned)i,
                (unsigned)s_trace[i].addr,
                (unsigned)s_trace[i].data,
                (c & 1u) ? 1u : 0u,
                (c & 2u) ? 1u : 0u,
                (c & 4u) ? 1u : 0u,
                ((c & 1u) == 0u) ? "  <- M1 fetch" : "");
    }
}

/* ===== ROMCS control + launch sequence ============================== */

#define ZX_PIN_ROMCS GPIO_Pin_3

void ZX_RomcsAssert (void) {
    /* Restore the idle config used while zxprog is running.  Address bus is
       sampled by the cart ISR (input), data bus is input-default (only
       driven during a RD response), control lines are floating inputs. */
    GPIO_InitTypeDef gpio = {0};
    s_rom_released = 0;
    ZX_AddrBusInput();
    ZX_DataBusInput();
    ZX_CtrlLinesInput();
    /* Re-drive ROMCS as push-pull HIGH (cart ROM selected). */
    gpio.GPIO_Pin = ZX_PIN_ROMCS;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init (GPIOB, &gpio);
    GPIO_SetBits (GPIOB, ZX_PIN_ROMCS);
    /* Re-arm the EXTI vector and unmask the line so the cart ISR runs again. */
    SetVTFIRQ ((u32)RunCartWithRAM, EXTI15_10_IRQn, 0, ENABLE);
    EXTI->INTFR = EXTI_Line10;
    EXTI->INTENR |= EXTI_Line10;
    NVIC_EnableIRQ (EXTI15_10_IRQn);
}

/* Reset the Z80 by pulling /RESET LOW via GPIOC Pin 6 (open-drain).
   ROMCS must already be HIGH (ZX_RomcsAssert) so the Z80 starts executing
   zxprog at 0x0000 after the reset pulse.  After reset, waits for zxprog
   to complete startup (screen clear LDIR ~36ms + init ~20ms) before
   returning, so the polling loop is live when the caller proceeds. */
void ZX_Z80Reset (void) {
    GPIO_InitTypeDef gpio = {0};
    /* Drive /RESET LOW as open-drain (OD): pulls the Spectrum /RESET line low,
       causing the Z80 to reset without conflicting with the RC pull-up. */
    gpio.GPIO_Pin  = GPIO_Pin_6;
    gpio.GPIO_Mode = GPIO_Mode_Out_OD;
    gpio.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init (GPIOC, &gpio);
    GPIO_ResetBits (GPIOC, GPIO_Pin_6);   /* pull /RESET LOW */
    Delay_Ms (80u);                        /* hold for 80 ms */
    GPIO_SetBits (GPIOC, GPIO_Pin_6);     /* release (OD → floats HIGH via pull-up) */
    /* Return pin to input pull-up (passive monitoring). */
    gpio.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init (GPIOC, &gpio);
     /* zxprog now clears 48K RAM at startup; allow enough time for that
         work to complete before host-side loaders start mailbox traffic. */
     Delay_Ms (1200u);
}

void ZX_RomcsRelease (void) {
    /* Stop responding to ZX RD/WR cycles BEFORE clearing the ROMCS pin so the
       internal Spectrum ROM is the only one driving the data bus.  Float
       every signal we have on the cart edge so the Spectrum CPU sees a
       cleanly empty slot (cart absent). */
    GPIO_InitTypeDef gpio = {0};
    s_rom_released = 1;
    EXTI->INTENR &= ~EXTI_Line10;
    NVIC_DisableIRQ (EXTI15_10_IRQn);
    EXTI->INTFR = EXTI_Line10;
    /* Disarm the VTF entry too — even with NVIC disabled and the EXTI mask
       cleared, leaving the VTF address loaded means the core can short-circuit
       to the cart ISR if any stray edge sneaks in. */
    SetVTFIRQ ((u32)RunCartWithRAM, EXTI15_10_IRQn, 0, DISABLE);

    /* Tristate ROMCS rather than driving low — the cart PCB / edge pin can
       have its own bias.  Letting it float lets the host pull it to its
       natural inactive level (cart absent). */
    gpio.GPIO_Pin = ZX_PIN_ROMCS;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init (GPIOB, &gpio);

    ZX_DataBusInput();
    ZX_AddrBusInput();
    ZX_CtrlLinesInput();
    /* Tristate BUSREQ and NMI as well — both are open-drain so floating them
       leaves the bus pull-ups to drive the inactive (HIGH) level naturally,
       and the Spectrum sees no MCU influence. */
    gpio.GPIO_Pin = ZX_PIN_BUSREQ | ZX_PIN_INT;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init (GPIOB, &gpio);
}

int ZX_RomcsIsReleased (void) {
    return s_rom_released;
}

/* Launch mailbox in cart RAM (must match LAUNCH_*_ADDR in zxprog.c) */
/* Cart-RAM launcher mailbox offsets (must match zxprog.c / z80_loader.h) */
#define ZX_LAUNCHER_REGBLOCK_ADDR    0x3F90u
#define ZX_LAUNCHER_REGBLOCK_LEN     26u
#define ZX_LAUNCHER_TAIL_ADDR        0x3FF0u
#define ZX_LAUNCHER_TAIL_LEN         6u
#define ZX_LAUNCHER_ALIVE_ADDR       0x3FAAu
#define ZX_LAUNCHER_PHASE_ADDR       0x3FABu
#define ZX_LAUNCHER_PHASE_READY      0xC3u
#define ZX_LAUNCH_TRIGGER_ADDR       0x3F10u
#define ZX_LAUNCH_TRIGGER_GO         0x55u

/* Build and write a clean-Spectrum-state regblock to 0x3F80 and a
   launcher tail to 0x3FF0, then arm ZX_SnapshotCommitDual to watch
   start_addr and 0x0038 (IM1+IFF1).  Returns 1 on success. */
int ZX_LaunchZ80 (uint16_t start_addr) {
    uint8_t regblock[ZX_LAUNCHER_REGBLOCK_LEN];
    uint8_t tail[ZX_LAUNCHER_TAIL_LEN];
    uint8_t zero = 0u;

    if (state_pointer == NULL) { return 0; }

    /* Launch must begin with cart ROM actively serving reads. */
    ZX_RomcsAssert();

    /* Clean Spectrum register state.  Initialisation matches what the ROM
       produces after a clean RESET; enough for most standalone code blocks.
       R_comp = (0 - 12) & 0x7F = 0x74 (12 M1 cycles from LD R,A to JP). */
    __builtin_memset (regblock, 0, ZX_LAUNCHER_REGBLOCK_LEN);
    regblock[10] = 0x3Au; regblock[11] = 0x5Cu;  /* IY = 0x5C3A */
    regblock[13] = 0x3Fu;                         /* I = 0x3F */
    regblock[14] = 4u;                            /* border = 4 (green) */
    regblock[15] = 0x74u;                         /* R_comp = (0-12)&0x7F */
    regblock[24] = 0x40u; regblock[25] = 0xFFu;  /* SP = 0xFF40 */

    /* Launcher tail: IM 1, EI, JP start_addr */
    tail[0] = 0xEDu;
    tail[1] = 0x56u;   /* IM 1 */
    tail[2] = 0xFBu;   /* EI */
    tail[3] = 0xC3u;   /* JP */
    tail[4] = (uint8_t)(start_addr & 0xFFu);
    tail[5] = (uint8_t)(start_addr >> 8);

    if (!ZX_CartRamWriteBlock (ZX_LAUNCHER_TAIL_ADDR, tail,
                               ZX_LAUNCHER_TAIL_LEN)) {
        printf ("launch: tail write failed\r\n");
        return 0;
    }
    if (!ZX_CartRamWriteBlock (ZX_LAUNCHER_REGBLOCK_ADDR, regblock,
                               ZX_LAUNCHER_REGBLOCK_LEN)) {
        printf ("launch: regblock write failed\r\n");
        return 0;
    }
    (void)ZX_CartRamWriteBlock (ZX_LAUNCHER_ALIVE_ADDR, &zero, 1u);

    /* Arm M1 watch at start_addr and 0x0038 (IM1 interrupt vector).
       ZX_SnapshotCommitDual writes the trigger byte directly to cart RAM
       (go_in_cart path — no BUSREQ needed). */
    return ZX_SnapshotCommitDual (start_addr, 0x0038u,
                                  ZX_LAUNCH_TRIGGER_ADDR,
                                  ZX_LAUNCH_TRIGGER_GO,
                                  500u);
}

/* ===== Snapshot launch (used by .z80 loader and launch_test) ======= */

/* Redirect the Z80 to a trampoline already written to ZX RAM.
   Uses the new cart-RAM launcher: writes a neutral regblock at 0x3F80 and
   a launcher tail at 0x3FF0 that jumps to tramp_addr, then triggers
   LAUNCH_TRIGGER (0x3F10 = 0x55).  zx_launcher restores registers and
   executes the tail.  After that ZX_SnapshotCommitDual can be called for
   the M1-handover once the trampoline's spin-loop fires. */
int ZX_SnapshotEnter (uint16_t tramp_addr, uint16_t alive_addr,
                      uint8_t alive_value, uint32_t timeout_ms) {
    uint8_t regblock[ZX_LAUNCHER_REGBLOCK_LEN];
    uint8_t tail[ZX_LAUNCHER_TAIL_LEN];
    uint8_t go = ZX_LAUNCH_TRIGGER_GO;
    uint8_t alive = 0u;
    uint32_t waited = 0u;
    const uint32_t poll_period_ms = 5u;
    int alive_in_cart = 0;

    if (state_pointer == NULL) { return 0; }

    /* Ensure launcher/trigger is visible to Z80 even after prior romcs off. */
    ZX_RomcsAssert();

    /* Neutral regblock: all registers zero except IY=0x5C3A, I=0x3F,
       SP=0xFF40, border=7, R_comp for 12 M1 cycles.  Good enough for the
       test trampolines in launch_test.c which restore their own state. */
    __builtin_memset (regblock, 0, ZX_LAUNCHER_REGBLOCK_LEN);
    regblock[10] = 0x3Au; regblock[11] = 0x5Cu;  /* IY = 0x5C3A */
    regblock[13] = 0x3Fu;                         /* I = 0x3F */
    regblock[14] = 4u;                            /* border = green */
    regblock[15] = 0x74u;                         /* R_comp = (0-12)&0x7F */
    regblock[24] = 0x40u; regblock[25] = 0xFFu;  /* SP = 0xFF40 */

    /* Tail: IM 1, EI, JP tramp_addr */
    tail[0] = 0xEDu; tail[1] = 0x56u;   /* IM 1 */
    tail[2] = 0xFBu;                     /* EI */
    tail[3] = 0xC3u;                     /* JP */
    tail[4] = (uint8_t)(tramp_addr & 0xFFu);
    tail[5] = (uint8_t)(tramp_addr >> 8);

    if (!ZX_CartRamWriteBlock (ZX_LAUNCHER_TAIL_ADDR, tail,
                               ZX_LAUNCHER_TAIL_LEN)) { return 0; }
    if (!ZX_CartRamWriteBlock (ZX_LAUNCHER_REGBLOCK_ADDR, regblock,
                               ZX_LAUNCHER_REGBLOCK_LEN)) { return 0; }

    /* Fire: LAUNCH_TRIGGER = 0x55 triggers zx_launcher in zxprog */
    if (!ZX_CartRamWriteBlock (ZX_LAUNCH_TRIGGER_ADDR, &go, 1u)) { return 0; }

    alive_in_cart = ((alive_addr >= state_pointer->RamBase) &&
                     (alive_addr <= state_pointer->RomLast));

    while (waited < timeout_ms) {
        Delay_Ms (poll_period_ms);
        waited += poll_period_ms;
        if (alive_in_cart) {
            if (!ZX_CartRamReadBlock (alive_addr, &alive, 1u)) { continue; }
        } else if (!ZX_BusReadBlock (alive_addr, &alive, 1u)) {
            continue;
        }
        if (alive == alive_value) {
            printf ("snap: trampoline alive after %lums\r\n", (unsigned long)waited);
            return 1;
        }
    }
    printf ("snap: trampoline did not come alive (alive=%02X target=%04X)\r\n",
            alive, (unsigned)tramp_addr);
    return 0;
}

/* Arm M1 handover so the cart ISR atomically drops ROMCS the instant the
   Z80 fetches an opcode at handover_addr, then write the go-byte (BUSREQ)
   to release the trampoline spin-loop.  Waits up to wait_ms for the ISR to
   fire, then performs the full ZX_RomcsRelease() tristate cleanup so the
   cart edge looks completely absent. */
int ZX_SnapshotCommitDual (uint16_t handover_addr_a,
                           uint16_t handover_addr_b,
                           uint16_t go_addr,
                           uint8_t go_value,
                           uint32_t wait_ms) {
    uint32_t waited = 0u;
    uint32_t waited_us = 0u;
    uint32_t spin = 0u;
    int go_in_cart = 0;
    int direct_release_mode = 0;
    int low_rom_mode = 0;
    int watch_armed = 0;
    uint8_t go_readback = 0xFFu;
    uint8_t alive = 0u;
    uint8_t phase = 0u;

    s_handover_fired = 0;
    s_handover_fired_addr = 0xFFFFu;
    s_handover_addr  = handover_addr_a;
    s_handover_addr_b = handover_addr_b;

    /* Fallback path for RAM-resident snapshot entry points (>=0x4000) without
       secondary low-ROM watch address: launch via trigger + alive poll, then
       release ROMCS directly. This avoids RunCartWithM1Watch vector swapping. */
    direct_release_mode = ((handover_addr_b == 0xFFFFu) &&
                           (handover_addr_a >= 0x4000u));
     low_rom_mode = (handover_addr_a < 0x4000u) ? 1 : 0;
    s_handover_armed = 0;
    /* Phase 1 always uses steady ISR so zxprog can continue polling normally. */
    SetVTFIRQ ((u32)RunCartWithRAM, EXTI15_10_IRQn, 0, ENABLE);

    go_in_cart = ((go_addr >= state_pointer->RamBase) &&
                  (go_addr <= state_pointer->RomLast));

     /* Do not arm the trace buffer here: launch-window tracing in the ISR adds
         enough latency to break tight RD service on some games.  Keep the M1
         watch path as close to RunCartWithRAM as possible. */

    /* Release trampoline spin via the appropriate backing store. */
    if (go_in_cart) {
        if (!ZX_CartRamWriteBlock (go_addr, &go_value, 1u)) {
            s_handover_armed = 0;
            s_handover_addr = 0xFFFFu;
            s_handover_addr_b = 0xFFFFu;
            SetVTFIRQ ((u32)RunCartWithRAM, EXTI15_10_IRQn, 0, ENABLE);
            return 0;
        }
        (void)ZX_CartRamReadBlock (go_addr, &go_readback, 1u);
        printf ("snap: trigger %04X <= %02X (rb=%02X)\r\n",
                (unsigned)go_addr, (unsigned)go_value, (unsigned)go_readback);
    } else {
        if (!ZX_BusAcquire()) {
            s_handover_armed = 0;
            s_handover_addr = 0xFFFFu;
            s_handover_addr_b = 0xFFFFu;
            SetVTFIRQ ((u32)RunCartWithRAM, EXTI15_10_IRQn, 0, ENABLE);
            return 0;
        }
        (void)ZX_BusWriteCycle (go_addr, go_value);
        (void)ZX_BusReadCycle (go_addr, &go_readback);
        ZX_BusRelease();
        printf ("snap: trigger %04X <= %02X (rb=%02X)\r\n",
                (unsigned)go_addr, (unsigned)go_value, (unsigned)go_readback);
    }

        if (direct_release_mode || low_rom_mode) {
        /* Direct-release path:
           - RAM entry (>=0x4000): no ROM fetch handover needed.
                     - Low-ROM entry (<0x4000): require launcher entry marker and
                         tail marker, then release promptly. */
        for (spin = 0u; spin < 80000u; ++spin) {
            if (!ZX_CartRamReadBlock (ZX_LAUNCHER_ALIVE_ADDR, &alive, 1u)) {
                continue;
            }
            if (alive == 0xAAu) {
                break;
            }
        }
        while ((alive != 0xAAu) && (waited < wait_ms)) {
            Delay_Ms (1u);
            ++waited;
            if (!ZX_CartRamReadBlock (ZX_LAUNCHER_ALIVE_ADDR, &alive, 1u)) {
                continue;
            }
        }
        if (alive != 0xAAu) {
            printf ("snap: launcher alive did not appear within %lums\r\n",
                    (unsigned long)wait_ms);
            s_handover_armed = 0;
            s_handover_addr = 0xFFFFu;
            s_handover_addr_b = 0xFFFFu;
            SetVTFIRQ ((u32)RunCartWithRAM, EXTI15_10_IRQn, 0, ENABLE);
            return 0;
        }
        if (low_rom_mode) {
            waited_us = 0u;
            for (spin = 0u; spin < 80000u; ++spin) {
                if (!ZX_CartRamReadBlock (ZX_LAUNCHER_PHASE_ADDR, &phase, 1u)) {
                    continue;
                }
                if (phase == ZX_LAUNCHER_PHASE_READY) {
                    break;
                }
            }
            while ((phase != ZX_LAUNCHER_PHASE_READY) &&
                   (waited_us < (wait_ms * 1000u))) {
                Delay_Us (50u);
                waited_us += 50u;
                if (!ZX_CartRamReadBlock (ZX_LAUNCHER_PHASE_ADDR, &phase, 1u)) {
                    continue;
                }
            }
            if (phase != ZX_LAUNCHER_PHASE_READY) {
                printf ("snap: low-ROM tail marker missing (alive=%02X phase=%02X)\r\n",
                        (unsigned)alive, (unsigned)phase);
                s_handover_armed = 0;
                s_handover_addr = 0xFFFFu;
                s_handover_addr_b = 0xFFFFu;
                SetVTFIRQ ((u32)RunCartWithRAM, EXTI15_10_IRQn, 0, ENABLE);
                return 0;
            }
            /* Tail marker is written immediately before JP user_pc. */
            Delay_Us (80u);
        }
        s_handover_fired = 1;
        s_handover_fired_addr = handover_addr_a;
    } else {
        /* High-ROM precise handover path: arm M1 watch after launcher stage marker. */
        for (spin = 0u; spin < 80000u; ++spin) {
            if (!ZX_CartRamReadBlock (ZX_LAUNCHER_ALIVE_ADDR, &alive, 1u)) {
                continue;
            }
            if ((alive == 0x5Au) || (alive == 0xAAu)) {
                __asm volatile ("" ::: "memory");
                s_handover_armed = 1;
                SetVTFIRQ ((u32)RunCartWithM1Watch, EXTI15_10_IRQn, 0, ENABLE);
                watch_armed = 1;
                break;
            }
        }

        waited = 0u;
        while (!watch_armed && (waited < wait_ms)) {
            Delay_Ms (1u);
            ++waited;
            if (!ZX_CartRamReadBlock (ZX_LAUNCHER_ALIVE_ADDR, &alive, 1u)) {
                continue;
            }
            if ((alive == 0x5Au) || (alive == 0xAAu)) {
                __asm volatile ("" ::: "memory");
                s_handover_armed = 1;
                SetVTFIRQ ((u32)RunCartWithM1Watch, EXTI15_10_IRQn, 0, ENABLE);
                watch_armed = 1;
                break;
            }
        }

        if (!watch_armed) {
            printf ("snap: launcher stage marker missing (alive=%02X)\r\n",
                    (unsigned)alive);
            s_handover_armed = 0;
            s_handover_addr = 0xFFFFu;
            s_handover_addr_b = 0xFFFFu;
            SetVTFIRQ ((u32)RunCartWithRAM, EXTI15_10_IRQn, 0, ENABLE);
            return 0;
        }

        waited = 0u;
        while ((waited < wait_ms) && !s_handover_fired) {
            Delay_Ms (1u);
            ++waited;
        }

        if (!s_handover_fired) {
            printf ("snap: M1 handover did not fire within %lums\r\n",
                    (unsigned long)wait_ms);
            s_handover_armed = 0;
            s_handover_addr = 0xFFFFu;
            s_handover_addr_b = 0xFFFFu;
            SetVTFIRQ ((u32)RunCartWithRAM, EXTI15_10_IRQn, 0, ENABLE);
            return 0;
        }
    }

     /* Full tristate of all cart-edge signals (same as `romcs off`).
         Release immediately once handover is considered fired; waiting here
         lets Z80 execute too far while cart is still mapped. */
    ZX_RomcsRelease();

    s_handover_addr = 0xFFFFu;
    s_handover_addr_b = 0xFFFFu;
    return s_handover_fired;
}

uint16_t ZX_SnapshotHandoverFiredAddr (void) {
    return s_handover_fired_addr;
}

int ZX_SnapshotCommit (uint16_t handover_addr, uint16_t go_addr,
                       uint8_t go_value, uint32_t wait_ms) {
    return ZX_SnapshotCommitDual (handover_addr, 0xFFFFu,
                                  go_addr, go_value, wait_ms);
}
