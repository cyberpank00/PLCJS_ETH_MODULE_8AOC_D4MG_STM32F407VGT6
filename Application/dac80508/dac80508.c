/**
  ******************************************************************************
  * @file    dac80508.c
  * @brief   DAC80508 driver implementation (see dac80508.h).
  ******************************************************************************
  */

#include "dac80508.h"

#include "main.h"
#include "spi_bus.h"
#include "stm32f4xx_hal.h"

static inline void short_delay(uint32_t loops)
{
    for (volatile uint32_t i = 0; i < loops; i++) { __NOP(); }
}

static inline void cs_assert(void)
{
    spi_bus_set_mode(SPI_BUS_MODE1);
    HAL_GPIO_WritePin(DAC_CS_GPIO_Port, DAC_CS_Pin, GPIO_PIN_RESET);
    short_delay(8u);
}

static inline void cs_release(void)
{
    short_delay(8u);
    HAL_GPIO_WritePin(DAC_CS_GPIO_Port, DAC_CS_Pin, GPIO_PIN_SET);
    short_delay(8u);   /* t_CSH between frames */
}

/* One 24-bit frame; returns the 24 bits shifted out on SDO meanwhile. */
static uint32_t frame(uint8_t cmd, uint16_t data)
{
    cs_assert();
    const uint8_t b2 = spi_bus_transfer(cmd);
    const uint8_t b1 = spi_bus_transfer((uint8_t)(data >> 8));
    const uint8_t b0 = spi_bus_transfer((uint8_t)(data & 0xFFu));
    cs_release();
    return ((uint32_t)b2 << 16) | ((uint32_t)b1 << 8) | b0;
}

static void write_reg(uint8_t reg, uint16_t data)
{
    (void)frame((uint8_t)(reg & 0x0Fu), data);
}

uint16_t dac80508_read_reg(uint8_t reg)
{
    (void)frame((uint8_t)(0x80u | (reg & 0x0Fu)), 0u);          /* read command */
    const uint32_t echo = frame(DAC80508_REG_NOP, 0u);          /* fetch answer */
    return (uint16_t)(echo & 0xFFFFu);
}

bool dac80508_alive(void)
{
    return dac80508_read_reg(DAC80508_REG_GAIN) == DAC80508_GAIN_X1_ALL;
}

bool dac80508_init(void)
{
    spi_bus_init();

    write_reg(DAC80508_REG_TRIGGER, DAC80508_TRIGGER_SOFT_RESET);
    short_delay(20000u);   /* > 50 us for the reset to complete */

    write_reg(DAC80508_REG_CONFIG, DAC80508_CONFIG_DEFAULT);   /* SDO on, all DACs powered */
    write_reg(DAC80508_REG_GAIN,   DAC80508_GAIN_X1_ALL);      /* 0..2.5 V span */
    write_reg(DAC80508_REG_SYNC,   0x0000u);                   /* asynchronous update */
    write_reg(DAC80508_REG_BRDCAST, 0u);                       /* all outputs at 0 */

    return dac80508_alive();
}

void dac80508_set_code(uint8_t ch, uint16_t code)
{
    if (ch >= DAC80508_CHANNEL_COUNT) {
        return;
    }
    write_reg((uint8_t)(DAC80508_REG_DAC0 + ch), code);
}

void dac80508_set_all(uint16_t code)
{
    write_reg(DAC80508_REG_BRDCAST, code);
}
