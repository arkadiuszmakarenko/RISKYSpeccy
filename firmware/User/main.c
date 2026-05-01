#include "debug.h"
#include "gpio.h"

#include "set_memory_split.h"
#include "usb_disk.h"
#include "zx_bus.h"
#include "zx_monitor.h"
#include "zx_image.h"
#include "ff.h"

static FATFS s_fatfs;

static void Handle_ResetButtonPA7 (void) {
    static uint8_t initialized = 0u;
    static uint8_t idle_level = 1u;
    static uint8_t press_armed = 1u;
    uint8_t level = ((GPIOA->INDR & GPIO_Pin_7) != 0u) ? 1u : 0u;

    if (initialized == 0u) {
        /* Capture the physical idle level once so wiring can be active-high or active-low. */
        Delay_Ms (20u);
        idle_level = ((GPIOA->INDR & GPIO_Pin_7) != 0u) ? 1u : 0u;
        initialized = 1u;
        press_armed = 1u;
        return;
    }

    if (level != idle_level) {
        if (press_armed != 0u) {
            /* Debounce edge away from idle before acting. */
            Delay_Ms (20u);
            level = ((GPIOA->INDR & GPIO_Pin_7) != 0u) ? 1u : 0u;
            if (level != idle_level) {
                press_armed = 0u;
                printf("PA7 reset button pressed: resetting ZX + cart\r\n");
                ZX_RomcsAssert();
                ZX_Z80Reset();
                NVIC_SystemReset();
            }
        }
    } else {
        press_armed = 1u;
    }
}

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
        Handle_ResetButtonPA7();
        ZX_Monitor_Poll();
    }
}
