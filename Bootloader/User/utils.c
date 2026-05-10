#include "utils.h"

void InitLED(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_8;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    GPIO_WriteBit(GPIOA, GPIO_Pin_8, Bit_SET);
}

void blinkLed(int number,int delayms)
{
    int i;

    for (i = 0; i < number; i++) {
        GPIO_WriteBit(GPIOA, GPIO_Pin_8, Bit_RESET);
        Delay_Ms(delayms);
        GPIO_WriteBit(GPIOA, GPIO_Pin_8, Bit_SET);
        Delay_Ms(delayms);
    }
}



void DeInitStuff(void)
{
    GPIO_DeInit(GPIOA);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, DISABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC, DISABLE);

}


