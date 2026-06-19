#include "debug.h"
#include "gpio.h"
#include "tape_player.h"
#include "usb_disk.h"
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

    GPIO_InitTypeDef gpio = {0};
    /* Re-drive ROMCS as push-pull HIGH (cart ROM selected). */
    gpio.GPIO_Pin = GPIO_Pin_14;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init (GPIOB, &gpio);
    GPIO_SetBits (GPIOB, GPIO_Pin_14);
        /* Re-drive ROMCS as push-pull HIGH (cart ROM selected). */
    gpio.GPIO_Pin = GPIO_Pin_15;
    gpio.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init (GPIOB, &gpio);
    GPIO_SetBits (GPIOB, GPIO_Pin_15);

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

     /* Show "Insert USB Drive" prompt on the ZX display and wait until the user
         inserts a drive and presses Enter on the ZX keyboard. */
     ZX_TerminalWaitUsbDriveReady();

    for (;;) {
        /* USB-state watchdog.  The file browser and TAP player are long-
         * running blocking functions that previously never re-tested the
         * USB host stack, so an unplug while they were running left the
         * launcher in a wedged state.  Each iteration of the main loop
         * asks the terminal layer whether the drive is still attached and
         * jumps back to the insert-USB prompt if it isn't. */
        ZX_TerminalPollUsb ();
        if (ZX_TerminalUsbLost ()) {
            printf ("main: USB drive lost, re-prompting\r\n");
            ZX_TerminalClearPendingZ80Selection ();
            ZX_TerminalClearPendingTapSelection ();
            ZX_TerminalClearUsbLost ();
            goto recheck_usb;
        }

        /* Open file browser (blocks until the user selects a file or cancels). */
        if (ZX_TerminalCommandZ80Select (NULL)) {
            /* Z80 game launched inside the terminal; spin here forever.
               Only a long-press hardware reset makes sense at this point.
               Also poll for an external ZX hardware reset: if the user
               presses the Spectrum's reset button (wired to PC6), zxprog
               re-runs in isolation with the cart ISR re-armed by the
               Spectrum's ROM-wins signalling — but the terminal/browser/
               tap state in CH32 RAM (s_prev_valid, s_selector_font_mode,
               pending tap selections, terminal char/attr model, etc.) is
               now stale and trying to surgically re-engage the cart from
               here leaves the launcher either invisible or showing a
               stray yellow/green border flash.  Issue a full NVIC reset
               so main() runs from the top: every peripheral is
               reinitialised, the cart state machine starts clean, FATFS
               is re-mounted, the file browser is reopened. */
            for (;;) {
                if (ZX_HandleExternalResetIfAny ()) {
                    printf ("ZX hardware reset detected — full CH32 reset\r\n");
                    NVIC_SystemReset();
                }
                Handle_ResetButtonPA7();
            }
        }

        /* .tap/.tzx selected: z80select already switched the ZX to BASIC and
           queued the path.  Poll the button until the user short-presses to
           start playback.  Do NOT clear pending here — the button handler
           needs it to know which file to load. */
        if ((ZX_TerminalPendingTapSelection() != NULL) &&
            (ZX_TerminalPendingTapSelection()[0] != '\0')) {

            /* Wait for a short press that actually starts the tape.
               Also bail out on a ZX hardware reset — same reasoning as
               the z80-spin path above: stale browser/tap/terminal state
               in CH32 RAM makes a partial recovery unreliable, so we do
               a full NVIC reset to restart cleanly from main(). */
            while (!TAP_Player_IsRunning()) {
                if (ZX_HandleExternalResetIfAny ()) {
                    printf ("ZX hardware reset detected — full CH32 reset\r\n");
                    NVIC_SystemReset();
                }
                Handle_ResetButtonPA7();
            }

            /* Tape is running: keep polling (allows long-press reset and
               external ZX reset).  Same full-reset recovery model. */
            while (TAP_Player_IsRunning()) {
                if (ZX_HandleExternalResetIfAny ()) {
                    printf ("ZX hardware reset detected — full CH32 reset\r\n");
                    NVIC_SystemReset();
                }
                Handle_ResetButtonPA7();
            }

            /* Tape finished: the Z80 is now running the loaded program.
               Spin here like the z80 snapshot path — ZX hardware reset
               triggers a full CH32 reset to bring the launcher back. */
            ZX_TerminalClearPendingTapSelection();
            for (;;) {
                if (ZX_HandleExternalResetIfAny ()) {
                    printf ("ZX hardware reset detected — full CH32 reset\r\n");
                    NVIC_SystemReset();
                }
                Handle_ResetButtonPA7();
            }
        }
        /* Nothing selected (user cancelled/escaped): loop and reopen browser. */
        continue;
recheck_usb:
        /* Drive was unplugged: drop any in-flight selections, reset the
         * host stack so the next attach is observed cleanly, and re-run
         * the insert-USB prompt.  Loop back to the top of main(). */
        ClearUSB();
        USB_Initialization();
        ZX_TerminalWaitUsbDriveReady();
        continue;
    }
}
