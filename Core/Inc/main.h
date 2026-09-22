/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define ETHINT_Pin GPIO_PIN_1
#define ETHINT_GPIO_Port GPIOB
#define ETHRST_Pin GPIO_PIN_11
#define ETHRST_GPIO_Port GPIOD
#define STAT_LED_Pin GPIO_PIN_9
#define STAT_LED_GPIO_Port GPIOE
#define FACT_RES_Pin GPIO_PIN_10
#define FACT_RES_GPIO_Port GPIOE

/* ---- 8AOC analog board (DAC80508 + 8x XTR111 + MCP23S17) ---------------- */
/* SPI1: SCLK = PA5, MISO = PA6, MOSI = PB5 (all AF5). Configured in spi_bus.c.
 * Both SPI slaves sit behind the digital isolators on the loop-powered side. */

/* Chip-selects (active-low, idle high). */
#define DAC_CS_Pin          GPIO_PIN_12
#define DAC_CS_GPIO_Port    GPIOA
#define MCP_CS_Pin          GPIO_PIN_8
#define MCP_CS_GPIO_Port    GPIOD

/* Isolated 24 V loop supply (URB2424S-6WR3) enable, via a PNP inverter to the
 * converter's CTRL pin. The DAC and the GPIO expander are powered from this
 * rail too, so it must be on before they are configured. */
#define PVD_CTRL_Pin        GPIO_PIN_7
#define PVD_CTRL_GPIO_Port  GPIOB
/* MCU level that turns the loop supply ON. PB7 drives the base of a PNP
 * (BC858) through 2k2; the converter's CTRL pin is pulled to the "off" state
 * by 10k otherwise, so the PNP must conduct (base LOW) to enable. If the rail
 * turns out inverted on hardware, flip these two lines only. */
#define PVD_CTRL_ON_LEVEL   GPIO_PIN_RESET
#define PVD_CTRL_OFF_LEVEL  GPIO_PIN_SET

/* Per-channel status LED (active-high). */
#define AO0_STAT_Pin        GPIO_PIN_15
#define AO0_STAT_GPIO_Port  GPIOB
#define AO1_STAT_Pin        GPIO_PIN_14
#define AO1_STAT_GPIO_Port  GPIOB
#define AO2_STAT_Pin        GPIO_PIN_10
#define AO2_STAT_GPIO_Port  GPIOB
#define AO3_STAT_Pin        GPIO_PIN_15
#define AO3_STAT_GPIO_Port  GPIOE
#define AO4_STAT_Pin        GPIO_PIN_14
#define AO4_STAT_GPIO_Port  GPIOE
#define AO5_STAT_Pin        GPIO_PIN_13
#define AO5_STAT_GPIO_Port  GPIOE
#define AO6_STAT_Pin        GPIO_PIN_12
#define AO6_STAT_GPIO_Port  GPIOE
#define AO7_STAT_Pin        GPIO_PIN_11
#define AO7_STAT_GPIO_Port  GPIOE

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
