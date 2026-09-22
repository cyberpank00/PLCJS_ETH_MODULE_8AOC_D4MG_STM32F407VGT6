/**
  ******************************************************************************
  * @file    dac80508.h
  * @brief   Driver for the TI DAC80508 (8-channel, 16-bit, internal 2.5 V
  *          reference) that drives the eight XTR111 current transmitters.
  *
  *  Wiring on the 8AOC board: the DAC sits on the isolated, loop-powered side
  *  (3V3_DAC from the URB2424S rail), SPI1 through the CA-IS37xx isolators,
  *  CS = DAC_CS. OUTn feeds the SET input of XTR111 channel n; with
  *  R_SET = 1.1 kΩ the transmitter sources  I = 10 · V_OUT / R_SET, so the
  *  0…2.5 V DAC span maps onto 0…22.7 mA.
  *
  *  Configuration used: internal reference on, REF-DIV /1, buffer gain ×1 on
  *  every channel (0…2.5 V), asynchronous update (each DAC write takes effect
  *  immediately), SDO enabled for register readback, CRC off.
  *
  *  SPI frames are 24 bits, MSB first, CS low for the whole frame, data latched
  *  on the falling SCLK edge (SPI mode 1):
  *      [23] R/W (1 = read)  [22:20] 0  [19:16] register  [15:0] data
  *  A read returns the register contents in the frame that FOLLOWS the read
  *  command (clock a NOP to fetch it).
  ******************************************************************************
  */
#ifndef APPLICATION_DAC80508_H
#define APPLICATION_DAC80508_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DAC80508_CHANNEL_COUNT      8u
#define DAC80508_CODE_MAX           65535u

/* Register addresses. */
#define DAC80508_REG_NOP            0x00u
#define DAC80508_REG_DEVICE_ID      0x01u
#define DAC80508_REG_SYNC           0x02u
#define DAC80508_REG_CONFIG         0x03u
#define DAC80508_REG_GAIN           0x04u
#define DAC80508_REG_TRIGGER        0x05u
#define DAC80508_REG_BRDCAST        0x06u
#define DAC80508_REG_STATUS         0x07u
#define DAC80508_REG_DAC0           0x08u   /* DACn = 0x08 + n */

/* CONFIG: [10] ALM-SEL [9] ALM-EN [8] CRC-EN [7:0] DAC-PDN (1 = power down). */
#define DAC80508_CONFIG_DEFAULT     0x0000u
/* GAIN: [8] REF-DIV (1 = /2) [7:0] BUFF-GAIN (1 = ×2). ×1 everywhere. */
#define DAC80508_GAIN_X1_ALL        0x0000u
/* TRIGGER: [4] LDAC, [3:0] SOFT-RESET = 0b1010. */
#define DAC80508_TRIGGER_SOFT_RESET 0x000Au

/** Bring the SPI bus up and configure the DAC (soft reset, CONFIG, GAIN).
 *  The isolated rail must already be up. Returns false if the readback of the
 *  GAIN register does not match — device absent or unpowered. */
bool dac80508_init(void);

/** Write one channel code (0..65535); takes effect immediately. */
void dac80508_set_code(uint8_t ch, uint16_t code);

/** Write the same code to all eight channels in one frame (BRDCAST). */
void dac80508_set_all(uint16_t code);

/** Read back a register; used for liveness checks and diagnostics. */
uint16_t dac80508_read_reg(uint8_t reg);

/** True if the device answers (GAIN readback matches the configured value). */
bool dac80508_alive(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_DAC80508_H */
