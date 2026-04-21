#include "debug.h"
#include "gpio.h"

#include "set_memory_split.h"
#include "usb_disk.h"
#include "cart.h"
#include "zx_image.h"

#define NMI_DEBOUNCE_MS 20u
#define NMI_COOLDOWN_MS 200u

static void NMI_TriggerPulse(void) {
    /* ZX NMI is edge-sensitive; generate a short active-low pulse. */
    GPIOC->BSHR = ((uint32_t)GPIO_Pin_9 << 16);
    Delay_Us (16);
    GPIOC->BSHR = GPIO_Pin_9;
}

static void NMI_ButtonTask_1ms(void) {
    static uint8_t debounced_pressed = 0;
    static uint8_t candidate_pressed = 0;
    static uint16_t debounce_count = 0;
    static uint32_t tick_ms = 0;
    static uint32_t next_allowed_ms = 0;

    const uint8_t raw_pressed = ((GPIOA->INDR & (GPIO_Pin_6 | GPIO_Pin_7)) != (GPIO_Pin_6 | GPIO_Pin_7)) ? 1u : 0u;

    if (raw_pressed == candidate_pressed) {
        if (debounce_count < NMI_DEBOUNCE_MS) {
            ++debounce_count;
        }
    } else {
        candidate_pressed = raw_pressed;
        debounce_count = 0;
    }

    if ((debounce_count >= NMI_DEBOUNCE_MS) && (debounced_pressed != candidate_pressed)) {
        debounced_pressed = candidate_pressed;

        if ((debounced_pressed != 0u) && (tick_ms >= next_allowed_ms)) {
            NMI_TriggerPulse();
            next_allowed_ms = tick_ms + NMI_COOLDOWN_MS;
        }
    }

    ++tick_ms;
}

int main (void) {
    NVIC_PriorityGroupConfig (NVIC_PriorityGroup_2);
    SystemCoreClockUpdate();
    Delay_Init();
    USART_Printf_Init (115200);
    GPIO_Config();
    USB_Initialization();
    Init_Cart();


    while (1) {
        NMI_ButtonTask_1ms();
        Delay_Ms (1);
    }
}
