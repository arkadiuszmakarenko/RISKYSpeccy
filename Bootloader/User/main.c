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



    // Check if GPIO pin is pulled low for full test suite
    // Initialize GPIO for test mode detection
    printf ("Initializing GPIO for test mode detection...\r\n");
    GPIO_InitTypeDef GPIO_InitStructure;
    RCC_APB2PeriphClockCmd (RCC_APB2Periph_GPIOA, ENABLE);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_7;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init (GPIOA, &GPIO_InitStructure);




    if (GPIO_ReadInputDataBit (GPIOA, GPIO_Pin_7) != 0) {


    // Initialize IAP subsystem
    printf ("Initializing IAP subsystem...\r\n");
    IAP_Initialization();

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_8;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;
    GPIO_Init (GPIOA, &GPIO_InitStructure);


        printf ("Waiting for USB device...\r\n");
        blinkLed (10, 500);
        while (1) {
            IAP_Main_Deal();
        }
    } else {
        printf ("Jumping to application\r\n");
        IAP_Jump_APP();
    }
}
