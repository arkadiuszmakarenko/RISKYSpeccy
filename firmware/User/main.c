#include "debug.h"
#include "gpio.h"

#include "set_memory_split.h"
#include "usb_disk.h"
#include "zx_bus.h"
#include "zx_monitor.h"
#include "zx_image.h"
#include "ff.h"

static FATFS s_fatfs;

int main (void) {
    NVIC_PriorityGroupConfig (NVIC_PriorityGroup_2);
    SystemCoreClockUpdate();
    Delay_Init();
    USART_Printf_Init (115200);
    GPIO_Config();
    USB_Initialization();
    Init_Cart();

    /* Mount USB MSC filesystem (lazy: actual init runs on first f_open). */
    {
        FRESULT fr = f_mount (&s_fatfs, "", 0);
        if (fr != FR_OK) {
            printf("WARN: f_mount failed (fr=%d) — USB drive features disabled\r\n",
                   (int)fr);
        }
    }

    ZX_Monitor_Init();

    printf("Hello from RISKY ZX Spectrum firmware!\n");

    while (1) {
        ZX_Monitor_Poll();
    }
}
