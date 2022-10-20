#ifndef USB_SERIAL_H_
#define USB_SERIAL_H_

#define USB_SERIAL_UART             USART1
#define USB_SERIAL_UART_IRQ         USART1_IRQHandler
#define USB_SERIAL_UART_PERIPHCLK   RCC_PERIPHCLK_USART1
#define USB_SERIAL_UART_RXTIMEOUT   20 /* in number of bits */
#define USB_SERIAL_UART_DEFBAUD     9600
#define USB_SERIAL_UART_GPIO_PTT    GPIOA
#define USB_SERIAL_UART_PIN_PTT1    GPIO_PIN_1
#define USB_SERIAL_UART_PIN_PTT2    GPIO_PIN_0

void USB_SerialInit(void);
void USB_SerialTask(void);

#endif /* USB_SERIAL_H_ */
