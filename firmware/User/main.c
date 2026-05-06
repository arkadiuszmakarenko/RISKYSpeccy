#include "debug.h"
#include "gpio.h"
#include "tape_player.h"
#include "usb_disk.h"
#include "zx_bus.h"
#include "zx_monitor.h"
#include "zx_image.h"
#include "zx_terminal.h"
#include "reset_button.h"
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
    ZX_Z80Reset();

    /* Mount USB MSC filesystem (lazy: actual init runs on first f_open). */
    {
        FRESULT fr = f_mount (&s_fatfs, "", 0);
        if (fr != FR_OK) {
            printf ("WARN: f_mount failed (fr=%d) — USB drive features disabled\r\n",
                    (int)fr);
        }
    }
    printf ("Hello from RISKY ZX Spectrum firmware!\n");

    ZX_Monitor_Init();
    ZX_Monitor_AutoStartZ80Select();


    while (1) {
        Handle_ResetButtonPA7();
        ZX_Monitor_Poll();
    }
}
