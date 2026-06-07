#include "usb_host_iap.h"
#include "usb_host_config.h"
#include "utils.h"

/*********************************************************************
 * @fn      main
 *
 * @brief   Main program.
 *
 * @return  none
 */

int main (void) {

    Delay_Init();
    USART_Printf_Init (115200);



    // Check if PA13/PA14 are jumpered to enter IAP:
    // PA13 driven low (push-pull output), PA14 as input pull-up.
    // If jumpered, PA14 reads low -> enter IAP; otherwise stays high -> jump to app.
    printf ("Initializing GPIO for IAP mode detection...\r\n");
    GPIO_InitTypeDef GPIO_InitStructure;
    RCC_APB2PeriphClockCmd (RCC_APB2Periph_GPIOA | RCC_APB2Periph_AFIO, ENABLE);
    // Disable SWJ to release PA13/PA14 from alternate function for use as GPIO
    GPIO_PinRemapConfig (GPIO_Remap_SWJ_Disable, ENABLE);
    // PA13: push-pull output, drive low
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_13;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_Init (GPIOA, &GPIO_InitStructure);
    GPIO_ResetBits (GPIOA, GPIO_Pin_13);
    // PA14: input with pull-up
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_14;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init (GPIOA, &GPIO_InitStructure);

    if (GPIO_ReadInputDataBit (GPIOA, GPIO_Pin_14) == 0) {


    // Initialize IAP subsystem
    printf ("Initializing IAP subsystem...\r\n");
    IAP_Initialization();

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_8;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;
    GPIO_Init (GPIOA, &GPIO_InitStructure);


        printf ("Waiting for USB device...\r\n");
        blinkLed (3, 500);
        while (1) {
            IAP_Main_Deal();
        }
    } else {
        printf ("Jumping to application\r\n");
        IAP_Jump_APP();
    }
}
