#include "gpio.h"

/* LED on PA8: open-drain, active-low.
   Bit_SET (release) = LED OFF; Bit_RESET (drive low) = LED ON.
   Direct register control in usb_disk.c:
     GPIOA->BCR  = GPIO_Pin_8;  -> LED on
     GPIOA->BSHR = GPIO_Pin_8;  -> LED off */
static void led_init (void) {
    GPIO_InitTypeDef GPIO_InitStructure;
    RCC_APB2PeriphClockCmd (RCC_APB2Periph_GPIOA, ENABLE);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_8;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init (GPIOA, &GPIO_InitStructure);
    GPIO_WriteBit (GPIOA, GPIO_Pin_8, Bit_SET);   /* LED off at boot */
}

void GPIO_Config() {
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOD | RCC_APB2Periph_GPIOE | RCC_APB2Periph_AFIO;
    RCC->APB1PCENR |= RCC_APB1Periph_DAC;
    AFIO->PCFR1 = (AFIO->PCFR1 & ~AFIO_PCFR1_SWJ_CFG) | AFIO_PCFR1_SWJ_CFG_JTAGDISABLE;
    /*Configure GPIO pin Output Level */
    GPIOA->BSHR |= GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_2 | GPIO_Pin_3 | GPIO_Pin_9;
    GPIOB->BSHR |= GPIO_Pin_3;
    GPIOC->BSHR |= GPIO_Pin_9;

    // GPIOA->CFGLR = 0x44403333;
    GPIOA->CFGLR = 0;
    GPIOA->CFGLR |= GPIO_CFGLR_MODE0;
    GPIOA->CFGLR |= GPIO_CFGLR_MODE1;
    GPIOA->CFGLR |= GPIO_CFGLR_MODE2;
    GPIOA->CFGLR |= GPIO_CFGLR_MODE3;
    // set up DAC GPIO 4 - leave 0;
    GPIOA->CFGLR |= GPIO_CFGLR_CNF5_0;
    GPIOA->CFGLR |= GPIO_CFGLR_CNF6_0;
    GPIOA->CFGLR |= GPIO_CFGLR_CNF7_0;

    GPIO_InitTypeDef GPIO_InitStructure;
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_3;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init (GPIOB, &GPIO_InitStructure);

    /* PA8 LED: open-drain, active-low. */
    led_init();

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_7;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init (GPIOB, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_4;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init (GPIOB, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_5;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init (GPIOB, &GPIO_InitStructure);


    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_10;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init (GPIOB, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_6 | GPIO_Pin_7;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init (GPIOA, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_9;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init (GPIOC, &GPIO_InitStructure);


    EXTI_InitTypeDef EXTI_InitStructure = {0};
    /* GPIOB ----> EXTI_Line10 - */
    GPIO_EXTILineConfig (GPIO_PortSourceGPIOB, GPIO_PinSource10);
    EXTI_InitStructure.EXTI_Line = EXTI_Line10;
    EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Interrupt;
    EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Falling;
    EXTI_InitStructure.EXTI_LineCmd = ENABLE;
    EXTI_Init (&EXTI_InitStructure);
}
