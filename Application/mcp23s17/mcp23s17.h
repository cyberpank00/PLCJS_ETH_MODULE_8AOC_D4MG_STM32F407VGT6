/**
  ******************************************************************************
  * @file    mcp23s17.h
  * @brief   Driver for the MCP23S17 SPI GPIO expander that carries the
  *          per-channel ON (output enable) and EF (error flag) lines of the
  *          eight XTR111 current transmitters.
  *
  *  The expander lives on the isolated, loop-powered rail (SPI1 through the
  *  isolators, CS = MCP_CS, hardware address 0, HAEN off). SPI mode 0.
  *
  *  Pin assignment (board):
  *      GPA0 AO3_EF  GPA1 AO3_ON  GPA2 AO5_EF  GPA3 AO5_ON
  *      GPA4 AO6_EF  GPA5 AO6_ON  GPA6 AO7_EF  GPA7 AO7_ON
  *      GPB0 AO4_ON  GPB1 AO4_EF  GPB2 AO2_ON  GPB3 AO2_EF
  *      GPB4 AO1_ON  GPB5 AO1_EF  GPB6 AO0_ON  GPB7 AO0_EF
  *
  *  EF is the XTR111 open-drain error flag (10 k pull-up): LOW = fault (open
  *  loop / output out of compliance / over-temperature). ON drives the
  *  channel's output-disable network through a P-MOSFET; the active level is
  *  MCP23S17_ON_ACTIVE_HIGH (flip on hardware if inverted).
  *
  *  The public API works in *channel* space (bit n = AOn); the port/bit
  *  scramble above is handled internally.
  ******************************************************************************
  */
#ifndef APPLICATION_MCP23S17_H
#define APPLICATION_MCP23S17_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MCP23S17_CHANNEL_COUNT      8u

/* 1: driving the ON line high enables the XTR111 output. */
#define MCP23S17_ON_ACTIVE_HIGH     1

/* Register addresses (IOCON.BANK = 0, interleaved A/B). */
#define MCP23S17_REG_IODIRA         0x00u
#define MCP23S17_REG_IODIRB         0x01u
#define MCP23S17_REG_GPPUA          0x0Cu
#define MCP23S17_REG_GPPUB          0x0Du
#define MCP23S17_REG_IOCON          0x0Au
#define MCP23S17_REG_GPIOA          0x12u
#define MCP23S17_REG_GPIOB          0x13u
#define MCP23S17_REG_OLATA          0x14u
#define MCP23S17_REG_OLATB          0x15u

/** Bring the SPI bus up and configure directions / pull-ups; all channels
 *  OFF. The isolated rail must already be up. Returns false if the IODIR
 *  readback does not match (device absent or unpowered). */
bool mcp23s17_init(void);

/** Drive the ON lines: bit n set = channel n enabled. */
void mcp23s17_set_on_mask(uint8_t on_mask);

/** Currently commanded ON mask (channel space). */
uint8_t mcp23s17_get_on_mask(void);

/**
 * Read the EF lines. @p fault_mask_out receives bit n set = channel n reports
 * a fault (EF low). Returns false if the device does not answer (IODIR
 * readback mismatch) — the mask is then all-ones.
 */
bool mcp23s17_read_faults(uint8_t* fault_mask_out);

/** True if the device answers (IODIR readback matches the configuration). */
bool mcp23s17_alive(void);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_MCP23S17_H */
