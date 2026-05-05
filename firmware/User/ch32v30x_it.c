/********************************** (C) COPYRIGHT *******************************
 * File Name          : ch32v30x_it.c
 * Author             : WCH
 * Version            : V1.0.0
 * Date               : 2024/03/06
 * Description        : Main Interrupt Service Routines.
 *********************************************************************************
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * Attention: This software (modified or not) and binary are used for
 * microcontroller manufactured by Nanjing Qinheng Microelectronics.
 *******************************************************************************/
#include "ch32v30x_it.h"
#include "zx_bus.h"
#include "tape_player.h"


void NMI_Handler (void) __attribute__ ((interrupt ("WCH-Interrupt-fast")));
void HardFault_Handler (void) __attribute__ ((interrupt ("WCH-Interrupt-fast")));
/* Fallback vector-table stubs for tape-player ISRs.
   In normal operation the VTF mechanism dispatches these directly to
   TAP_IorqISR / TAP_TimerISR; these stubs are never reached. */
void EXTI9_5_IRQHandler (void) __attribute__ ((interrupt ("WCH-Interrupt-fast")));
void TIM2_IRQHandler    (void) __attribute__ ((interrupt ("WCH-Interrupt-fast")));

/*********************************************************************
 * @fn      NMI_Handler
 *
 * @brief   This function handles NMI exception.
 *
 * @return  none
 */
void NMI_Handler (void) {
    while (1) {
    }
}

/*********************************************************************
 * @fn      HardFault_Handler
 *
 * @brief   This function handles Hard Fault exception.
 *
 * @return  none
 */
void HardFault_Handler (void) {
    while (1) {
    }
}

void EXTI15_10_IRQHandler (void) {
    RunCartWithRAM();
}

void EXTI9_5_IRQHandler (void) {
    /* VTF bypasses this; present only for the vector table.           */
    TAP_IorqISR();
}

void TIM2_IRQHandler (void) {
    /* VTF bypasses this; present only for the vector table.           */
    TAP_TimerISR();
}


