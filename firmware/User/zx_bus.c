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

/* NMI mailbox protocol (shared with zxprog) */
#define ZX_NMI_WCMD_SEQ_ADDR    0x302Eu
#define ZX_NMI_WCMD_DONE_ADDR   0x302Fu
#define ZX_NMI_WCMD_DST_LO_ADDR 0x3030u
#define ZX_NMI_WCMD_DST_HI_ADDR 0x3031u
#define ZX_NMI_WCMD_LEN_LO_ADDR 0x3032u
#define ZX_NMI_WCMD_LEN_HI_ADDR 0x3033u
#define ZX_NMI_WCMD_DATA_ADDR   0x3034u
#define ZX_NMI_WCMD_CHUNK       0x0200u

#define ZX_NMI_RCMD_SEQ_ADDR    0x3F20u
#define ZX_NMI_RCMD_DONE_ADDR   0x3F21u
#define ZX_NMI_RCMD_SRC_LO_ADDR 0x3F22u
#define ZX_NMI_RCMD_SRC_HI_ADDR 0x3F23u
#define ZX_NMI_RCMD_LEN_ADDR    0x3F24u
#define ZX_NMI_RCMD_BUF_ADDR    0x3F40u
#define ZX_NMI_RCMD_CHUNK       64u

#define ZX_NMI_TIMEOUT_MS       200u

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
static uint8_t s_key_last_seq = 0u;
static uint8_t s_nmi_wcmd_seq = 0u;
static uint8_t s_nmi_rcmd_seq = 0u;

static void ZX_DataBusInput (void) {
    GPIOD->CFGLR = 0x44444444;
}

static void ZX_AddrBusInput (void) {
    GPIOE->CFGLR = 0x44444444;
    GPIOE->CFGHR = 0x44444444;
}

static void ZX_CtrlLinesInput (void) {
    GPIO_InitTypeDef gpio = {0};
    gpio.GPIO_Pin = ZX_CTRL_MASK;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init (GPIOB, &gpio);
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


static int ZX_NmiWriteBlockInternal (uint16_t address, const uint8_t *buffer, uint16_t length, uint32_t timeout_ms) {
    uint16_t offset = 0u;

    if ((buffer == NULL) && (length != 0u)) {
        return 0;
    }
    if (state_pointer == NULL) {
        return 0;
    }

    while (offset < length) {
        uint16_t chunk = (uint16_t)(length - offset);
        uint16_t dst = (uint16_t)(address + offset);
        uint8_t seq;
        uint8_t done;
        uint32_t waited;
        uint16_t i;

        if (chunk > ZX_NMI_WCMD_CHUNK) {
            chunk = ZX_NMI_WCMD_CHUNK;
        }

        ++s_nmi_wcmd_seq;
        seq = s_nmi_wcmd_seq;

        for (i = 0u; i < chunk; ++i) {
            state_pointer->ram[(ZX_NMI_WCMD_DATA_ADDR - 0x3000u) + i] = buffer[offset + i];
        }

        state_pointer->ram[ZX_NMI_WCMD_DST_LO_ADDR - 0x3000u] = (uint8_t)(dst & 0x00FFu);
        state_pointer->ram[ZX_NMI_WCMD_DST_HI_ADDR - 0x3000u] = (uint8_t)(dst >> 8);
        state_pointer->ram[ZX_NMI_WCMD_LEN_LO_ADDR - 0x3000u] = (uint8_t)(chunk & 0x00FFu);
        state_pointer->ram[ZX_NMI_WCMD_LEN_HI_ADDR - 0x3000u] = (uint8_t)(chunk >> 8);
        __asm volatile ("" ::: "memory");
        state_pointer->ram[ZX_NMI_WCMD_SEQ_ADDR - 0x3000u] = seq;

        ZX_TriggerNMI();

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
            return 0;
        }

        offset = (uint16_t)(offset + chunk);
    }

    return 1;
}

static int ZX_NmiReadBlockInternal (uint16_t address, uint8_t *buffer, uint16_t length, uint32_t timeout_ms) {
    uint16_t offset = 0u;

    if ((buffer == NULL) && (length != 0u)) {
        return 0;
    }
    if (state_pointer == NULL) {
        return 0;
    }

    while (offset < length) {
        uint16_t chunk = (uint16_t)(length - offset);
        uint16_t src = (uint16_t)(address + offset);
        uint8_t seq;
        uint8_t done;
        uint32_t waited;
        uint16_t i;

        if (chunk > ZX_NMI_RCMD_CHUNK) {
            chunk = ZX_NMI_RCMD_CHUNK;
        }

        ++s_nmi_rcmd_seq;
        seq = s_nmi_rcmd_seq;

        state_pointer->ram[ZX_NMI_RCMD_SRC_LO_ADDR - 0x3000u] = (uint8_t)(src & 0x00FFu);
        state_pointer->ram[ZX_NMI_RCMD_SRC_HI_ADDR - 0x3000u] = (uint8_t)(src >> 8);
        state_pointer->ram[ZX_NMI_RCMD_LEN_ADDR - 0x3000u] = (uint8_t)chunk;
        __asm volatile ("" ::: "memory");
        state_pointer->ram[ZX_NMI_RCMD_SEQ_ADDR - 0x3000u] = seq;

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

        for (i = 0u; i < chunk; ++i) {
            buffer[offset + i] = state_pointer->ram[(ZX_NMI_RCMD_BUF_ADDR - 0x3000u) + i];
        }

        offset = (uint16_t)(offset + chunk);
    }

    return 1;
}

int ZX_BusReadBlock (uint16_t address, uint8_t *buffer, uint16_t length) {
    return ZX_NmiReadBlockInternal (address, buffer, length, ZX_NMI_TIMEOUT_MS);
}

int ZX_BusWriteBlock (uint16_t address, const uint8_t *buffer, uint16_t length) {
    return ZX_NmiWriteBlockInternal (address, buffer, length, ZX_NMI_TIMEOUT_MS);
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

     /* ROMCS as push-pull output. Keep HIGH so cart ROM remains selected. */
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

__attribute__((section(".text.fastirq"), aligned(64), noinline))
void RunCartWithRAM (void) {
    struct ZXCartState *sp = state_pointer;
    uint16_t address = (uint16_t)GPIOE->INDR;

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






/* ===== ROMCS/launch compatibility stubs ============================= */

#define ZX_PIN_ROMCS GPIO_Pin_3

void ZX_RomcsAssert (void) {
    /* Keep cart ROM selected and cart ISR active. */
    GPIO_InitTypeDef gpio = {0};
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