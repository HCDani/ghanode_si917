#ifndef PIN_CONFIG_H
#define PIN_CONFIG_H

// $[USART0]
// USART0 TX on GPIO_30
#ifndef USART0_TX_PORT                          
#define USART0_TX_PORT                           HP
#endif
#ifndef USART0_TX_PIN                           
#define USART0_TX_PIN                            30
#endif
#ifndef USART0_TX_LOC                           
#define USART0_TX_LOC                            5
#endif

// USART0 RX on GPIO_29
#ifndef USART0_RX_PORT                          
#define USART0_RX_PORT                           HP
#endif
#ifndef USART0_RX_PIN                           
#define USART0_RX_PIN                            29
#endif
#ifndef USART0_RX_LOC                           
#define USART0_RX_LOC                            10
#endif

// [USART0]$

// $[UART1]
// [UART1]$

// $[ULP_UART]
// ULP_UART TX on ULP_GPIO_11/GPIO_75
#ifndef ULP_UART_TX_PORT                        
#define ULP_UART_TX_PORT                         ULP
#endif
#ifndef ULP_UART_TX_PIN                         
#define ULP_UART_TX_PIN                          11
#endif
#ifndef ULP_UART_TX_LOC                         
#define ULP_UART_TX_LOC                          1
#endif

// ULP_UART RX on ULP_GPIO_9/GPIO_73
#ifndef ULP_UART_RX_PORT                        
#define ULP_UART_RX_PORT                         ULP
#endif
#ifndef ULP_UART_RX_PIN                         
#define ULP_UART_RX_PIN                          9
#endif
#ifndef ULP_UART_RX_LOC                         
#define ULP_UART_RX_LOC                          3
#endif

// [ULP_UART]$

// $[I2C0]
// [I2C0]$

// $[I2C1]
// [I2C1]$

// $[ULP_I2C]
// ULP_I2C SCL on ULP_GPIO_7/GPIO_71
#ifndef ULP_I2C_SCL_PORT                        
#define ULP_I2C_SCL_PORT                         ULP
#endif
#ifndef ULP_I2C_SCL_PIN                         
#define ULP_I2C_SCL_PIN                          7
#endif
#ifndef ULP_I2C_SCL_LOC                         
#define ULP_I2C_SCL_LOC                          2
#endif

// ULP_I2C SDA on ULP_GPIO_6/GPIO_70
#ifndef ULP_I2C_SDA_PORT                        
#define ULP_I2C_SDA_PORT                         ULP
#endif
#ifndef ULP_I2C_SDA_PIN                         
#define ULP_I2C_SDA_PIN                          6
#endif
#ifndef ULP_I2C_SDA_LOC                         
#define ULP_I2C_SDA_LOC                          6
#endif

// [ULP_I2C]$

// $[SSI_MASTER]
// [SSI_MASTER]$

// $[SSI_SLAVE]
// [SSI_SLAVE]$

// $[ULP_SSI]
// [ULP_SSI]$

// $[GSPI_MASTER]
// [GSPI_MASTER]$

// $[I2S0]
// [I2S0]$

// $[ULP_I2S]
// [ULP_I2S]$

// $[SCT]
// [SCT]$

// $[SIO]
// [SIO]$

// $[PWM]
// [PWM]$

// $[PWM_CH0]
// [PWM_CH0]$

// $[PWM_CH1]
// [PWM_CH1]$

// $[PWM_CH2]
// [PWM_CH2]$

// $[PWM_CH3]
// [PWM_CH3]$

// $[ADC_CH1]
// ADC_CH1 P on GPIO_25
#ifndef ADC_CH1_P_PORT                          
#define ADC_CH1_P_PORT                           HP
#endif
#ifndef ADC_CH1_P_PIN                           
#define ADC_CH1_P_PIN                            25
#endif
#ifndef ADC_CH1_P_LOC                           
#define ADC_CH1_P_LOC                            6
#endif

// [ADC_CH1]$

// $[ADC_CH2]
// [ADC_CH2]$

// $[ADC_CH3]
// [ADC_CH3]$

// $[ADC_CH4]
// [ADC_CH4]$

// $[ADC_CH5]
// [ADC_CH5]$

// $[ADC_CH6]
// [ADC_CH6]$

// $[ADC_CH7]
// [ADC_CH7]$

// $[ADC_CH8]
// [ADC_CH8]$

// $[ADC_CH9]
// [ADC_CH9]$

// $[ADC_CH10]
// [ADC_CH10]$

// $[ADC_CH11]
// [ADC_CH11]$

// $[ADC_CH12]
// [ADC_CH12]$

// $[ADC_CH13]
// [ADC_CH13]$

// $[ADC_CH14]
// [ADC_CH14]$

// $[ADC_CH15]
// [ADC_CH15]$

// $[ADC_CH16]
// [ADC_CH16]$

// $[ADC_CH17]
// [ADC_CH17]$

// $[ADC_CH18]
// [ADC_CH18]$

// $[ADC_CH19]
// [ADC_CH19]$

// $[COMP1]
// [COMP1]$

// $[COMP2]
// [COMP2]$

// $[DAC0]
// [DAC0]$

// $[DAC1]
// [DAC1]$

// $[SYSRTC]
// [SYSRTC]$

// $[UULP_VBAT_GPIO]
// [UULP_VBAT_GPIO]$

// $[GPIO]
// [GPIO]$

// $[QEI]
// [QEI]$

// $[SDIO]
// [SDIO]$

// $[HSPI_SECONDARY]
// [HSPI_SECONDARY]$

// $[OPAMP1]
// [OPAMP1]$

// $[OPAMP2]
// [OPAMP2]$

// $[OPAMP3]
// [OPAMP3]$

// $[SDC_CH1]
// [SDC_CH1]$

// $[SDC_CH2]
// [SDC_CH2]$

// $[SDC_CH3]
// [SDC_CH3]$

// $[SDC_CH4]
// [SDC_CH4]$

// $[MCU_CLK_OUT]
// [MCU_CLK_OUT]$

// $[CUSTOM_PIN_NAME]
#ifndef _PORT                                   
#define _PORT                                    HP
#endif
#ifndef _PIN                                    
#define _PIN                                     6
#endif














































// [CUSTOM_PIN_NAME]$

#endif // PIN_CONFIG_H

