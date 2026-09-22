/**
  ******************************************************************************
  * @file    spi_bus.h
  * @brief   Minimal bare-register SPI1 master driver shared by the DAC80508
  *          and the MCP23S17 (chip-selects are driven by their drivers).
  *
  *  The two slaves need different clock phases — DAC80508 latches SDIN on the
  *  falling SCLK edge (mode 1), MCP23S17 only supports modes 0 and 3 — so the
  *  mode is selected per transaction with spi_bus_set_mode(). Otherwise:
  *    - Master, 8-bit, MSB first
  *    - ~2.6 MHz (PCLK2 / 32) — conservative, tolerant of the digital isolators
  *
  *  Pins (AF5):
  *    - PA5  SPI1_SCK
  *    - PA6  SPI1_MISO
  *    - PB5  SPI1_MOSI
  *
  *  The driver uses direct register access to avoid pulling in the STM32 HAL
  *  SPI module (kept consistent with temp_module's bare-register approach).
  ******************************************************************************
  */
#ifndef APPLICATION_SPI_BUS_H
#define APPLICATION_SPI_BUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SPI_BUS_MODE0 = 0,   /* CPOL 0, CPHA 0 — MCP23S17          */
    SPI_BUS_MODE1 = 1,   /* CPOL 0, CPHA 1 — DAC80508          */
    SPI_BUS_MODE2 = 2,
    SPI_BUS_MODE3 = 3,
} spi_bus_mode_t;

/** Configure the SPI1 peripheral (mode 0) and its GPIO pins. Idempotent. */
void spi_bus_init(void);

/** Switch CPOL/CPHA. Call with chip-select released; cheap when unchanged. */
void spi_bus_set_mode(spi_bus_mode_t mode);

/**
 * Full-duplex 8-bit transfer: shift @p out on MOSI while capturing MISO.
 * Blocking, polled. Chip-select management is the caller's responsibility.
 */
uint8_t spi_bus_transfer(uint8_t out);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_SPI_BUS_H */
