#include "debug.h"
#include "gpio.h"

#include "set_memory_split.h"
#include "usb_disk.h"
#include "cart.h"
#include "zx_image.h"

int main (void) {
    NVIC_PriorityGroupConfig (NVIC_PriorityGroup_2);
    SystemCoreClockUpdate();
    Delay_Init();
    USART_Printf_Init (115200);
    GPIO_Config();
    USB_Initialization();
    Init_Cart();


    while (1) {
    }
}
