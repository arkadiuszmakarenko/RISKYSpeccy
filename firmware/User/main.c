#include "debug.h"
#include "gpio.h"
#include "tape_player.h"
#include "usb_disk.h"
#include "z80_loader.h"
#include "zx_bus.h"
#include "zx_image.h"
#include "zx_terminal.h"
#include "reset_button.h"
#include "ff.h"

#include <stdio.h>

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

    setvbuf (stdout, NULL, _IONBF, 0);
    ZX_TerminalInit();

    /* Wait until ZX bus is accessible, then launch the file browser loop. */
    {
        uint8_t probe = 0u;
        while (!ZX_BusReadBlock (0x0000u, &probe, 1u)) {
            Handle_ResetButtonPA7();
        }
    }

    for (;;) {
        const char *pending;
        ZX_TerminalCommandZ80Select (NULL);
        pending = ZX_TerminalPendingZ80Selection();
        if ((pending != NULL) && (pending[0] != '\0')) {
            printf ("z80select: loading %s\r\n", pending);
            (void)Z80_LoadAndRun (pending);
            ZX_TerminalClearPendingZ80Selection();
        }
        Handle_ResetButtonPA7();
    }
}
