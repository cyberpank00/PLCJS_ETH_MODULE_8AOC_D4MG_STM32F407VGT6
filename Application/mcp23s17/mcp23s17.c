/**
  ******************************************************************************
  * @file    mcp23s17.c
  * @brief   MCP23S17 driver implementation (see mcp23s17.h).
  ******************************************************************************
  */

#include "mcp23s17.h"

#include <stddef.h>

#include "main.h"
#include "spi_bus.h"
#include "stm32f4xx_hal.h"

/* Opcode: 0100 A2 A1 A0 R/W — hardware address 0. */
#define MCP_OPCODE_WRITE    0x40u
#define MCP_OPCODE_READ     0x41u

/* Per-channel location of the EF / ON lines: port (0 = A, 1 = B) and bit. */
typedef struct { uint8_t port; uint8_t ef_bit; uint8_t on_bit; } pinmap_t;

static const pinmap_t s_map[MCP23S17_CHANNEL_COUNT] = {
    { 1, 7, 6 },   /* AO0: GPB7 EF, GPB6 ON */
    { 1, 5, 4 },   /* AO1: GPB5 EF, GPB4 ON */
    { 1, 3, 2 },   /* AO2: GPB3 EF, GPB2 ON */
    { 0, 0, 1 },   /* AO3: GPA0 EF, GPA1 ON */
    { 1, 1, 0 },   /* AO4: GPB1 EF, GPB0 ON */
    { 0, 2, 3 },   /* AO5: GPA2 EF, GPA3 ON */
    { 0, 4, 5 },   /* AO6: GPA4 EF, GPA5 ON */
    { 0, 6, 7 },   /* AO7: GPA6 EF, GPA7 ON */
};

/* Direction masks derived from the map: EF bits are inputs (1). */
static uint8_t s_iodir[2];
static uint8_t s_on_mask;   /* channel space */

static inline void short_delay(uint32_t loops)
{
    for (volatile uint32_t i = 0; i < loops; i++) { __NOP(); }
}

static inline void cs_assert(void)
{
    spi_bus_set_mode(SPI_BUS_MODE0);
    HAL_GPIO_WritePin(MCP_CS_GPIO_Port, MCP_CS_Pin, GPIO_PIN_RESET);
    short_delay(8u);
}

static inline void cs_release(void)
{
    short_delay(8u);
    HAL_GPIO_WritePin(MCP_CS_GPIO_Port, MCP_CS_Pin, GPIO_PIN_SET);
    short_delay(8u);
}

/* Sequential addressing (SEQOP = 0, default) — consecutive bytes land in
 * consecutive registers, so A and B of a pair go out in one frame. */
static void write_pair(uint8_t reg_a, uint8_t a, uint8_t b)
{
    cs_assert();
    (void)spi_bus_transfer(MCP_OPCODE_WRITE);
    (void)spi_bus_transfer(reg_a);
    (void)spi_bus_transfer(a);
    (void)spi_bus_transfer(b);
    cs_release();
}

static void read_pair(uint8_t reg_a, uint8_t* a, uint8_t* b)
{
    cs_assert();
    (void)spi_bus_transfer(MCP_OPCODE_READ);
    (void)spi_bus_transfer(reg_a);
    *a = spi_bus_transfer(0x00u);
    *b = spi_bus_transfer(0x00u);
    cs_release();
}

/* Channel-space ON mask -> port latch bytes. */
static void on_mask_to_ports(uint8_t on_mask, uint8_t out[2])
{
    out[0] = 0u; out[1] = 0u;
    for (uint8_t ch = 0; ch < MCP23S17_CHANNEL_COUNT; ch++) {
        const bool on = ((on_mask >> ch) & 1u) != 0u;
#if MCP23S17_ON_ACTIVE_HIGH
        if (on)  { out[s_map[ch].port] |= (uint8_t)(1u << s_map[ch].on_bit); }
#else
        if (!on) { out[s_map[ch].port] |= (uint8_t)(1u << s_map[ch].on_bit); }
#endif
    }
}

bool mcp23s17_alive(void)
{
    uint8_t a, b;
    read_pair(MCP23S17_REG_IODIRA, &a, &b);
    return a == s_iodir[0] && b == s_iodir[1];
}

bool mcp23s17_init(void)
{
    spi_bus_init();

    s_iodir[0] = 0u; s_iodir[1] = 0u;
    for (uint8_t ch = 0; ch < MCP23S17_CHANNEL_COUNT; ch++) {
        s_iodir[s_map[ch].port] |= (uint8_t)(1u << s_map[ch].ef_bit);
    }

    /* IOCON: BANK 0, SEQOP 0, HAEN 0 — power-on defaults, written explicitly
     * so a warm restart of the isolated rail always lands in a known state. */
    write_pair(MCP23S17_REG_IOCON, 0x00u, 0x00u);

    /* Outputs OFF before the pins become outputs, then directions + pull-ups
     * on the EF inputs (redundant with the board's 10 k, harmless). */
    s_on_mask = 0u;
    uint8_t lat[2];
    on_mask_to_ports(0u, lat);
    write_pair(MCP23S17_REG_OLATA, lat[0], lat[1]);
    write_pair(MCP23S17_REG_IODIRA, s_iodir[0], s_iodir[1]);
    write_pair(MCP23S17_REG_GPPUA,  s_iodir[0], s_iodir[1]);

    return mcp23s17_alive();
}

void mcp23s17_set_on_mask(uint8_t on_mask)
{
    uint8_t lat[2];
    s_on_mask = on_mask;
    on_mask_to_ports(on_mask, lat);
    write_pair(MCP23S17_REG_OLATA, lat[0], lat[1]);
}

uint8_t mcp23s17_get_on_mask(void)
{
    return s_on_mask;
}

bool mcp23s17_read_faults(uint8_t* fault_mask_out)
{
    uint8_t gp[2];
    read_pair(MCP23S17_REG_GPIOA, &gp[0], &gp[1]);

    uint8_t m = 0u;
    for (uint8_t ch = 0; ch < MCP23S17_CHANNEL_COUNT; ch++) {
        if (((gp[s_map[ch].port] >> s_map[ch].ef_bit) & 1u) == 0u) {   /* EF low = fault */
            m |= (uint8_t)(1u << ch);
        }
    }

    if (!mcp23s17_alive()) {
        if (fault_mask_out != NULL) { *fault_mask_out = 0xFFu; }
        return false;
    }
    if (fault_mask_out != NULL) { *fault_mask_out = m; }
    return true;
}
