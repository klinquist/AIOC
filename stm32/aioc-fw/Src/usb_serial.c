#include "usb_serial.h"
#include "stm32f3xx_hal.h"
#include "aioc.h"
#include "tusb.h"
#include "led.h"
#include "ptt.h"
#include "settings.h"
#include "usb_descriptors.h"

void USB_SERIAL_UART_IRQ(void)
{
    uint32_t ISR = USB_SERIAL_UART->ISR;

    if (ISR & USART_ISR_TC) {
        /* Transmission complete and no new data queued to send.
         * Clear interrupt flag and further processing inside TXE Bit Handler.
         * This is safe, because TXE bit will always be set, when TC bit is also set. */
        USB_SERIAL_UART->ICR = USART_ICR_TCCF;
    }

    if (ISR & USART_ISR_TXE) {
        /* TX register is empty, load up another character if there is one left in the buffer.
         * Otherwise atomically shutdown the transmission. */
        __disable_irq();
        uint32_t available = tud_cdc_n_available(0);
        if (available == 0) {
            /* No char left in fifo. Disable the TX-empty interrupt and wait for transmission to complete */
            USB_SERIAL_UART->CR1 &= (uint32_t) ~USART_CR1_TXEIE;

            if (ISR & USART_ISR_TC) {
                /* If TC is also set, all transmissions have completed, thus disable the transmitter. */
                USB_SERIAL_UART->CR1 &= (uint32_t) ~(USART_CR1_TE | USART_CR1_TCIE);
            }
        }
        __enable_irq();

        if (available > 0) {
             /* Write char from fifo */
            int32_t c = tud_cdc_n_read_char(0);
            TU_ASSERT(c != -1, /**/);
            USB_SERIAL_UART->TDR = (uint8_t) c;
            LED_MODE(1, LED_MODE_FASTPULSE);
        }
    }

    if (ISR & USART_ISR_RXNE) {
        /* RX register is not empty, get character and put into USB send buffer */
        if (tud_cdc_n_write_available(0) > 0) {
            uint8_t c = USB_SERIAL_UART->RDR;
            if (!(PTT_Status() & PTT_MASK_PTT1) ) {
                /* Only store character when no PTT1 is asserted (shares the same pin) */
                tud_cdc_n_write(0, &c, 1);
                LED_MODE(0, LED_MODE_FASTPULSE);
            }
        } else {
            /* No space in fifo currently. Pause this interrupt and re-enable later */
            __disable_irq();
            USB_SERIAL_UART->CR1 &= (uint32_t) ~USART_CR1_RXNEIE;
            __enable_irq();
        }
    }

    if (ISR & USART_ISR_RTOF) {
        USB_SERIAL_UART->ICR = USART_ICR_RTOCF;
        /* Receiver timeout. Flush data via USB. */
        tud_cdc_n_write_flush(0);
    }

    if (ISR & USART_ISR_ORE) {
        /* Overflow error */
        USB_SERIAL_UART->ICR = USART_ICR_ORECF;
        TU_ASSERT(0, /**/);
    }

    if (ISR & USART_ISR_FE) {
        USB_SERIAL_UART->ICR = USART_ISR_FE;
    }

    if (ISR & USART_ISR_NE) {
        USB_SERIAL_UART->ICR = USART_ISR_NE;
    }
}

// Invoked when CDC interface received data from host
void tud_cdc_rx_cb(uint8_t itf)
{
    TU_ASSERT(itf == 0, /**/);

    uint8_t pttStatus = PTT_Status();

    if (pttStatus & PTT_MASK_PTT1) {
        /* Make sure PTT1 is disabled, since it shares signal lines with the serial interface */
        PTT_Control(pttStatus & ~PTT_MASK_PTT1);
    }

    /* This enables the transmitter and the TX-empty interrupt, which handles writing UART data */
    __disable_irq();
    USB_SERIAL_UART->CR1 |= USART_CR1_TE | USART_CR1_TXEIE | USART_CR1_TCIE;
    __enable_irq();

}

// Invoked when space becomes available in TX buffer
void tud_cdc_tx_complete_cb(uint8_t itf)
{
    TU_ASSERT(itf == 0, /**/);

    /* Re-enable UART RX-nonempty interrupt to handle reading UART data */
    __disable_irq();
    USB_SERIAL_UART->CR1 |= USART_CR1_RXNEIE;
    __enable_irq();
}

// Invoked when line coding is change via SET_LINE_CODING
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const* p_line_coding)
{
    TU_ASSERT(itf == 0, /**/);

    /* Disable IRQs and UART */
    __disable_irq();
    USB_SERIAL_UART->CR1 &= (uint32_t) ~USART_CR1_UE;

    /* Calculate new baudrate */
    USB_SERIAL_UART->BRR = (HAL_RCCEx_GetPeriphCLKFreq(USB_SERIAL_UART_PERIPHCLK) + p_line_coding->bit_rate/2) / p_line_coding->bit_rate;

    if (p_line_coding->data_bits == 8) {
    } else {
        /* Support only 8 bit character size */
        TU_ASSERT(0, /**/);
    }

    if (p_line_coding->parity == 0) {
        /* No parity */
        USB_SERIAL_UART->CR1 = (USB_SERIAL_UART->CR1 & (uint32_t) ~(USART_CR1_PCE | USART_CR1_PS | USART_CR1_M | USART_CR1_M0))
                | UART_PARITY_NONE;
    } else if (p_line_coding->parity == 1) {
        /* Odd parity */
        USB_SERIAL_UART->CR1 = (USB_SERIAL_UART->CR1 & (uint32_t) ~(USART_CR1_PCE | USART_CR1_PS | USART_CR1_M | USART_CR1_M0))
                | UART_PARITY_ODD | UART_WORDLENGTH_9B;
    } else if (p_line_coding->parity == 2) {
        /* Even parity */
        USB_SERIAL_UART->CR1 = (USB_SERIAL_UART->CR1 & (uint32_t) ~(USART_CR1_PCE | USART_CR1_PS | USART_CR1_M | USART_CR1_M0))
                | UART_PARITY_EVEN | UART_WORDLENGTH_9B;
    } else {
        /* Other parity modes are not supported */
        TU_ASSERT(0, /**/);
    }

    if (p_line_coding->stop_bits == 0) {
        /* 1 stop bit */
        USB_SERIAL_UART->CR2 = (USB_SERIAL_UART->CR2 & (uint32_t) ~USART_CR2_STOP) | UART_STOPBITS_1;
    } else if (p_line_coding->stop_bits == 1) {
        /* 1.5 stop bit */
        USB_SERIAL_UART->CR2 = (USB_SERIAL_UART->CR2 & (uint32_t) ~USART_CR2_STOP) | UART_STOPBITS_1_5;
    } else if (p_line_coding->stop_bits == 2) {
        /* 2 stop bit */
        USB_SERIAL_UART->CR2 = (USB_SERIAL_UART->CR2 & (uint32_t) ~USART_CR2_STOP) | UART_STOPBITS_2;
    } else {
        /* Other stop bits unsupported */
        TU_ASSERT(0, /**/);
    }

    /* Re-enable UART */
    USB_SERIAL_UART->CR1 |= USART_CR1_UE;
    __enable_irq();
}

// Invoked when cdc when line state changed e.g connected/disconnected
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    TU_ASSERT(itf == 0, /**/);

    uint8_t pttMask = PTT_MASK_NONE;

    if (settingsRegMap[SETTINGS_REG_PTT1] & SETTINGS_REG_PTT1_SRC_SERIALDTR_MASK) {
        pttMask |= dtr ? PTT_MASK_PTT1 : 0;
    }

    if (settingsRegMap[SETTINGS_REG_PTT1] & SETTINGS_REG_PTT1_SRC_SERIALRTS_MASK) {
        pttMask |= rts ? PTT_MASK_PTT1 : 0;
    }

    if (settingsRegMap[SETTINGS_REG_PTT1] & SETTINGS_REG_PTT1_SRC_SERIALDTRNRTS_MASK) {
        pttMask |= (dtr && !rts) ? PTT_MASK_PTT1 : 0;
    }

    if (settingsRegMap[SETTINGS_REG_PTT1] & SETTINGS_REG_PTT1_SRC_SERIALNDTRRTS_MASK) {
        pttMask |= (!dtr && rts) ? PTT_MASK_PTT1 : 0;
    }

    if (settingsRegMap[SETTINGS_REG_PTT2] & SETTINGS_REG_PTT2_SRC_SERIALDTR_MASK) {
        pttMask |= dtr ? PTT_MASK_PTT2 : 0;
    }

    if (settingsRegMap[SETTINGS_REG_PTT2] & SETTINGS_REG_PTT2_SRC_SERIALRTS_MASK) {
        pttMask |= rts ? PTT_MASK_PTT2 : 0;
    }

    if (settingsRegMap[SETTINGS_REG_PTT2] & SETTINGS_REG_PTT2_SRC_SERIALDTRNRTS_MASK) {
        pttMask |= (dtr && !rts) ? PTT_MASK_PTT2 : 0;
    }

    if (settingsRegMap[SETTINGS_REG_PTT2] & SETTINGS_REG_PTT2_SRC_SERIALNDTRRTS_MASK) {
        pttMask |= (!dtr && rts) ? PTT_MASK_PTT2 : 0;
    }

    if (! (USB_SERIAL_UART->CR1 & USART_CR1_TE) ) {
        /* Enable PTT only when UART transmitter is not currently transmitting */
        PTT_Control(pttMask);
    }
}

void USB_SerialInit(void)
{
    /* Set up GPIO */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitTypeDef SerialGpio;
    SerialGpio.Pin = (USB_SERIAL_UART_PIN_TX | USB_SERIAL_UART_PIN_RX);
    SerialGpio.Mode = GPIO_MODE_AF_PP;
    SerialGpio.Pull = GPIO_PULLUP;
    SerialGpio.Speed = GPIO_SPEED_FREQ_LOW;
    SerialGpio.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(USB_SERIAL_UART_GPIO, &SerialGpio);

    /* Errata 2.11.5 When PCLK is selected as clock source for USART1, PCLK1 is used instead of PCLK2.
     *  To reach 9 Mbaud, System Clock (SYSCLK) should be selected as USART1 clock source. */
    RCC_PeriphCLKInitTypeDef PeriphClk = {
        .PeriphClockSelection = RCC_PERIPHCLK_USART1,
        .Usart1ClockSelection = RCC_USART1CLKSOURCE_SYSCLK
    };

    HAL_StatusTypeDef status = HAL_RCCEx_PeriphCLKConfig(&PeriphClk);
    TU_ASSERT(status == HAL_OK, /**/);

    /* Initialize UART */
    __HAL_RCC_USART1_CLK_ENABLE();
    USB_SERIAL_UART->CR1 = USART_CR1_RTOIE | UART_OVERSAMPLING_16 | UART_WORDLENGTH_8B
            | UART_PARITY_NONE | USART_CR1_RXNEIE | UART_MODE_RX; /* Enable receiver only, transmitter will be enabled on-demand */
    USB_SERIAL_UART->CR2 = UART_RECEIVER_TIMEOUT_ENABLE | UART_STOPBITS_1;
    USB_SERIAL_UART->CR3 = USART_CR3_EIE;
    USB_SERIAL_UART->BRR = (HAL_RCCEx_GetPeriphCLKFreq(USB_SERIAL_UART_PERIPHCLK) + USB_SERIAL_UART_DEFBAUD/2) / USB_SERIAL_UART_DEFBAUD;
    USB_SERIAL_UART->RTOR = ((uint32_t) USB_SERIAL_UART_RXTIMEOUT << USART_RTOR_RTO_Pos) & USART_RTOR_RTO_Msk;
    USB_SERIAL_UART->CR1 |= USART_CR1_UE;

    /* Enable interrupt */
    NVIC_SetPriority(USART1_IRQn, AIOC_IRQ_PRIO_SERIAL);
    NVIC_EnableIRQ(USART1_IRQn);
}

void USB_SerialTask(void)
{

}

#include "device/usbd_pvt.h"

bool USB_SerialSendLineState(uint8_t lineState)
{
#if 0
    /* TODO: This causes issues with the stack if its being called before CDC was initialized? Crashes on terminal connect */
    uint8_t const rhport = 0;
    static uint8_t notification[10] = {
        /* bmRequestType */ 0xA1,
        /* bNotification */ 0x20,
        /* wValue */ 0x00, 0x00,
        /* wIndex */ ITF_NUM_CDC_0, 0x00,
        /* wLength */ 0x02, 0x00,
        /* Data */ 0x00, 0x00
    };

    notification[8] = lineState;
    notification[9] = 0x00;

    // claim endpoint
    TU_VERIFY( usbd_edpt_claim(rhport, EPNUM_CDC_0_NOTIF) );

    return usbd_edpt_xfer(rhport, EPNUM_CDC_0_NOTIF, notification, sizeof(notification));
#else
    return true;
#endif
}

