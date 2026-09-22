/**
  ******************************************************************************
  * @file    spi_bus.c
  * @brief   Bare-register SPI1 master driver (see spi_bus.h).
  ******************************************************************************
  */

#include "spi_bus.h"

#include "stm32f4xx_hal.h"

static volatile uint8_t s_initialised = 0u;
static spi_bus_mode_t   s_mode        = SPI_BUS_MODE0;

static void apply_mode(spi_bus_mode_t mode)
{
    uint32_t cr1 = SPI1->CR1 & ~(SPI_CR1_SPE | SPI_CR1_CPOL | SPI_CR1_CPHA);
    if (mode & 2u) { cr1 |= SPI_CR1_CPOL; }
    if (mode & 1u) { cr1 |= SPI_CR1_CPHA; }
    SPI1->CR1 = cr1;                   /* disabled while the phase changes */
    SPI1->CR1 = cr1 | SPI_CR1_SPE;
    s_mode = mode;
}

void spi_bus_init(void)
{
    if (s_initialised) {
        return;
    }

    /* Peripheral + GPIO clocks. */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_SPI1_CLK_ENABLE();

    /* --- GPIO alternate-function setup (AF5 = SPI1) --------------------- */
    /* PA5 SCK, PA6 MISO -> AF mode, AF5, very-high speed. */
    GPIOA->MODER   &= ~((3u << (5u * 2u)) | (3u << (6u * 2u)));
    GPIOA->MODER   |=  ((2u << (5u * 2u)) | (2u << (6u * 2u)));   /* AF */
    GPIOA->OSPEEDR |=  ((3u << (5u * 2u)) | (3u << (6u * 2u)));
    GPIOA->AFR[0]  &= ~((0xFu << (5u * 4u)) | (0xFu << (6u * 4u)));
    GPIOA->AFR[0]  |=  ((5u << (5u * 4u)) | (5u << (6u * 4u)));

    /* PB5 MOSI -> AF mode, AF5, very-high speed. */
    GPIOB->MODER   &= ~(3u << (5u * 2u));
    GPIOB->MODER   |=  (2u << (5u * 2u));
    GPIOB->OSPEEDR |=  (3u << (5u * 2u));
    GPIOB->AFR[0]  &= ~(0xFu << (5u * 4u));
    GPIOB->AFR[0]  |=  (5u << (5u * 4u));

    /* --- SPI1 configuration -------------------------------------------- */
    SPI1->CR1 = 0u;
    SPI1->CR1 =  SPI_CR1_MSTR          /* master                            */
               | (4u << SPI_CR1_BR_Pos)/* PCLK2 / 32 (~2.6 MHz)             */
               | SPI_CR1_SSM           /* software slave management         */
               | SPI_CR1_SSI;          /* internal NSS high (master)        */
    SPI1->CR2 = 0u;
    apply_mode(SPI_BUS_MODE0);

    s_initialised = 1u;
}

void spi_bus_set_mode(spi_bus_mode_t mode)
{
    if (mode == s_mode) {
        return;
    }
    while (SPI1->SR & SPI_SR_BSY) { }  /* never true with CS released, be safe */
    apply_mode(mode);
}

uint8_t spi_bus_transfer(uint8_t out)
{
    /* 8-bit access to DR so the peripheral clocks out exactly one byte. */
    while ((SPI1->SR & SPI_SR_TXE) == 0u) { }
    *(volatile uint8_t*)&SPI1->DR = out;

    while ((SPI1->SR & SPI_SR_RXNE) == 0u) { }
    return *(volatile uint8_t*)&SPI1->DR;
}
