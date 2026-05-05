#include "debug.h"
#include "gpio.h"
#include "tape_player.h"

#include "set_memory_split.h"
#include "usb_disk.h"
#include "zx_bus.h"
#include "zx_monitor.h"
#include "zx_image.h"
#include "zx_terminal.h"
#include "ff.h"

static FATFS s_fatfs;

static void Handle_ResetButtonPA7 (void) {
    static uint8_t initialized = 0u;
    static uint8_t idle_level = 1u;
    static uint8_t press_armed = 1u;
    const char *tap_path;
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
            uint16_t held_ms = 0u;

            /* Debounce edge away from idle before acting. */
            Delay_Ms (20u);
            level = ((GPIOA->INDR & GPIO_Pin_7) != 0u) ? 1u : 0u;
            if (level != idle_level) {
                press_armed = 0u;

                while (level != idle_level) {
                    Delay_Ms (10u);
                    held_ms = (uint16_t)(held_ms + 10u);
                    level = ((GPIOA->INDR & GPIO_Pin_7) != 0u) ? 1u : 0u;
                    if (held_ms >= 900u) {
                        printf ("Play/Reset button long press: resetting ZX + cart\r\n");
                        TAP_Player_Stop();
                        ZX_RomcsAssert();
                        NVIC_SystemReset();
                    }
                }

                tap_path = ZX_TerminalPendingTapSelection();
                if ((tap_path == NULL) || (tap_path[0] == '\0')) {
                    printf ("Play/Reset button short press: no queued .tap, use tapselect first\r\n");
                    return;
                }
                if (!TAP_Player_HasTapeLoaded() && !TAP_Player_Load (tap_path)) {
                    printf ("Play/Reset button short press: failed to prepare %s\r\n", tap_path);
                    return;
                }
                if (TAP_Player_IsRunning()) {
                    printf ("Play/Reset button short press: tape already playing\r\n");
                    return;
                }

                printf ("Play/Reset button short press: starting tape %s\r\n", tap_path);
                TAP_Player_Start();
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
