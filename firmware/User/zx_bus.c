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

/* Cart-side control flags mailbox (0x3F00 in cart RAM, offset 0x0F00 from RamBase 0x3000).
   Set CTRL_DRAW_SUSPEND to prevent ZX main loop from calling draw_bridge_screen()
   (which does memset(0x4000,0,6144)), allowing CH32 to write/verify screen RAM. */
#define ZX_CTRL_FLAGS_OFFSET  0x0F00u  /* 0x3F00 - 0x3000 */
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

    ZX_CtrlLinesInput();
    ZX_DataBusInput();
    ZX_AddrBusInput();

    EXTI->INTFR = state_pointer->IRQLine;
    SetVTFIRQ ((u32)RunCartWithRAM, EXTI15_10_IRQn, 0, ENABLE);
    NVIC_EnableIRQ (EXTI15_10_IRQn);
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

#pragma GCC pop_options
