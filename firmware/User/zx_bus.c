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


void RunCartWithRAM (void) {
    struct ZXCartState *sp = state_pointer;
    uint16_t address = (uint16_t)GPIOE->INDR;

    /* When ROMCS has been released the internal Spectrum ROM owns 0x0000-0x3FFF;
       the cart must not drive the data bus. Just clear the EXTI flag and exit. */
    if (s_rom_released) {
        EXTI->INTFR = sp->IRQLine;
        return;
    }

 //    if ((GPIOB->INDR & GPIO_Pin_10) != 0u) {
 //       EXTI->INTFR = EXTI_Line10;
 //       return;
 //   }

     if ((GPIOB->INDR & sp->PinRD) == 0u) { // Check for RD active (active low)
        if (address < sp->RamBase) {
            GPIOD->CFGLR = sp->BusOn;
            GPIOD->OUTDR = (GPIOD->OUTDR & ~sp->DataMask) | g_zx_image[address];
            while ((GPIOB->INDR & sp->PinRD) == 0u) { }
            GPIOD->CFGLR = sp->BusOff;
        } else if (address <= sp->RomLast) {
            GPIOD->CFGLR = sp->BusOn;
            GPIOD->OUTDR = (GPIOD->OUTDR & ~sp->DataMask) | sp->ram[address - sp->RamBase];
            while ((GPIOB->INDR & sp->PinRD) == 0u) { }
            GPIOD->CFGLR = sp->BusOff;
        }
    } else {
        while (((GPIOB->INDR & sp->PinMREQ) == 0u) && ((GPIOB->INDR & sp->PinWR) != 0u)) { }

        if (((GPIOB->INDR & sp->PinMREQ) == 0u) && ((GPIOB->INDR & sp->PinWR) == 0u)) { // Check for delayed WR while MREQ remains active
            if ((address >= sp->RamBase) && (address <= sp->RomLast)) {
                GPIOD->CFGLR = sp->BusOff; // Data bus must be input while capturing host writes
                do {
                    sp->ram[address - sp->RamBase] = (uint8_t)(GPIOD->INDR & sp->DataMask);
                } while ((GPIOB->INDR & sp->PinWR) == 0u);
            }
        }
    }

    EXTI->INTFR = sp->IRQLine; // Clear the interrupt flag for EXTI line 10 to allow the next interrupt to be triggered

    return;
}

/* Variant of RunCartWithRAM with M1-handover detection at the top.  Used
   only during the .z80 snapshot launch window: ZX_SnapshotCommit swaps the
   VTF entry to point here, then back to RunCartWithRAM (or releases ROMCS)
   after the M1 fetch fires.  Kept as a separate function so the normal cart
   ISR has zero added cost. */
void RunCartWithM1Watch (void) {
    struct ZXCartState *sp = state_pointer;
    uint16_t address = (uint16_t)GPIOE->INDR;

    /* Record every MREQ event during the launch window, skipping RFSH cycles
       (M1=1, RD=1, WR=1 = ctrl bits all 1 = 0x07) to halve trace usage:
       each Z80 instruction produces one M1 MREQ and one RFSH MREQ; skipping
       RFSH doubles the number of useful instructions visible in the buffer.
       Data is sampled at MREQ falling edge: reliable for WR (Z80-driven);
       for ROM/RAM reads the MCU hasn't driven GPIOD yet so data = 0xFF. */
    if (!s_trace_full) {
        uint16_t bport_entry = (uint16_t)GPIOB->INDR;
        uint8_t ctrl = (uint8_t)(
            ((bport_entry & ZX_PIN_M1)  ? 0x01u : 0u) |
            ((bport_entry & sp->PinRD)  ? 0x02u : 0u) |
            ((bport_entry & sp->PinWR)  ? 0x04u : 0u));
        /* 0x07 = M1 inactive + RD inactive + WR inactive = RFSH cycle */
        if (ctrl != 0x07u) {
            uint16_t th = s_trace_head;
            s_trace[th].addr = address;
            s_trace[th].data = (uint8_t)(GPIOD->INDR & 0xFFu);
            s_trace[th].ctrl = ctrl;
            s_trace_head = (uint16_t)(th + 1u);
            if (s_trace_head >= ZX_TRACE_DEPTH) { s_trace_full = 1u; }
        }
    }

    /* Match on address alone, not M1.  Reasoning: while the trampoline is
       running, the Z80 only accesses cart RAM (0x3F90..0x3FFF) and a few
       I/O ports; the cart address bus never visits handover_addr.  The
       first time we see handover_addr on the bus is the opcode fetch
       triggered by `JP user_pc` at the end of the trampoline.  Sampling M1
       synchronously was racing the ~285ns M1-low window vs EXTI latency
       and we'd miss the only fetch that ever lands on this address.
       Guard: only match if address is not 0xFFFF (the "unused" sentinel
       value for handover_addr_b would otherwise fire on RFSH cycles when
       the R register wraps to 0x7F and the upper address bits are all 1). */
    if (((address == s_handover_addr) ||
         ((s_handover_addr_b != 0xFFFFu) && (address == s_handover_addr_b)))) {
        GPIO_ResetBits (GPIOB, GPIO_Pin_3);   /* ROMCS LOW immediately */
        s_rom_released = 1;
        s_handover_armed = 0;
        s_handover_fired_addr = address;
        s_handover_fired = 1;
        EXTI->INTFR = sp->IRQLine;
        return;
    }

    if (s_rom_released) {
        EXTI->INTFR = sp->IRQLine;
        return;
    }

    if ((GPIOB->INDR & sp->PinRD) == 0u) {
        if (address < sp->RamBase) {
            GPIOD->CFGLR = sp->BusOn;
            GPIOD->OUTDR = (GPIOD->OUTDR & ~sp->DataMask) | g_zx_image[address];
            while ((GPIOB->INDR & sp->PinRD) == 0u) { }
            GPIOD->CFGLR = sp->BusOff;
        } else if (address <= sp->RomLast) {
            GPIOD->CFGLR = sp->BusOn;
            GPIOD->OUTDR = (GPIOD->OUTDR & ~sp->DataMask) | sp->ram[address - sp->RamBase];
            while ((GPIOB->INDR & sp->PinRD) == 0u) { }
            GPIOD->CFGLR = sp->BusOff;
        }
    } else {
        while (((GPIOB->INDR & sp->PinMREQ) == 0u) && ((GPIOB->INDR & sp->PinWR) != 0u)) { }
        if (((GPIOB->INDR & sp->PinMREQ) == 0u) && ((GPIOB->INDR & sp->PinWR) == 0u)) {
            if ((address >= sp->RamBase) && (address <= sp->RomLast)) {
                GPIOD->CFGLR = sp->BusOff;
                do {
                    sp->ram[address - sp->RamBase] = (uint8_t)(GPIOD->INDR & sp->DataMask);
                } while ((GPIOB->INDR & sp->PinWR) == 0u);
            }
        }
    }

    EXTI->INTFR = sp->IRQLine;
    return;
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

    /* Clean Spectrum register state.  Initialisation matches what the ROM
       produces after a clean RESET; enough for most standalone code blocks.
       R_comp = (0 - 12) & 0x7F = 0x74 (12 M1 cycles from LD R,A to JP). */
    __builtin_memset (regblock, 0, ZX_LAUNCHER_REGBLOCK_LEN);
    regblock[10] = 0x3Au; regblock[11] = 0x5Cu;  /* IY = 0x5C3A */
    regblock[13] = 0x3Fu;                         /* I = 0x3F */
    regblock[14] = 7u;                            /* border = 7 (white) */
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

    /* Neutral regblock: all registers zero except IY=0x5C3A, I=0x3F,
       SP=0xFF40, border=7, R_comp for 12 M1 cycles.  Good enough for the
       test trampolines in launch_test.c which restore their own state. */
    __builtin_memset (regblock, 0, ZX_LAUNCHER_REGBLOCK_LEN);
    regblock[10] = 0x3Au; regblock[11] = 0x5Cu;  /* IY = 0x5C3A */
    regblock[13] = 0x3Fu;                         /* I = 0x3F */
    regblock[14] = 7u;                            /* border */
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
    int go_in_cart = 0;

    s_handover_fired = 0;
    s_handover_fired_addr = 0xFFFFu;
    s_handover_addr  = handover_addr_a;
    s_handover_addr_b = handover_addr_b;
    /* Memory barrier so the ISR sees both writes in order. */
    __asm volatile ("" ::: "memory");
    s_handover_armed = 1;

    /* Swap the cart ISR to the M1-watching variant.  This adds the address
       compare + M1 sample to the hot path only for the brief launch window;
       the normal cart ISR (RunCartWithRAM) has zero added cost. */
    SetVTFIRQ ((u32)RunCartWithRAM, EXTI15_10_IRQn, 0, ENABLE);

    go_in_cart = ((go_addr >= state_pointer->RamBase) &&
                  (go_addr <= state_pointer->RomLast));

    /* Arm bus trace immediately before trigger so the trace buffer captures
       the full launcher sequence: trigger read, alive write, register POPs,
       launcher tail M1 fetches, and the final JP user_pc M1 (handover). */
    ZX_BusTraceArm ();

    /* Release trampoline spin via the appropriate backing store. */
    if (go_in_cart) {
        if (!ZX_CartRamWriteBlock (go_addr, &go_value, 1u)) {
            s_handover_armed = 0;
            s_handover_addr = 0xFFFFu;
            s_handover_addr_b = 0xFFFFu;
            SetVTFIRQ ((u32)RunCartWithRAM, EXTI15_10_IRQn, 0, ENABLE);
            return 0;
        }
    } else {
        if (!ZX_BusAcquire()) {
            s_handover_armed = 0;
            s_handover_addr = 0xFFFFu;
            s_handover_addr_b = 0xFFFFu;
            SetVTFIRQ ((u32)RunCartWithRAM, EXTI15_10_IRQn, 0, ENABLE);
            return 0;
        }
        (void)ZX_BusWriteCycle (go_addr, go_value);
        ZX_BusRelease();
    }

    /* Wait for ISR to fire on the M1 fetch.  At 3.5 MHz the trampoline
       (~80 T-states from go-read to JP target) finishes in ~30us. */
    while ((waited < wait_ms) && !s_handover_fired) {
        Delay_Ms (1u);
        ++waited;
    }

    if (!s_handover_fired) {
        printf ("snap: M1 handover did not fire within %lums\r\n",
                (unsigned long)wait_ms);
        s_handover_armed = 0;
        /* Fall through and force release anyway — Z80 may still be running
           if the trampoline took an unexpected path; tristating everything
           lets the user reset. */
    }

    /* Full tristate of all cart-edge signals (same as `romcs off`). */
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
