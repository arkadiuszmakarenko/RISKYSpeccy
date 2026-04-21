#include "cart.h"
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

//
//  Config Cart emulation hardware.
void Init_Cart() {


    FLASH_Enhance_Mode (ENABLE);

    EXTI->INTFR = EXTI_Line10;
    SetVTFIRQ ((u32)RunCart16k, EXTI15_10_IRQn, 0, ENABLE);
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

#pragma GCC pop_options
