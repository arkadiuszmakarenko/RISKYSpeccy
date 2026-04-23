#include "debug.h"
#include "gpio.h"

#include "set_memory_split.h"
#include "usb_disk.h"
#include "zx_bus.h"
#include "zx_image.h"

#define NMI_DEBOUNCE_MS 20u
#define NMI_COOLDOWN_MS 200u




int main (void) {
    NVIC_PriorityGroupConfig (NVIC_PriorityGroup_2);
    SystemCoreClockUpdate();
    Delay_Init();
    USART_Printf_Init (115200);
    GPIO_Config();
    USB_Initialization();
    Init_Cart();

    printf("Hello from RISKY ZX Spectrum firmware!\n");

    while (1) {

    }
}
